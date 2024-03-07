#include "memory_types.hpp"

#include <spdlog/logger.h>

namespace Pica::Vulkan {

MemoryTypeDatabase::MemoryTypeDatabase(spdlog::logger& logger, vk::PhysicalDevice physical_device) : physical_device(physical_device) {
    // Output list of memory types for debug purposes
    auto properties = physical_device.getMemoryProperties();
    for (uint32_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
        auto& type = properties.memoryTypes[type_index];
        auto& heap = properties.memoryHeaps[type.heapIndex];
        logger.info("Memory type {}: {}, using heap {} ({} bytes, {})", type_index, vk::to_string(type.propertyFlags), type.heapIndex, heap.size, vk::to_string(heap.flags));
    }
}

} // namespace Pica::Vulkan
