#include "ipc.hpp"
#include "os.hpp"

namespace HLE {

namespace IPC {

TLSReader::TLSReader(OS::Thread& thread) : thread(thread) {
}

OS::Handle TLSReader::ParseHandle() {
    // TODO: Read the number of handles from the descriptor at "offset" rather
    //       than assuming we should only read a single one!
    auto ret = thread.ReadTLS(offset);
    offset += 4;
    return {ret};
}

OS::Result TLSReader::operator()(const IPC::CommandTags::result_tag& /* unused */) {
    uint32_t data = thread.ReadTLS(offset);
    offset += 4;
    return data;
}

uint32_t TLSReader::operator()(const IPC::CommandTags::uint32_tag& /* unused */) {
    uint32_t data = thread.ReadTLS(offset);
    offset += 4;
    return data;
}

uint64_t TLSReader::operator()(const IPC::CommandTags::uint64_tag& /* unused */) {
    uint64_t data = (static_cast<uint64_t>(thread.ReadTLS(offset + 4)) << 32) | thread.ReadTLS(offset);
    offset += 8;
    return data;
}

IPC::StaticBuffer TLSReader::operator()(const IPC::CommandTags::static_buffer_tag& /* unused */) {
    // TLS@offset stores the translation descriptor, which is only relevant for the kernel
    IPC::TranslationDescriptor descriptor = { thread.ReadTLS(offset) };
    uint32_t buffer_addr = thread.ReadTLS(offset + 4);
    offset += 8;
    return { buffer_addr, descriptor.static_buffer.size.Value(), descriptor.static_buffer.id.Value() };
}

IPC::StaticBuffer TLSReader::operator()(const IPC::CommandTags::pxi_buffer_tag<false>& /* unused */) {
    // TLS@offset stores the translation descriptor, which is only relevant for the kernel
    IPC::TranslationDescriptor descriptor = { thread.ReadTLS(offset) };
    uint32_t buffer_addr = thread.ReadTLS(offset + 4);
    offset += 8;
    // TODO: DONT DO THIS. Instead use the commented-out version.
    return { buffer_addr, descriptor.static_buffer.size.Value(), descriptor.static_buffer.id.Value() };
//    return { buffer_addr, descriptor.pxi_buffer.size.Value(), descriptor.pxi_buffer.id.Value() };
}

IPC::StaticBuffer TLSReader::operator()(const IPC::CommandTags::pxi_buffer_tag<true>& /* unused */) {
    // TLS@offset stores the translation descriptor, which is only relevant for the kernel
    IPC::TranslationDescriptor descriptor = { thread.ReadTLS(offset) };
    uint32_t buffer_addr = thread.ReadTLS(offset + 4);
    offset += 8;
    // TODO: DONT DO THIS. Instead use the commented-out version.
    return { buffer_addr, descriptor.static_buffer.size.Value(), descriptor.static_buffer.id.Value() };
//    return { buffer_addr, descriptor.pxi_buffer.size.Value(), descriptor.pxi_buffer.id.Value() };
}

OS::ProcessId TLSReader::operator()(const IPC::CommandTags::process_id_tag& /* unused */) {
    uint32_t process_id = thread.ReadTLS(offset + 4);
    offset += 8;
    return process_id;
}

MappedBuffer TLSReader::operator()(const IPC::CommandTags::map_buffer_r_tag& /* unused */) {
    MappedBuffer ret;
    ret.size = TranslationDescriptor{thread.ReadTLS(offset)}.map_buffer.size;
    ret.addr = thread.ReadTLS(offset + 4);
    offset += 8;
    return ret;
}

MappedBuffer TLSReader::operator()(const IPC::CommandTags::map_buffer_w_tag& /* unused */) {
    MappedBuffer ret;
    ret.size = TranslationDescriptor{thread.ReadTLS(offset)}.map_buffer.size;
    ret.addr = thread.ReadTLS(offset + 4);
    offset += 8;
    return ret;
}


TLSWriter::TLSWriter(OS::Thread& thread) : thread(thread) {
}

void TLSWriter::WriteHandleDescriptor(const OS::Handle* data, size_t num, bool close) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MakeHandles(num, close).raw);
    offset += 4;
    for (auto data_ptr = data; data_ptr != data + num; ++data_ptr) {
        thread.WriteTLS(offset, data_ptr->value);
        offset += 4;
    }
}

void TLSWriter::operator()(IPC::CommandTags::result_tag, OS::Result data) {
    thread.WriteTLS(offset, data);
    offset += 4;
}

void TLSWriter::operator()(IPC::CommandTags::uint32_tag, uint32_t data) {
    thread.WriteTLS(offset, data);
    offset += 4;
}

void TLSWriter::operator()(IPC::CommandTags::uint64_tag, uint64_t data) {
    thread.WriteTLS(offset    , data & 0xFFFFFFFF);
    thread.WriteTLS(offset + 4, data >> 32);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::map_buffer_r_tag, const MappedBuffer& buffer) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MapBuffer(buffer.size, TranslationDescriptor::Read).raw);
    thread.WriteTLS(offset + 4, buffer.addr);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::map_buffer_w_tag, const MappedBuffer& buffer) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MapBuffer(buffer.size, TranslationDescriptor::Write).raw);
    thread.WriteTLS(offset + 4, buffer.addr);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::static_buffer_tag, const StaticBuffer& buffer) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MakeStaticBuffer(buffer.id, buffer.size).raw);
    thread.WriteTLS(offset + 4, buffer.addr);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::process_id_tag, const EmptyValue&) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MakeProcessHandle().raw);
    thread.WriteTLS(offset + 4, 0);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::pxi_buffer_tag<false>, const StaticBuffer& buffer) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MakePXIBuffer(buffer.id, buffer.size).raw);
    thread.WriteTLS(offset + 4, buffer.addr);
    offset += 8;
}

void TLSWriter::operator()(IPC::CommandTags::pxi_buffer_tag<true>, const StaticBuffer& buffer) {
    thread.WriteTLS(offset, IPC::TranslationDescriptor::MakePXIConstBuffer(buffer.id, buffer.size).raw);
    thread.WriteTLS(offset + 4, buffer.addr);
    offset += 8;
}

} // namespace IPC

} // namespace HLE
