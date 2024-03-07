#pragma once

#include "os.hpp"
#include "framework/formats.hpp" // TODO: Rename contents to generic Serialization namespace

namespace Serialization {

inline auto Loader(HLE::OS::Thread& thread, /*VAddr*/ uint32_t vaddr) {
    return [&thread, vaddr](char* dest, size_t size) mutable {
        for (auto data_ptr = dest; data_ptr - dest < size; ++data_ptr) {
            *data_ptr = thread.ReadMemory(vaddr++);
        }
    };
};

// TODO: Move to common framework
template<typename Data, typename... T>
inline auto LoadVia(T&&... ts) {
    return FileFormat::SerializationInterface<Data>::Load(Loader(std::forward<T>(ts)...));
}

// TODO: We can't implement this currently: Load() doesn't take a reference, but copy, hence the state of loader isn't carried on!
//template<typename... Datas, typename... T>
//inline auto LoadVia(T&&... ts) {
//    auto loader = Loader(std::forward<T>(ts)...);
//    if constexpr (sizeof... (Datas) == 1) {
//        // Return the element directly
//        return (FileFormat::SerializationInterface<Datas>::Load(loader), ...);
//    } else {
//        // Return a tuple of loaded elements
//        return std::tuple<Datas...> { FileFormat::SerializationInterface<Datas>::Load(loader)... };
//    }
//}

} // namespace Serialization
