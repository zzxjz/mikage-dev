#pragma once

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <chrono>
#include <utility>

class CPUAwaitable {
    struct ControlBlock {
        ControlBlock(vk::Fence fence) noexcept : fence(fence), observer_count(1) {
        }

        bool IsReady(vk::Device device) {
            if (!fence) {
                // Fence was found to be ready before
                return true;
            }

            auto result = device.getFenceStatus(fence);
            if (result != vk::Result::eSuccess) { // TODO: Check for vk::Result::eNotReady, and handle other errors!
                return false;
            }

            // Reset fence to a null handle so future calls don't need to check again
            fence = nullptr;
            return true;
        }

        vk::Result Wait(vk::Device device, std::chrono::nanoseconds timeout) {
            if (!fence) {
                // Fence was found to be ready before
                return vk::Result::eSuccess;
            }

            auto ret = device.waitForFences({ fence }, false, static_cast<uint64_t>(timeout.count()));
            if (ret == vk::Result::eSuccess) {
                // Reset fence to a null handle so future calls don't need to check again
                fence = nullptr;
            }
            return ret;
        }

        /// Refers to a fence that signals readiness, or nullptr if the fence has been found to be ready before already
        /// TODO: Should this be a UniqueFence? We never appear to free this...
        /// TODO: Actually, I think the point is that the fence is managed outside...
        vk::Fence fence;

        /// Number of CPUAwaitable objects that reference this control block
        std::atomic<size_t> observer_count;
    };

    ControlBlock* control_block = nullptr;

    void Unlink() {
        if (control_block && --control_block->observer_count == 0) {
            delete control_block;
            control_block = nullptr;
        }
    }

public:
    CPUAwaitable() = default;

    explicit CPUAwaitable(vk::Fence fence) : control_block(!fence ? nullptr : new ControlBlock(fence)) {
    }

    CPUAwaitable(const CPUAwaitable& oth) noexcept : control_block(oth.control_block) {
        if (control_block) {
            ++control_block->observer_count;
        }
    }

    CPUAwaitable(CPUAwaitable&& oth) noexcept : control_block(oth.control_block) {
        oth.control_block = nullptr;
    }

    CPUAwaitable& operator=(const CPUAwaitable& oth) noexcept {
        if (this == &oth) {
            return *this;
        }
        Unlink();
        control_block = oth.control_block;
        if (control_block) {
            ++control_block->observer_count;
        }
        return *this;
    }

    CPUAwaitable& operator=(CPUAwaitable&& oth) noexcept {
        if (this == &oth) {
            return *this;
        }
        Unlink();
        control_block = std::exchange(oth.control_block, nullptr);
        return *this;
    }

    ~CPUAwaitable() noexcept {
        Unlink();
    }

    bool IsReady(vk::Device device) {
        return !control_block || control_block->IsReady(device);
    }

    vk::Result Wait(vk::Device device, std::chrono::nanoseconds timeout = std::chrono::nanoseconds { 0x7fffffff }) {
        if (!control_block) {
            return vk::Result::eSuccess;
        }
        return control_block->Wait(device, timeout);
    }
};
