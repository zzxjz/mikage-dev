#include "os.hpp"
#include "os_console.hpp"

#include <platform/sm.hpp>

#include <framework/meta_tools.hpp>

#include <boost/algorithm/string/predicate.hpp>

#include <boost/hana/ext/boost/fusion/deque.hpp>
#include <boost/hana/ext/std/tuple.hpp>

#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/for_each.hpp>

#include <range/v3/algorithm/find_if.hpp>
#include <range/v3/algorithm/for_each.hpp>
#include <range/v3/algorithm/transform.hpp>
#include <range/v3/action/join.hpp>
#include <range/v3/action/sort.hpp>
#include <range/v3/action/transform.hpp>
#include <range/v3/iterator/insert_iterators.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/intersperse.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/zip.hpp>
#include <range/v3/distance.hpp>
#include <range/v3/range_for.hpp>

#include <boost/spirit/home/x3.hpp>

namespace HLE {

namespace OS {

ConsoleModule::ConsoleModule(OS& os) : os(os) {

}

template<typename... T>
decltype(auto) LiftIfVariable(std::tuple<T...>&& tuple) {
    return std::forward(tuple);
}

template<typename T>
auto LiftIfVariable(T&& var) {
    return std::make_tuple(var);
}

std::optional<std::string> ConsoleModule::HandleCommand(const std::string& command) {
    namespace x3 = boost::spirit::x3;

    // Separator
    auto sep = x3::omit[+x3::blank];

    auto process_id = x3::uint_parser<uint32_t, 10>();
    auto thread_id = x3::uint_parser<uint32_t, 10>();
    auto handle = process_id >> x3::lit(".") >> x3::uint_parser<uint32_t, 10>();
    auto object_pointer = x3::lit("0x") > x3::uint_parser<ptrdiff_t, 16>();

    std::string ret;

    // TODO: Use the one from repl.hpp instead!!!!
    auto wrap_handler = [](auto&& callee) {
        return [&callee](auto&& ctx) {
            namespace hana = boost::hana;
            auto args = hana::to_tuple(/*LiftIfVariable(*/x3::_attr(ctx)/*)*/);
            hana::unpack(args, callee);
        };
    };

    auto wrap_singleton_handler = [](auto&& callee) {
        return [&callee](auto&& ctx) {
            callee(x3::_attr(ctx));
        };
    };

    auto&& thread_info = [](Thread& thread) {
        std::string ret;
        auto&& process = thread.GetParentProcess();
        ret += fmt::format("Thread {}.{}, {}/{}: ", process.GetId(), thread.GetId(),
                           process.GetName(), thread.GetName());
        using Status = HLE::OS::Thread::Status;
        switch (thread.status) {
        case Status::Ready:
            ret += "Ready";
            break;

        case Status::Sleeping:
        {
            if (thread.wait_for_all)
                ret += "Waiting for all of [";
            else
                ret += "Waiting for any of [";

            bool first = true;
            for (auto object : thread.wait_list) {
                if (object->GetName() == "CSession_Port_srv:") {
                    auto service_name = Platform::SM::PortName::IPCDeserialize(thread.ReadTLS(0x84), thread.ReadTLS(0x88), 8).ToString();
                    ret += fmt::format("{}(Service {} to be up)", first ? "" : ", ", service_name);
                } else {
                    ret += fmt::format("{}{:#x} ({})", first ? "" : ", ", reinterpret_cast<uintptr_t>(object.get()), object->GetName());
                }
                first = false;
            }
            ret += fmt::format("] with timeout={:#x}", thread.timeout_at);
            break;
        }

        case Status::WaitingForTimeout:
            ret += "WaitingForTimeout in " + std::to_string((thread.timeout_at - process.GetOS().GetTimeInNanoSeconds()) / 1000000.f) + " ms";
            break;

        case Status::WaitingForArbitration:
            ret += fmt::format("Arbiting on address {:#010x} via arbiter {}", thread.arbitration_address,
                                process.handle_table.FindObject<AddressArbiter>(thread.arbitration_handle)->GetName());
            break;

        case Status::Stopped:
            ret += "Stopped";
            break;
        }
        return ret;
    };

    auto&& handle_debug_info = [&](const DebugHandle::DebugInfo& handle_info, Object* object_ptr) {
        std::string ret = fmt::format("created after {:#x} ticks", handle_info.created_time_stamp);
        switch (handle_info.source) {
        case DebugHandle::Original:
            break;

        case DebugHandle::Duplicated:
            ret += ", duplicate of TODO";
            break;

        case DebugHandle::FromIPC:
            ret += fmt::format(", copied via IPC command {:#010x}", handle_info.ipc_command_header);
            break;

        default:
            ret += ", unknown origin";
            break;
        }

        auto&& is_given_object = [&](std::weak_ptr<Event> event) {
            return object_ptr && object_ptr == event.lock().get();
        };

        // Gather list of interrupts this object may be subscribed to
        std::vector<uint32_t> interrupt_subscriptions;
        // TODO: Instead of this zip, we can just use ranges::enumerate!
        ranges::for_each(ranges::view::zip(ranges::view::ints, os.bound_interrupts),
                         [&](auto&& interrupt_subscribers_pair) {
                             auto&& replace_with_interrupt_id = [&](auto&&) { return interrupt_subscribers_pair.first; };
                             auto&& subscriber_list = interrupt_subscribers_pair.second;
                             ranges::transform(subscriber_list | ranges::view::filter(is_given_object),
                                               ranges::back_inserter(interrupt_subscriptions),
                                               replace_with_interrupt_id);
                         });
        if (!interrupt_subscriptions.empty()) {
            ret += fmt::format(", subscribed to interrupt{} ", interrupt_subscriptions.size() == 1 ? "" : "s");
            auto&& to_hex_string = [](uint32_t interrupt) { return fmt::format("{:#x}", interrupt); };
            ranges::for_each(interrupt_subscriptions | ranges::view::transform(to_hex_string) | ranges::view::intersperse(", "),
                             [&](auto&& new_string) { ret += new_string; });
        }

        return ret;
    };

    auto&& handle_info = [this,handle_debug_info](const DebugHandle& handle, Object* object_ptr) {
        auto&& handle_info = handle.debug_info;
        auto ret = fmt::format("Handle {} ({}), ", handle.value, handle_debug_info(handle_info, object_ptr));
        if (auto* timer = dynamic_cast<Timer*>(object_ptr)) {
            ret += fmt::format("{}, ", timer->type == ResetType::OneShot ? "one shot"
                                     : timer->type == ResetType::Sticky ? "sticky"
                                     : timer->type == ResetType::Pulse ? "pulse"
                                     : "other");
            ret += fmt::format("{}, ", timer->active ? "active" : "inactive");
            auto timeout_in = timer->timeout_time_ns - os.GetTimeInNanoSeconds();
            ret += fmt::format("timeout in {}.{:03}, ", timeout_in / 1000, timeout_in % 1000);
            ret += fmt::format("period {}.{:03}, ", timer->period_ns / 1000, timer->period_ns % 1000);
        }
        return ret;
    };

    auto about_memory = [&]() {
        std::string ret;
        for (size_t manager_idx = 0; manager_idx < os.memory_regions.size(); ++manager_idx) {
            auto& manager = os.memory_regions[manager_idx];
            for (auto& mapping : manager.taken) {
                auto owner = mapping.second.owner.lock();
                ret += fmt::format("[{:#010x}-{:#010x}] ({:#x} bytes, owned by {} {})\n",
                                   mapping.first, mapping.first + mapping.second.size_bytes, mapping.second.size_bytes,
                                   owner ? typeid(*owner).name() : "", fmt::ptr(owner.get()));
            }
        }
        return ret;
    };

    auto&& about_thread = [&](ProcessId pid, ThreadId tid) {
        auto&& process = os.GetProcessFromId(pid);
        if (!process) {
            ret += "Unknown process ID\n";
            return;
        }
        auto&& thread = process->GetThreadFromId(tid);
        if (!thread) {
            ret += "Unknown thread ID\n";
            return;
        }

        ret += thread_info(*thread);
        // TODO: Do the following only for EmuThreads!
        ret += "\n\nCPU registers:\n";
        std::array<const char*,17> reg_names = {{
            "r0  ", "r1  ", "r2  ", "r3  ", "r4  ", "r5  ", "r6  ", "r7  ",
            "r8  ", "r9  ", "r10 ", "r11 ", "r12 ", "r13 ", "r14 ", "r15 ",
            "cpsr"
        }};
        RANGES_FOR (auto&& name_and_index, ranges::view::zip(reg_names, ranges::view::ints)) {
            ret += fmt::format("{} {:#x}\n", name_and_index.first, thread->GetCPURegisterValue(name_and_index.second));
        }

        // TODO: Print stack trace
    };

    auto&& about_process = [&](ProcessId pid) {
        auto process = os.process_handles[pid];
        ret += "Process " + process->GetName() + "\n\n";

        ret += "\nMemory map:\n";
        decltype(process->virtual_memory) contiguous_regions;
        auto memory_region_it = process->virtual_memory.begin();
        while (memory_region_it != std::end(process->virtual_memory)) {
            auto is_noncontiguous = [](auto left, auto right) {
                auto left_vaddr_end = left.first + left.second.size;
                auto right_vaddr_start = right.first;
                auto left_paddr_end = left.second.phys_start + left.second.size;
                auto right_paddr_start = right.second.phys_start;
                auto left_permissions = left.second.permissions;
                auto right_permissions = right.second.permissions;

                return (left_vaddr_end != right_vaddr_start) ||
                       (left_paddr_end != right_paddr_start) ||
                       (left_permissions != right_permissions);
            };

            // Find the last contiguous map entry for the current region
            // (if no noncontiguity can be found, the current region extends until the last map entry)
            auto end_region_it = std::adjacent_find(memory_region_it, std::end(process->virtual_memory),
                                                    is_noncontiguous);
            if (end_region_it == std::end(process->virtual_memory))
                end_region_it = std::prev(end_region_it);

            auto region_start_virt = memory_region_it->first;
            auto region_start_phys = memory_region_it->second.phys_start;
            auto region_size = end_region_it->first + end_region_it->second.size - region_start_virt;
            auto raw_permissions = Meta::to_underlying(memory_region_it->second.permissions);
            ret += fmt::format("[{:#010x}-{:#010x}] -> [{:#010x}-{:#010x}] ({:#x} bytes, {}{}{})\n",
                               region_start_virt, region_start_virt + region_size,
                               region_start_phys, region_start_phys + region_size,
                               region_size, (raw_permissions & 1) ? "R" : "", (raw_permissions & 2) ? "W" : "", (raw_permissions & 4) ? "X" : "");

            memory_region_it = std::next(end_region_it);
        }

        ret += "\nHandle table:\n";
        std::map<DebugHandle, std::shared_ptr<Object>, std::less<Handle>> sorted_handle_table;
        ranges::copy(process->handle_table.table, ranges::inserter(sorted_handle_table, ranges::begin(sorted_handle_table)));
        for (auto entry : sorted_handle_table) {
            ret += fmt::format("Handle {} -> {:#x} ({})", entry.first.value, reinterpret_cast<uintptr_t>(entry.second.get()), entry.second ? entry.second->GetName() : "INVALID");
            ret += ": " + handle_debug_info(entry.first.debug_info, entry.second.get()) + "\n";
        }

        ret += "\nThreads:\n";
        std::vector<std::shared_ptr<Thread>> sorted_threads;
        ranges::copy(process->threads, ranges::back_inserter(sorted_threads));
        ranges::sort(sorted_threads, [](auto&& thread1, auto&& thread2) { return thread1->GetId() < thread2->GetId(); });
        for (auto thread : sorted_threads) {
            ret += thread_info(*thread);
            ret += '\n';
        }
    };

    auto about_object = [&](uintptr_t object) {
        // Safely find an Object reference to the given object address by searching all handle tables for the object
        std::map<ProcessId, DebugHandle> matching_handles;

        auto&& get_handle = [=](auto&& process) {
            return process->handle_table.FindHandle(reinterpret_cast<void*>(object));
        };

        auto&& handle_is_present = [=](auto&& process) {
            return Handle{HANDLE_INVALID} != get_handle(process);
        };

        auto&& get_tagged_handle = [=](auto&& process) {
            return std::make_pair(process->GetId(), get_handle(process));
        };

        ranges::transform(os.process_handles | ranges::view::values | ranges::view::filter(handle_is_present), ranges::inserter(matching_handles, ranges::begin(matching_handles)),
                          get_tagged_handle);
        if (ranges::empty(matching_handles)) {
            ret += fmt::format("No kernel object found at address {:#x}", object);
        } else {
            // Pick some process in the list to safely look up the actual Object pointer
            auto some_match = *matching_handles.begin();
            auto object_ptr = os.process_handles[some_match.first]->handle_table.FindObject<Object>(some_match.second);
            ret += fmt::format("Kernel object {:#x} ({}) is explicitly referenced in:\n", object, object_ptr->GetName());
            // TODO: Further information based on the dynamic object type!
            // TODO: List interrupts this object was subscribed to via SVCBindInterrupt

            for (auto handles : matching_handles) {
                auto pid = handles.first;

                ret += fmt::format("Process {} ({}): ", pid, os.process_handles[pid]->GetName());
                ret += handle_info(handles.second, object_ptr.get());
                ret += '\n';
            }
        }
    };

    auto&& lookup_and_set_object_name = [&](ProcessId pid, uint32_t raw_handle, std::string name) {
        if (0 == os.process_handles.count(pid)) {
            ret += "Unknown process ID\n";
            return;
        }

        auto&& process = os.process_handles[pid];
        auto&& object = process->handle_table.FindObject<Object>(Handle{raw_handle});
        if (object == nullptr) {
            ret += "Handle not found in process handle table\n";
            return;
        }

        object->name = name;
        ret += "Ok\n";
    };

    if (x3::parse(command.begin(), command.end(), x3::lit("about threads"))) {
        std::string ret;
        // TODO: Thread-safety!
        // TODO: Embed context information for EmuThreads!
        for (auto& pair : os.process_handles) {
            auto process = pair.second;

            std::vector<std::shared_ptr<Thread>> sorted_threads;
            ranges::copy(process->threads, ranges::back_inserter(sorted_threads));
            ranges::sort(sorted_threads, [](auto&& thread1, auto&& thread2) { return thread1->GetId() < thread2->GetId(); });

            for (auto thread : sorted_threads) {
                ret += thread_info(*thread);
                ret += '\n';
            }
        }
        return ret;
    } else if (x3::parse(command.begin(), command.end(), x3::lit("about memory"))) {
        return about_memory();
    } else if (x3::parse(command.begin(), command.end(), (x3::lit("about thread") >> sep >> process_id >> x3::lit(".") >> thread_id)[wrap_handler(about_thread)])) {
        return ret;
    } else if (x3::parse(command.begin(), command.end(), (x3::lit("about process") >> sep >> process_id)[wrap_singleton_handler(about_process)])) {
        return ret;
    } else if (x3::parse(command.begin(), command.end(), x3::lit("about") >> sep >> object_pointer[wrap_singleton_handler(about_object)])) {
        return ret;
    } else if (x3::parse(command.begin(), command.end(), x3::lit("about objects") >> +(sep >> object_pointer[wrap_singleton_handler(about_object)]))) {
        return ret;
    } else if (x3::parse(command.begin(), command.end(), x3::lit("set name") >> sep >> (handle >> sep >> +x3::graph)[wrap_handler(lookup_and_set_object_name)])) {
        return ret;
    } else if (x3::parse(command.begin(), command.end(), x3::lit("help"))) {
        return {{"Commands:\nabout threads\nabout process <pid>\nabout <host object address>\nset name <pid>.<handle> <name>"}};
    } else {
        return {{"Unknown command!"}};
    }
}

}  // namespace OS

}  // namespace HLE
