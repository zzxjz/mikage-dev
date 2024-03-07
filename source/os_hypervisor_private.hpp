#pragma once

#include <os_types.hpp>

#include <ipc.hpp>

#include <memory>

#include <boost/mp11/list.hpp>

namespace HLE {

namespace OS {

class Thread;

class Hypervisor;

namespace HPV {

struct ThreadLocalStorage;

struct Object {
    virtual ~Object() = default;
};

template<typename T>
struct RefCounted {
    RefCounted() = default;

    RefCounted(T* raw_object) : object(new std::unique_ptr<Object>(static_cast<Object*>(raw_object))) {
        static_assert(std::is_base_of_v<HPV::Object, T>, "Given type is not a child class of HPV::Object");
    }

    RefCounted(const RefCounted& oth) : object(oth.object) {
    }

    RefCounted(RefCounted&& oth) : object(std::move(oth.object)) {
    }

    RefCounted& operator=(const RefCounted& oth) = delete;
    RefCounted& operator=(RefCounted&& oth) = delete;

    // Releases ownership of the previously managed object and takes ownership of the given one instead.
    // The internal reference count will not be changed.
    std::unique_ptr<Object> ReplaceManagedObject(T* new_object) {
//        return std::exchange(*object, new_object);
        auto old = std::move(*object);
        *object = std::unique_ptr<Object>(new_object);
        return old;
    }

    operator bool() const {
        return object->get();
    }
    T* operator->() {
        return static_cast<T*>(object->get());
    }

    const T* operator->() const {
        return static_cast<const T*>(object->get());
    }

    std::shared_ptr<std::unique_ptr<Object>> object;
};

template<typename T, typename U>
static RefCounted<T> static_refcounted_cast(RefCounted<U> obj) {
    static_cast<void>(static_cast<T*>(obj.object->get())); // Throw away result, and just static cast to verify the involved pointers are compatible
    RefCounted<T> ret;
    ret.object = obj.object;
    return ret;
}

template<typename T, typename U>
static RefCounted<T> dynamic_refcounted_cast(RefCounted<U> obj) {
    if (!dynamic_cast<T*>(obj.object->get())) {
        return nullptr;
    }
    RefCounted<T> ret;
    ret.object = obj.object;
    return ret;
}

/**
 * Context roughly equivalent to the state usually managed in actual HLE/LLE
 * services. Instances of this are shared between ports relate to the same
 * module (fs, sm, ...)
 */
struct SessionContext {
protected:
    ~SessionContext() = default;
};

struct Port : Object {
    virtual std::string Name() const = 0;
};

// Port accessible using a global identifier (assigned using SVCCreatePort)
struct NamedPort : Port {
    NamedPort(std::string_view name) : name(name) {
    }

    std::string Name() const final {
        return (name.empty() ? "UnknownPort" : name);
    }

    std::string name;
};

// Port accessible only through ServiceManager (name assigned using SM::SRV::RegisterService)
struct ServicePort : Port {
    ServicePort(std::string_view name) : name(name) {
    }

    std::string Name() const final {
        return (name.empty() ? "UnknownPort" : name);
    }

    std::string name;
};

struct Session : Object {
    virtual std::string Describe() const = 0;

    void OnRequest(Hypervisor&, Thread&, Thread&, Handle session);

    // Callback invoked when intercepting IPC replies on this session
    HPV::IPCCallback hpv_ipc_callback;

protected:
    // Default handler for the public OnRequest member function
    void OnRequest(Hypervisor&, Thread&, Handle session, std::string_view command_description);

    virtual void OnRequest(Hypervisor&, Thread&, Handle session);

private:
    // Valid only during execution of OnRequest
    Thread* client_thread = nullptr;
};

// Default implementation used as a placeholder until the Session is named. We replace the Session object in place with a more suiting subclass then.
struct UnnamedSession : Session {
    // Placeholder for sessions that have no name associated to them, yet
    std::string Describe() const override {
        return "UnknownSession";
    }
};

struct SessionToPort : Session {
    SessionToPort(RefCounted<Port> port, SessionContext& context) : port(port), context(context) {
    }

    std::string Describe() const override {
        return port->Name();
    }

