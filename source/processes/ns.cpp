#include "ns.hpp"
#include "os.hpp"

#include <loader/gamecard.hpp>
#include <processes/pxi_fs.hpp> // TODO: Get rid of this dependency..
#include <platform/file_formats/ncch.hpp>

#include "../platform/fs.hpp"
#include "../platform/ns.hpp"
#include "../platform/sm.hpp"
#include "../platform/file_formats/bcfnt.hpp"

#include "processes/pxi.hpp"

#include <framework/exceptions.hpp>

#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/endian/arithmetic.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/sliced.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm/fill.hpp>
#include <boost/scope_exit.hpp>

#include <range/v3/algorithm/equal.hpp>
#include <range/v3/algorithm/search.hpp>

#include <bitset>

#include <fstream>
#include <functional>
#include <sstream>
#include <iomanip>

namespace FS = Platform::FS;
namespace SM = Platform::SM;

namespace HLE {

namespace OS {

using Platform::NS::AppId;
using Platform::NS::AppletAttr;
using Platform::NS::AppletCommand;
using Platform::NS::AppletPos;
using Platform::PXI::PM::ProgramInfo;


template<typename RangeIn>
static uint32_t GetDecompressedLZSSDataSize(const RangeIn& compressed_reverse) {
    // Reverse the input buffer
    auto compressed = compressed_reverse | boost::adaptors::reversed;

    // Get difference between compressed and uncompressed size from the last word in the input buffer
    uint32_t size_delta = compressed[3] | (static_cast<uint32_t>(compressed[2]) << 8)
                          | (static_cast<uint32_t>(compressed[1]) << 16) | (static_cast<uint32_t>(compressed[0]) << 24);

    return boost::size(compressed) + size_delta;
}

template<typename RangeIn, typename RangeOut>
static void DecompressLZSSData(const RangeIn& compressed_reverse, const RangeOut& decompressed_reverse) {
    // Sanity checks
    if (boost::size(compressed_reverse) < 8)
        std::runtime_error("Invalid input: Smaller than 8 bytes");

    if (boost::size(decompressed_reverse) < boost::size(compressed_reverse))
        throw std::runtime_error("Decompressed buffer is smaller than compressed one!");

    // Copy the raw compressed buffer into the output buffer
    boost::fill(decompressed_reverse, 0);
    boost::copy(compressed_reverse, boost::begin(decompressed_reverse));

    // Reverse the input buffer
    auto compressed = compressed_reverse | boost::adaptors::reversed;

    const uint32_t begin = compressed[4];
    const uint32_t end = compressed[7] | (compressed[6] << 8) | (compressed[5] << 16);
    if (end > boost::size(compressed))
        throw std::runtime_error("Invalid size given");

    auto working_set = compressed | boost::adaptors::sliced(begin, end); // "begin" and "end" inclusive
    auto input_it = std::begin(working_set);

    // TODO: Working with the reversed decompressed buffer would be more readable, but I couldn't really get it working...
//    auto decompressed = decompressed_reverse | boost::adaptors::reversed;
//    auto output_it = boost::begin(decompressed);
    auto output_it = boost::end(decompressed_reverse);

    for (;;) {
        auto input_bytes_left = std::distance(input_it, boost::end(working_set));
        if (input_bytes_left <= 1)
            break;

        const auto compression_mask = *input_it++;

        for (unsigned i = 0; i < 8; ++i) {
            input_bytes_left = std::distance(input_it, boost::end(working_set));

            auto output_bytes_left = std::distance(boost::begin(decompressed_reverse), output_it);

            if (compression_mask & (0x80 >> i)) {
                // compressed section

                // TODO: Figure out if this could actually be a valid input
                if (input_bytes_left < 2) {
                    // Make sure to terminate the loop if we haven't already reached the end
                    input_it = boost::end(working_set);
                    throw std::runtime_error("Unexpected end of file");
                }

                auto offset = 2 + (((input_it[0] & 0xF) << 8) | input_it[1]);
                auto size = 3 + (input_it[0] >> 4);
                input_it += 2;

                if (output_bytes_left < size)
                    throw std::runtime_error("No output bytes left");

                // NOTE: These ranges may overlap, hence using <algorithm> may yield undefined behavior.
                // TODO: Double-check the guarantees given by std::copy/copy_backward to see whether we may use it here
                auto end = output_it + offset - size;
                for (auto temp_it = output_it + offset; temp_it != end; temp_it--)
                    *--output_it = *temp_it;
            } else {
                // uncompressed byte

                // NOTE: This is not an error; there is no explicit "EOF" indicator
                if (input_bytes_left < 1)
                    break;

                if (output_bytes_left < 1)
                    throw std::runtime_error("No output bytes left");

                *--output_it = *input_it++;
            }
        }
    }
}


HandleTable::Entry<Process> LoadProcessFromFile(FakeThread& source,
                                                bool from_firm,
                                                const FileFormat::ExHeader& exheader,
                                                std::unique_ptr<HLE::PXI::FS::File> file, bool is_exefs) {
    HLE::PXI::FS::FileContext file_context { *source.GetLogger() };
    if (!is_exefs) {
        // Read ExeFS
        uint8_t code[8] = { '.', 'c', 'o', 'd', 'e' };
        auto input_file = PXI::FS::NCCHOpenExeFSSection(*source.GetLogger(), file_context,
                                                        source.GetParentProcess().interpreter_setup.keydb,
                                                        std::move(file), 1, std::basic_string_view<uint8_t>(code, sizeof(code)));
        if (std::get<0>(input_file->OpenReadOnly(file_context)) != RESULT_OK) {
            // TODO: Better error message
            throw std::runtime_error("Could not launch title from emulated NAND.");
        }

        file = std::move(input_file);
    }
    auto& input_file = file;

    // Load process data into CodeSet based on contents of the ExeFs ".code" section.
    // The actual program binary may be compressed, so decompress it along the way.
    HandleTable::Entry<CodeSet> codeset;
source.GetLogger()->info("{}: {}", __FUNCTION__, __LINE__);
    const uint32_t page_size = 0x1000;
    uint32_t total_size_aligned = (exheader.section_text.size_pages + exheader.section_ro.size_pages + exheader.section_data.size_pages) * page_size;
    // TODO: Check if we still need the following line. It used to be necessary for loading SM, but I never figured out why!
//    auto code_buffer = parent_process.AllocateBuffer(decompressed_size + /*0x300*/0);
    auto& parent_process = source.GetParentProcess();
    auto code_buffer = parent_process.AllocateBuffer(total_size_aligned);
    auto [result, code_size] = input_file->GetSize(file_context);
    if (exheader.flags.compress_exefs_code()()) {
        auto compressed_code_buffer = parent_process.AllocateBuffer(code_size);
        uint32_t bytes_read;
        std::tie(result, bytes_read) = input_file->Read(file_context, 0, code_size, HLE::PXI::FS::FileBufferInHostMemory(compressed_code_buffer.second, code_size));
        if (result != RESULT_OK || bytes_read != code_size) {
            throw std::runtime_error("Failed to read code section from ExeFS");
        }

        // The last word in the compressed stream denotes the difference between compressed and decompressed buffer size
        auto compressed_buffer_range = boost::iterator_range<uint8_t*>(compressed_code_buffer.second, compressed_code_buffer.second + code_size);
        uint32_t decompressed_size = GetDecompressedLZSSDataSize(compressed_buffer_range);

        if (decompressed_size > total_size_aligned)
            throw std::runtime_error("Decompressed size exceeds virtual memory size. Corrupt ROM?");

        // TODO: Handle exceptions thrown by this function!
        DecompressLZSSData(compressed_buffer_range,
                           boost::iterator_range<uint8_t*>(code_buffer.second, code_buffer.second + decompressed_size));

        parent_process.FreeBuffer(compressed_code_buffer.first);
    } else {
        if (code_size > total_size_aligned) {
            throw std::runtime_error("Code section size exceeds virtual memory size. Corrupt ROM?");
        }

        uint32_t bytes_read;
        std::tie(result, bytes_read) = input_file->Read(file_context, 0, code_size, HLE::PXI::FS::FileBufferInHostMemory(code_buffer.second, code_size));
        if (result != RESULT_OK || bytes_read != code_size) {
            throw std::runtime_error("Failed to read code section from ExeFS");
        }
    }

    auto kernel_flags = Meta::invoke([&]() {
        for (auto cap : exheader.aci.arm11_kernel_capabilities) {
            auto flags = FileFormat::ExHeader::ARM11KernelCapabilityDescriptor::KernelFlags { cap.storage };
            if (flags.id_field() == flags.id_field_ref()) {
                return flags;
            }
        }
        throw Mikage::Exceptions::Invalid("ExHeader did not specify kernel flags");
    });

    uint32_t code_buffer2;
    std::tie(result, code_buffer2) = source.CallSVC(&OS::SVCControlMemory, 0, 0, total_size_aligned, 3 /* COMMIT */ | (kernel_flags.memory_type() << 8), 3 /* RW */);
    if (result != RESULT_OK) {
        throw std::runtime_error("Failed to allocate memory for program");
    }

    for (uint32_t offset = 0; offset < total_size_aligned; ++offset) {
        source.WriteMemory(code_buffer2 + offset, *(code_buffer.second + offset));
    }

    parent_process.FreeBuffer(code_buffer.first);

    // NOTE: The decompressed buffer need not be aligned to page size, hence the distinction between size_bytes and size_pages is critical!
    VAddr text_vaddr = code_buffer2;
    VAddr ro_vaddr;
    VAddr data_vaddr;
    VAddr data_vaddr_end;
    if (from_firm) {
        // Apparently, only modules loaded from FIRM take the true size in bytes (TODOTEST)
        ro_vaddr = text_vaddr + exheader.section_text.size_bytes;
        data_vaddr = ro_vaddr + exheader.section_ro.size_bytes;
        data_vaddr_end = data_vaddr + exheader.section_data.size_bytes;
    } else {
        // Modules loaded "normally" align each source section to pages.
        ro_vaddr = text_vaddr + exheader.section_text.size_pages * page_size;
        data_vaddr = ro_vaddr + exheader.section_ro.size_pages * page_size;
        data_vaddr_end = data_vaddr + exheader.section_data.size_pages * page_size;
    }
    if (data_vaddr_end - text_vaddr > total_size_aligned)
        throw std::runtime_error("Text size exceeds virtual memory size. Corrupt ROM?");

    // TODO: Chances are the "size_bytes" fields are actually page sizes!
    CodeSetInfo codesetinfo{};
    codesetinfo.text_start = exheader.section_text.address;
    codesetinfo.text_pages = (exheader.section_text.size_bytes + page_size - 1) >> 12;
    codesetinfo.text_size = exheader.section_text.size_pages;
    codesetinfo.ro_start = exheader.section_ro.address;
    codesetinfo.ro_pages = (exheader.section_ro.size_bytes + page_size - 1) >> 12;
    codesetinfo.ro_size = exheader.section_ro.size_pages;
    codesetinfo.data_start = exheader.section_data.address;

    // NOTE: The data size in the exheader is exclusive of the bss region, while the kernel expects an inclusive size.
    //       Hence, we just add the two together and round the result up to the page size.
    // TODO: Should data_pages be inclusive or exclusive of bss?
    codesetinfo.data_pages = (exheader.section_data.size_bytes + page_size - 1) >> 12;
    codesetinfo.data_size = exheader.section_data.size_pages + (exheader.bss_size + page_size - 1) / page_size;
    memcpy(codesetinfo.app_name.data(), exheader.application_title.data(), sizeof(exheader.application_title));

    std::tie(result,codeset) = source.CallSVC(&OS::SVCCreateCodeSet, codesetinfo, text_vaddr, ro_vaddr, data_vaddr);
    if (result != RESULT_OK)
        throw std::runtime_error("LaunchTitle failed to SVCCreateCodeSet");

    // Create the actual process
    HandleTable::Entry<Process> process;
    // TODO: Avoid cast...
    std::tie(result,process) = source.CallSVC(&OS::SVCCreateProcess, codeset.first, (HLE::OS::OS::KernelCapability*)exheader.aci.arm11_kernel_capabilities.data(), std::size(exheader.aci.arm11_kernel_capabilities));
    if (result != RESULT_OK)
        throw std::runtime_error("LaunchTitle failed to SVCCreateProcess");

    // Clean up
    codeset.second.reset();
    source.CallSVC(&OS::SVCCloseHandle, codeset.first);

    return process;
}


OS::ResultAnd<ProcessId> LaunchTitleInternal(FakeThread& source, bool from_firm, uint64_t title_id, uint32_t flags /* (currently unused) */) {
    // TODO: This is fairly ad-hoc currently, i.e. all logic in here is
    //       just done so that we can get any system processes up and running
    //       at all for now.
    //       Normally, this would go through filesystem, loader, and other
    //       services instead.

    // Read ExHeader

    ProgramInfo info { title_id, !title_id ? uint8_t { 2 } : uint8_t { 0 } };
    auto exheader = HLE::PXI::GetExtendedHeader(source, info);

    // Read ExeFS
    uint8_t code[8] = { '.', 'c', 'o', 'd', 'e' };
    auto input_file = PXI::FS::OpenNCCHSubFile(source, info, 0, 1, std::basic_string_view<uint8_t>(code, sizeof(code)), source.GetOS().setup.gamecard.get());
    HLE::PXI::FS::FileContext file_context { *source.GetLogger() };
    if (std::get<0>(input_file->OpenReadOnly(file_context)) != RESULT_OK) {
        throw std::runtime_error(fmt::format(   "Tried to launch non-existing title {:#x} from emulated NAND.\n\nPlease dump the title from your 3DS and install it manually to this path:\n{}",
                                                title_id, "filename" /* TODO */));
    }

    OS::Result result;
    auto process = LoadProcessFromFile(source, from_firm, exheader, std::move(input_file), true);
    auto& parent_process = source.GetParentProcess();

    ProcessId process_id;
    std::tie(result, process_id) = source.CallSVC(&OS::SVCGetProcessId, *process.second);
    if (result != RESULT_OK)
        throw std::runtime_error("LaunchTitle failed to SVCGetProcessId");

    HandleTable::Entry<ClientSession> srv_session = { HANDLE_INVALID, nullptr };
    bool needs_services = !from_firm;
    if (needs_services) {
        std::tie(result,srv_session) = source.CallSVC(&OS::SVCConnectToPort, "srv:");
        if (result != RESULT_OK)
            throw std::runtime_error("LaunchTitle failed to SVCConnectToPort");

        IPC::SendIPCRequest<SM::SRV::RegisterClient>(source, srv_session.first, IPC::EmptyValue{});
    }

    // Register the created process to srv:pm
    // TODO: Enable this code...
    // TODO: Now that we fixed SM HLE, WE REALLY NEED TO DO THIS PROPERLY INSTEAD. I.e. have loader do this, and have loader register these via pm
    auto not_null = [](auto letter) { return letter != 0; };
    auto nonempty_entry = [&](const auto& service) { return boost::algorithm::any_of(service, not_null); };
//    bool needs_services = boost::algorithm::any_of(exheader.service_access_list, nonempty_entry);
    if (false && needs_services) {
        // Register process through "srv:pm"
        // TODO: srv:pm is a service for firmwares higher than 7.0.0
//         Handle srvpm_handle = IPC::SendIPCRequest<SM::SRV::GetServiceHandle>(source, srv_session.first,
//                                                                              SM::PortName("srv:pm"), 0);
        HandleTable::Entry<ClientSession> srvpm_session;
        // TODO: On system version 7.0.0 and up, srv:pm is a service!
        std::tie(result,srvpm_session) = source.CallSVC(&OS::SVCConnectToPort, "srv:pm");
        if (result != RESULT_OK) {
            throw std::runtime_error("LaunchTitle failed to connect to srv:pm");
        }

        // srvpm RegisterProcess
        source.WriteTLS(0x80, IPC::CommandHeader::Make(0x403, 2, 2).raw);
        source.WriteTLS(0x84, process_id);
        auto service_acl_entry_size = sizeof(exheader.aci.service_access_list[0]);
        auto service_acl_size = (sizeof(exheader.aci.service_access_list) + service_acl_entry_size - 1) / service_acl_entry_size * service_acl_entry_size;
        auto service_acl_addr = parent_process.AllocateStaticBuffer(service_acl_size);
        auto service_acl_numentries = service_acl_size / service_acl_entry_size;

        for (auto off = 0; off < sizeof(exheader.aci.service_access_list); ++off) {
            auto entry = off / service_acl_entry_size;
            auto byte = off % service_acl_entry_size;
            source.WriteMemory(service_acl_addr + off, exheader.aci.service_access_list[entry][byte]);
        }
        source.WriteTLS(0x88, service_acl_numentries);
        source.WriteTLS(0x8c, IPC::TranslationDescriptor::MakeStaticBuffer(0, service_acl_size).raw);
        source.WriteTLS(0x90, service_acl_addr);
        std::tie(result) = source.CallSVC(&OS::SVCSendSyncRequest, srvpm_session.first);
        if (result != RESULT_OK && source.ReadTLS(0x84) != RESULT_OK)
            throw std::runtime_error("LaunchTitle failed to RegisterProcess to srv:pm");

        parent_process.FreeStaticBuffer(service_acl_addr);

        source.CallSVC(&OS::SVCCloseHandle, srvpm_session.first);
    }

    FS::ProgramInfo program_info{};
    program_info.program_id = exheader.aci.program_id;
    program_info.media_type = (title_id == 0) ? 2 : 0; // NAND or game card

    // TODO: SD titles may be launched through this function too, and supposedly the title id indicates whether a title is from NAND or SD...

    // Connect to fs:REG and register the new process
    // NOTE: Currently, we do this for all processes other than the FS module
    //       itself. There probably is an exheader flag that determines when to
    //       do this, though!
    // NOTE: Disabled for now, reflecting the recent changes in the boot
    //       process, due to which actual process launching will be handled in
    //       low-level emulated system modules anyway. TODO: NOT ANYMORE!
    if (!from_firm) {
        Handle fsreg_handle = IPC::SendIPCRequest<SM::SRV::GetServiceHandle>(source, srv_session.first,
                                                                             SM::PortName("fs:REG"), 0);
        source.GetLogger()->info("Registering to fs:REG with program id {:#018x}", program_info.program_id);

        // TODO: This program handle is supposed to be retrieved from
        //       PxiPM::RegisterProgram, which returns the Process9-internal
        //       handle for the registered application. Since we don't emulate
        //       PxiPM currently, we just dummy-initialize this program handle
        //       with an incorrect (but unique) value for now.
        Platform::PXI::PM::ProgramHandle program_handle = {process_id};

        IPC::SendIPCRequest<FS::Reg::Register>(source, fsreg_handle,
                                               process_id, program_handle,
                                               program_info, FS::StorageInfo{});

        source.CallSVC(&OS::SVCCloseHandle, fsreg_handle);
    }

    if (!from_firm)
        source.CallSVC(&OS::SVCCloseHandle, srv_session.first);
    source.GetLogger()->info("{} {}EXHEADER STACK SIZE: {:#x}", ThreadPrinter{source}, __LINE__, exheader.stack_size);

    // Run main thread
    OS::StartupInfo startup{};
    startup.stack_size = exheader.stack_size;
    startup.priority = exheader.aci.flags.priority();
    source.GetLogger()->info("{}about to start title main thread", ThreadPrinter{source});
    std::tie(result) = source.CallSVC(&OS::SVCRun, process.first, startup);

    // Clean up
    source.CallSVC(&OS::SVCCloseHandle, std::move(process).first);

    return std::make_tuple(RESULT_OK, process_id);
}


}  // namespace OS

}  // namespace HLE
