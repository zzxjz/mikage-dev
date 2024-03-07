#pragma once

#include "ipc.hpp"

#include <boost/hana/fwd/integral_constant.hpp>

#include <string>

namespace Platform {

/**
 * ServiceManager: Manages "services" on the system. Services are nameless
 * ports, which implies that applications can not connect to them through
 * ConnectToPort system call, but instead always have to go through the "srv:"
 * port.
 *
 * This system allows for fine-grained service access control for each process.
 * Before connecting to services through the "srv:" port, applications must be
 * registered to the SM module through the "srv:pm" service. An exception to
 * this rule is made for processes that are embedded in the system firmware:
 * Those processes generally may access any service withour prior registering
 * (hence in particular solving the "hen or egg" problem due to which
 * unregistered processes technically cannot access the srv:pm service to
 * register themselves).
 */
namespace SM {

struct PortName {
    PortName(const char* name) {
        std::string strname(name);
        length = strname.length();
        uint32_t value = 0;
        for (int i = 0; i < 8; ++i) {
            char c = (i < length) ? strname[i] : 0;
            value = (c << 24) | (value >> 8);
            if (i == 3 || i == 7) {
                this->name[i / 4] = value;
                value = 0;
            }
        }
    }

    PortName(uint32_t low, uint32_t high, uint32_t length) : name{low, high}, length(length) {
    }

    static auto IPCDeserialize(uint32_t word_low, uint32_t word_high, uint32_t length) {
        return PortName(word_low, word_high, length);
    }
    static auto IPCSerialize(const PortName& data) {
        return std::make_tuple(data.name[0], data.name[1], data.length);
    }

    std::string ToString() const {
        std::string ret;
        for (unsigned index = 0; index < 8; ++index) {
            auto word_index = index / 4;
            auto byte_within_word = index % 4;
            auto byte = static_cast<uint8_t>(name[word_index] >> (byte_within_word * 8));
            if (byte == 0)
                break;
            ret += byte;
        }
        return ret;
    }

    using ipc_data_size = boost::hana::size_t<3>;

    uint32_t name[2];
    uint32_t length;
};

namespace SRV {

namespace IPC = Platform::IPC;

struct RegisterClient     : IPC::IPCCommand<0x1>::add_process_id
                               ::response {};
struct EnableNotification : IPC::IPCCommand<0x2>
                               ::response::add_handle<IPC::HandleType::Semaphore> {};
struct RegisterService    : IPC::IPCCommand<0x3>::add_serialized<PortName>::add_uint32
                               ::response::add_and_close_handle<IPC::HandleType::ServerPort> {};

/**
 * NOTE: The native SM module returns IPC header 0x00050042 if the service is
 *       not in the source application's extended header (as provided to
 *       srvpm:RegisterProcess), which makes our kernel implementation throw
 *       an error due to attempting to translate a null handle.
 *       TODO: Double-check whether the reference implementation special-cases
 *             null handles for this or not.
 */
struct GetServiceHandle   : IPC::IPCCommand<0x5>::add_serialized<PortName>::add_uint32
                               ::response::add_and_close_handle<IPC::HandleType::ClientSession> {};
struct Subscribe          : IPC::IPCCommand<0x9>::add_uint32
                               ::response {};

struct PublishToSubscriber : IPC::IPCCommand<0xc>::add_uint32::add_uint32
                                ::response {};

} // namespace SRV

namespace SRVPM {

// NOTE: This returns 0xe7e3ffff on success, and 0xd8606408 on error
struct PublishToAll : IPC::IPCCommand<0x402>::add_uint32
                         ::response {};

/**
 * Authorizes a process to access services listed in the static buffer
 *
 * Inputs:
 * - ProcessId for the process to authorize
 * - Number of words in the given static buffer (i.e. twice the number of services to grant access to)
 * - List of PortNames of accessible services
 */
struct RegisterProcess : IPC::IPCCommand<0x403>::add_uint32::add_uint32::add_static_buffer
                            ::response {};

} // namespace SRVPM

} // namespace SM

} // namespace Platform
