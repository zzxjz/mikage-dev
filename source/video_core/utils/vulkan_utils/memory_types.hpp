#pragma once

#include <vulkan/vulkan.hpp>

namespace spdlog {
class logger;
}

namespace Pica::Vulkan {

class MemoryTypeDatabase {
    vk::PhysicalDevice physical_device;

public:
    static constexpr std::array<vk::MemoryPropertyFlags, 2> known_flags_combinations = {{
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    }};

    static constexpr bool FlagsAreValid(vk::MemoryPropertyFlags flags) noexcept {
        for (auto supported_flags : known_flags_combinations) {
            if (flags == supported_flags) {
                return true;
            }
        }

        return false;
    }

public:
    MemoryTypeDatabase(spdlog::logger& logger, vk::PhysicalDevice physical_device);

    template<vk::MemoryPropertyFlagBits... FlagBits>
    vk::MemoryAllocateInfo GetMemoryAllocateInfo(const vk::MemoryRequirements& requirements) const {
        return GetMemoryAllocateInfo<FlagBits...>(requirements.size, requirements);
    }

    template<vk::MemoryPropertyFlagBits... FlagBits>
    vk::MemoryAllocateInfo GetMemoryAllocateInfo(vk::DeviceSize size, const vk::MemoryRequirements& requirements) const {
        constexpr auto desired_flags = (FlagBits | ...);

        static_assert(FlagsAreValid(desired_flags), "Given combination of flags is not supported");

        auto properties = physical_device.getMemoryProperties();
        for (uint32_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
            if (0 == (requirements.memoryTypeBits & (1 << type_index))) {
                continue;
            }

            auto& type = properties.memoryTypes[type_index];
            auto& heap = properties.memoryHeaps[type.heapIndex];

            if ((desired_flags & type.propertyFlags) == desired_flags) {
                return { size, type_index };
            }
        }

        throw std::runtime_error("Could not find find a memory type with flags " + vk::to_string(desired_flags) + " and memory type bits " + std::to_string(requirements.memoryTypeBits));
    }
};

} // namespace Pica::Vulkan
