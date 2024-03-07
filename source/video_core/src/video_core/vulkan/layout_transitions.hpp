#pragma once

#include <vulkan/vulkan.hpp>

// TODO: Allow specifying multiple image memory barriers
inline void TransitionImageLayout(vk::CommandBuffer command_buffer,
                           vk::Image image, vk::ImageSubresourceRange image_subresource_range,
                           vk::ImageLayout old_layout, vk::PipelineStageFlags src_stage_mask, vk::AccessFlags src_access_mask,
                           vk::ImageLayout new_layout, vk::PipelineStageFlags dst_stage_mask, vk::AccessFlags dst_access_mask) {
    vk::ImageMemoryBarrier barrier {
        src_access_mask,
        dst_access_mask,
        old_layout,
        new_layout,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        image,
        image_subresource_range
    };
    command_buffer.pipelineBarrier( src_stage_mask, dst_stage_mask, vk::DependencyFlags { },
                                    {}, // memory barriers
                                    {}, // buffer memory barriers
                                    { barrier });
}

struct ImageLayoutTransitionPoint {
    vk::ImageLayout layout;
    vk::PipelineStageFlags stage_mask;
    vk::AccessFlags access_mask;

    static ImageLayoutTransitionPoint FromUndefined() {
        return { vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits::eTopOfPipe, vk::AccessFlags { } };
    }

    static ImageLayoutTransitionPoint FromTransferSrcGeneral() {
        return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead };
    }

    static ImageLayoutTransitionPoint ToTransferSrcGeneral() {
        return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead };
    }

    static ImageLayoutTransitionPoint FromTransferSrc() {
        return { vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead };
    }

    static ImageLayoutTransitionPoint ToTransferSrc() {
        return { vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead };
    }

    static ImageLayoutTransitionPoint FromTransferDst() {
        return { vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite };
    }

    static ImageLayoutTransitionPoint FromTransferDstGeneral() {
        return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite };
    }

    static ImageLayoutTransitionPoint ToTransferDst() {
        return { vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite };
    }

    static ImageLayoutTransitionPoint ToTransferDstGeneral() {
        return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite };
    }

    static ImageLayoutTransitionPoint ToShaderRead() {
        return { vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead };
    }

    static ImageLayoutTransitionPoint ToShaderReadGeneral() {
        return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead };
    }

    static ImageLayoutTransitionPoint ToPresent() {
        return { vk::ImageLayout::ePresentSrcKHR, vk::PipelineStageFlagBits::eAllCommands, vk::AccessFlags { } };
    }

    static ImageLayoutTransitionPoint FromColorAttachmentOutput() {
        return {    vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead };
    }

    static ImageLayoutTransitionPoint ToColorAttachmentOutput() {
        return {    vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead };
    }

    static ImageLayoutTransitionPoint FromColorAttachmentOutputGeneral() {
        return {    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead };
    }

    static ImageLayoutTransitionPoint ToColorAttachmentOutputGeneral() {
        return {    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead };
    }

    static ImageLayoutTransitionPoint ToDepthStencilAttachmentOutput() {
        return {    vk::ImageLayout::eDepthStencilAttachmentOptimal,
                    vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead };
    }

    static ImageLayoutTransitionPoint ToDepthStencilAttachmentOutputGeneral() {
        return {    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead };
    }

    static ImageLayoutTransitionPoint From(vk::ImageLayout layout) {
        switch (layout) {
        case vk::ImageLayout::eUndefined:
            return FromUndefined();

        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return {    vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits::eFragmentShader,
                        vk::AccessFlagBits::eShaderRead };

        case vk::ImageLayout::eColorAttachmentOptimal:
            return FromColorAttachmentOutput();

        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return {    vk::ImageLayout::eDepthStencilAttachmentOptimal,
                        vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                        vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead };

//        case vk::ImageLayout::eTransferDstOptimal:
//            return FromTransferDst();

        case vk::ImageLayout::eGeneral:
            // Force full synchronization for lack of context
            return { vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eBottomOfPipe, /* TODO?? */ {} };

        default:
            throw std::runtime_error("Unable to infer ImageLayoutTransitionPoint from VkImageLayout " + vk::to_string(layout));
        }
    }
};

inline void TransitionImageLayout(vk::CommandBuffer command_buffer,
                           vk::Image image, vk::ImageSubresourceRange image_subresource_range,
                           ImageLayoutTransitionPoint src, ImageLayoutTransitionPoint dst) {
    return TransitionImageLayout(command_buffer, image, image_subresource_range,
                                 src.layout, src.stage_mask, src.access_mask,
                                 dst.layout, dst.stage_mask, dst.access_mask);
}