    RefCounted<Port> port;
    SessionContext& context;
};

struct NamedSession : Session {
    NamedSession(std::string_view name, SessionContext& context) : name(name), context(context) {
    }

    std::string Describe() const override {
        return name;
    }

    std::string name;
    SessionContext& context;
};

template<typename Handler, typename... ExtraParameters, typename... Tags>
static auto InvokeWithIPCArguments(Handler&& handler, IPC::TLSReader reader, boost::mp11::mp_list<Tags...>, ExtraParameters&&... extras) {
    constexpr bool handler_is_valid = std::is_invocable_v<Handler, ExtraParameters..., decltype(reader(Tags{}))...>;
    static_assert(handler_is_valid, "Given handler is not compatible with this IPC command");

    // Hide the rest of this function behind a validity check to prevent compilers from spamming errors
    if constexpr (handler_is_valid) {
        using Result = decltype(handler(extras..., reader(Tags{})...));
        if constexpr (std::is_void_v<Result>) {
            Meta::CallWithSequentialEvaluation<Result> { std::forward<Handler>(handler), std::forward<ExtraParameters>(extras)..., reader(Tags{})... };
        } else {
            return Meta::CallWithSequentialEvaluation<Result> { std::forward<Handler>(handler), std::forward<ExtraParameters>(extras)..., reader(Tags{})... }.GetResult();
        }
    }
}

template<typename Result = void>
struct DispatcherBase {
    Thread& thread;
    uint32_t command_header;
    bool handled = false;

    // Convert void to nullptr_t so that this compiles
    using Result2 = std::conditional_t<std::is_void_v<Result>, decltype(nullptr), Result>;
    std::optional<Result2> result = { };

    template<typename Handler>
    void OnUnknown(Handler&& handler) {
        if (!handled) {
            if constexpr (std::is_void_v<Result>) {
                std::forward<Handler>(handler)();
            } else {
                result = std::forward<Handler>(handler)();
            }
        }
    }

protected:
    template<bool IsResponse, typename Command, typename Handler, typename... ExtraParameters>
    void DecodeMessage(Handler&& handler, ExtraParameters&&... extras) {
        using CommandTags = std::conditional_t<IsResponse, typename Command::response_list, typename Command::request_list>;

        IPC::TLSReader reader = { thread };
        if constexpr (std::is_void_v<Result>) {
            InvokeWithIPCArguments(std::forward<Handler>(handler), std::move(reader), CommandTags { }, std::forward<ExtraParameters>(extras)...);
        } else {
            result = InvokeWithIPCArguments(std::forward<Handler>(handler), std::move(reader), CommandTags { }, std::forward<ExtraParameters>(extras)...);
        }
    }
};

template<typename Command, typename Result = void>
struct ResponseDispatcher : DispatcherBase<Result> {
    Session& session;

    ResponseDispatcher(Thread& thread, Session& session)
        : DispatcherBase<Result> { thread, 0x0 /* TODO */ },
          session(session) {
    }

    ResponseDispatcher(const ResponseDispatcher&) = delete;

    template<typename Handler>
    void OnResponse(Handler&& handler) const {
        session.hpv_ipc_callback = [handler=std::forward<Handler>(handler)](Hypervisor& hypervisor, Thread& thread) mutable {
            // TODO: Verify thread.ReadTLS(0x80) == Command::response_header

            IPC::TLSReader reader = { thread };
            using CommandTags = typename Command::response_list;
            return InvokeWithIPCArguments(std::forward<Handler>(handler), std::move(reader), CommandTags { }, hypervisor, thread);
        };
    }
};

template<typename Result = void>
struct RequestDispatcher : DispatcherBase<Result> {
    Session& session;

    RequestDispatcher(Thread& thread, Session& session, uint32_t command_header)
        : DispatcherBase<Result> { thread, command_header },
          session(session) {
    }

    template<typename Command, typename Handler>
    void DecodeRequest(Handler&& handler) {
        if (!this->handled && this->command_header == Command::request_header) {
            auto response_disp = ResponseDispatcher<Command>(this->thread, session);
            this->template DecodeMessage<false, Command>(std::forward<Handler>(handler), response_disp);
            this->handled = true;
        }
    }
};

} // namespace HPV

} // namespace OS

} // namespace HLE
