#include "dummy.hpp"

namespace HLE {

namespace OS {

struct DummyProcess {
    DummyProcess(FakeThread& thread) {
        thread.CallSVC(&OS::SVCExitThread);
    }
};

template<> std::shared_ptr<WrappedFakeProcess> CreateFakeProcessViaContext<DummyProcess>(OS& os, Interpreter::Setup& setup, uint32_t pid, const std::string& name) {
    return WrappedFakeProcess::CreateWithContext<DummyProcess>(os, setup, pid, name);
}

} // namespace OS

} // namespace HLE
