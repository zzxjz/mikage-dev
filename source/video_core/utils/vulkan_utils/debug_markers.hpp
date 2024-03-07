#pragma once

#include <vulkan/vulkan.hpp>

#include <type_traits>

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC != 1
#error "May not use this file without dynamic dispatcher"
#endif

namespace Pica::Vulkan {

namespace detail {

template<typename T>
struct is_unique_handle : std::false_type { };
template<typename T, typename Dispatch>
struct is_unique_handle<vk::UniqueHandle<T, Dispatch>> : std::true_type { };

inline constexpr auto DebugObjectType(vk::Semaphore) {
    return vk::DebugReportObjectTypeEXT::eSemaphore;
}

inline constexpr auto DebugObjectType(vk::CommandBuffer) {
    return vk::DebugReportObjectTypeEXT::eCommandBuffer;
}

inline constexpr auto DebugObjectType(vk::Fence) {
    return vk::DebugReportObjectTypeEXT::eFence;
}

inline constexpr auto DebugObjectType(vk::DeviceMemory) {
    return vk::DebugReportObjectTypeEXT::eDeviceMemory;
}

inline constexpr auto DebugObjectType(vk::Buffer) {
    return vk::DebugReportObjectTypeEXT::eBuffer;
}

inline constexpr auto DebugObjectType(vk::Image) {
    return vk::DebugReportObjectTypeEXT::eImage;
}

inline constexpr auto DebugObjectType(vk::ImageView) {
    return vk::DebugReportObjectTypeEXT::eImageView;
}

} // namespace detail

template<typename T>
inline void SetDebugName(vk::Device device, const T& object, const char* name) {
    if constexpr (detail::is_unique_handle<std::decay_t<T>>::value) {
        SetDebugName(device, *object, name);
    } else {
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkDebugMarkerSetObjectNameEXT) {
            vk::DebugMarkerObjectNameInfoEXT object_info {
                detail::DebugObjectType(object),
                reinterpret_cast<uint64_t>(static_cast<typename T::CType>(object)),
                name
            };
            device.debugMarkerSetObjectNameEXT(object_info);
        }
    }
}

struct GuardedDebugMarker {
    vk::CommandBuffer cmd_buffer;

    GuardedDebugMarker(vk::CommandBuffer cmd_buffer, const char* name, const std::array<float, 4>& color = { })
        : cmd_buffer(cmd_buffer) {
        if (HasDebugMarkerExtension()) {
            cmd_buffer.debugMarkerBeginEXT({ name, color });
        }
    }

    ~GuardedDebugMarker() {
        if (HasDebugMarkerExtension()) {
            cmd_buffer.debugMarkerEndEXT();
        }
    }

private:
    bool HasDebugMarkerExtension() const {
        return (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDebugMarkerBeginEXT && VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDebugMarkerEndEXT);
    }
};

} // namespace Pica::Vulkan
