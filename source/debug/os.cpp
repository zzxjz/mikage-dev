#include "os.hpp"
#include <os.hpp>

#include <pistache/router.h>

#include <range/v3/algorithm/copy.hpp>
#include <range/v3/iterator/insert_iterators.hpp>

namespace Debugger {

void OSService::RegisterProcess(HLE::OS::ProcessId pid, HLE::OS::Process& process) {
    std::lock_guard guard(access_mutex);

    processes[pid].process = &process;
}

void OSService::UnregisterProcess(HLE::OS::ProcessId pid) {
    std::lock_guard guard(access_mutex);

    processes.erase(pid);
}

void OSService::RegisterThread(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid, HLE::OS::Thread& thread) {
    std::lock_guard guard(access_mutex);

    processes[pid].threads[tid] = &thread;
}

void OSService::UnregisterThread(HLE::OS::ProcessId pid, HLE::OS::ThreadId tid) {
    std::lock_guard guard(access_mutex);

    processes[pid].threads.erase(tid);
}

void OSService::Shutdown() {
    std::lock_guard guard(access_mutex);

    processes.clear();
}

using namespace Pistache;

static void doOptions(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response) {
    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::AccessControlAllowHeaders>("user-agent");
    response.send(Http::Code::No_Content);
}

static void doProcessList(OSService& service, const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    std::string body = "[";
    for (auto& pid_and_process : service.processes) {
        auto& process_info = pid_and_process.second;

        // TODO: EmuProcess vs FakeProcess
        body += fmt::format(R"({{ "id": {}, "name": "{}", "threadcount": {} }},)", pid_and_process.first, process_info.process->GetName(), process_info.threads.size());
    }
    if (!service.processes.empty()) {
        // Drop trailing comma
        body.pop_back();
    }

    response.send(Http::Code::Ok, body + "]");
}

static void doHandleTable(OSService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto pid = request.param(":pid").as<HLE::OS::ProcessId>();
    if (!service.processes.count(pid)) {
        throw std::runtime_error(fmt::format("Tried to access invalid process with pid {}", pid));
    }

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    std::string body = "[";

    namespace HOS = HLE::OS;

    auto& process = *service.processes[pid].process;
    std::map<HOS::DebugHandle, std::shared_ptr<HOS::Object>, std::less<HOS::Handle>> sorted_handle_table;
    ranges::copy(process.handle_table.table, ranges::inserter(sorted_handle_table, ranges::begin(sorted_handle_table)));
    for (auto& handle : sorted_handle_table) {
        body += fmt::format(R"({{ "id": {}, "name": "{}" }},)", handle.first.value, handle.second->GetName());
    }
    if (!sorted_handle_table.empty()) {
        // Drop trailing comma
        body.pop_back();
    }
    response.send(Http::Code::Ok, body + "]");
}

static void doProcessThreadList(OSService& service, const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
    std::lock_guard guard(service.access_mutex);

    auto pid = request.param(":pid").as<HLE::OS::ProcessId>();
    if (!service.processes.count(pid)) {
        throw std::runtime_error(fmt::format("Tried to access invalid process with pid {}", pid));
    }

    response.headers().add<Http::Header::AccessControlAllowOrigin>("*");
    response.headers().add<Http::Header::ContentType>(Http::Mime::MediaType { Http::Mime::Type::Application, Http::Mime::Subtype::Json });

    std::string body = "[";
    for (auto& tid_and_thread : service.processes[pid].threads) {
        auto& thread = *tid_and_thread.second;

        const char* status = [&]() {
            switch (thread.status) {
            case HLE::OS::Thread::Status::Ready: return "Ready";
            case HLE::OS::Thread::Status::Sleeping: return "Sleeping";
            case HLE::OS::Thread::Status::WaitingForTimeout: return "WaitingForTimeout";
            case HLE::OS::Thread::Status::WaitingForArbitration: return "WaitingForArbitration";
            case HLE::OS::Thread::Status::Stopped: return "Stopped";
            }
            throw std::runtime_error("Unknown thread status");
        }();

        // TODO: Send jitcontextid only for EmuThreads!
        auto emu_thread = dynamic_cast<HLE::OS::EmuThread*>(&thread);
        auto jitcontextid = 4294967297; // TODO: Retrieve from emu_thread
        body += fmt::format(R"({{ "id": {}, "pid": {}, "name": "{}", "status": "{}", "jitcontextid": {} }},)",
                            tid_and_thread.first, thread.GetParentProcess().GetId(), thread.GetName(),
                            status, jitcontextid);
    }
    if (!service.processes.empty()) {
        // Drop trailing comma
        body.pop_back();
    }
    response.send(Http::Code::Ok, body + "]");
}

void OSService::RegisterRoutes(Pistache::Rest::Router& router) {
    using namespace Rest;

    Routes::Get(router, "/os/processes",
                [this](const auto& request, auto response) { doProcessList(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/os/process/:pid/handletable",
                [this](const auto& request, auto response) { doHandleTable(*this, request, std::move(response)); return Route::Result::Ok; });
    Routes::Get(router, "/os/process/:pid/threads",
                [this](const auto& request, auto response) { doProcessThreadList(*this, request, std::move(response)); return Route::Result::Ok; });
//    Routes::Get(router, "/os/process/:pid/thread/:tid/registers",
//                [this](const auto& request, auto response) { doProcessThreadList(*this, request, std::move(response)); return Route::Result::Ok; });

    // TODO: Interface to read process memory

    // TODO: Move those to DebugServer itself
//    Routes::Options(router, "/os", Routes::bind(doOptions));
    Routes::Options(router, "/os/*", Routes::bind(doOptions));
    Routes::Options(router, "/os/*/*/*", Routes::bind(doOptions));
}

template<>
std::unique_ptr<Service> CreateService<OSService>() {
    return std::make_unique<OSService>();
}

} // namespace Debugger
