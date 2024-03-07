#pragma once

#include "context.h"
#include "memory.h"
#include "shader.hpp"

#include <platform/gpu/pica.hpp>

#include <common/log.h>

#include <boost/range/algorithm/fill.hpp>

namespace Pica {

class VertexLoader {
public:
    VertexLoader(const Context& context, const Regs::VertexAttributes& attribute_config) {
        boost::fill(vertex_attribute_sources, Memory::HostMemoryBackedPages { });

        // Setup attribute data from loaders
        for (int loader = 0; loader < 12; ++loader) {
            const auto& loader_config = attribute_config.attribute_loaders[loader];

            uint32_t load_address = attribute_config.GetPhysicalBaseAddress() + loader_config.data_offset;

            // TODO: What happens if a loader overwrites a previous one's data?
            for (unsigned component = 0; component < loader_config.component_count; ++component) {
                uint32_t attribute_index = loader_config.GetComponent(component);

                if (attribute_index >= 12) {
                    // Explicit alignment by 4/8/12/16 bytes.
                    // TODO: Should offset be additionally aligned to the alignment size itself?
                    load_address = (load_address + 3) & ~3;
                    load_address += 4 * (attribute_index - 11);
                    continue;
                }

                // TODO: attribute_index >= 12 has special semantics (padding!) ?

                // Align to element size
                const auto element_size = attribute_config.GetElementSizeInBytes(attribute_index);
                load_address = (load_address + element_size - 1) & ~(element_size - 1);

                // TODO: Grab all memory until the bus end, then compare against a maximum vertex bound in Load()
                // TODO: Keep around the physical address used to look up vertex_attribute_sources for debugging
                vertex_attribute_source_addrs[attribute_index] = load_address;
                vertex_attribute_sources[attribute_index] = Memory::LookupContiguousMemoryBackedPage/*<Memory::HookKind::Read>*/(*context.mem, load_address, 1);
                vertex_attribute_strides[attribute_index] = static_cast<uint32_t>(loader_config.byte_count);
                vertex_attribute_formats[attribute_index] = static_cast<uint32_t>(attribute_config.GetFormat(attribute_index));
                vertex_attribute_elements[attribute_index] = attribute_config.GetNumElements(attribute_index);
                vertex_attribute_element_size[attribute_index] = element_size;
                load_address += attribute_config.GetStride(attribute_index);
            }
        }

        default_attributes = GetDefaultAttributes(context, attribute_config);
    }


    void Load(VertexShader::InputVertex& input, Context& context, const Regs::VertexAttributes& attribute_config, uint32_t vertex) const {
        input = default_attributes;

        // TODO: Add bounds check on vertex! When initializing vertex_attribute_sources, we should compute the maximum vertex index possible and compare against that
        // TODO: Verify all vertex_attribute_sources used have actually been initialized

        for (int i = 0; i < attribute_config.GetNumTotalAttributes(); ++i) {
            auto& attr = input.attr[i];
            if (!vertex_attribute_sources[i]) { // TODO: Check vertex_attribute_elements and vertex_attribute_element_size instead?
                // TODO: Required by lego batman during bootup?
                continue;
            }

//            // Load dynamic vertex data (taking priority over fixed attributes)
//            for (unsigned int comp = 0; comp < vertex_attribute_elements[i]; ++comp) {
//                PAddr data_addr = vertex_attribute_strides[i] * vertex + comp * vertex_attribute_element_size[i];
//                auto get_data = [&](auto type) { return Memory::Read<decltype(type)>(vertex_attribute_sources[i], data_addr); };
//                auto get_data_float = [&]() {
//                    uint32_t ret = get_data(uint32_t{});
//                    float fret;
//                    memcpy(&fret, &ret, sizeof(ret));
//                    return fret;
//                };

//                const float srcval = (vertex_attribute_formats[i] == 0) ? static_cast<int8_t>(get_data(uint8_t{})) :
//                                    (vertex_attribute_formats[i] == 1) ? get_data(uint8_t{}) :
//                                    (vertex_attribute_formats[i] == 2) ? static_cast<int16_t>(get_data(uint16_t{})) :
//                                                                        get_data_float();
//                attr[comp] = float24::FromFloat32(srcval);
//                LOG_TRACE(HW_GPU, "Loaded component %x of attribute %x for vertex %x (index %x) from 0x%08x + 0x%08lx + 0x%04lx: %f",
//                            comp, i, vertex, index,
//                            attribute_config.GetPhysicalBaseAddress(),
//                            vertex_attribute_sources[i] - attribute_config.GetPhysicalBaseAddress(),
//                            vertex_attribute_strides[i] * vertex + comp * vertex_attribute_element_size[i],
//                            input.attr[i][comp].ToFloat32());
//            }
            switch (vertex_attribute_formats[i]) {
            case 0:
                ComponentLoader<int8_t>(i, vertex, attr);
                break;

            case 1:
                ComponentLoader<uint8_t>(i, vertex, attr);
                break;

            case 2:
                ComponentLoader<int16_t>(i, vertex, attr);
                break;

            default:
                ComponentLoader<float>(i, vertex, attr);
                break;
            }
        }
    }

    std::array<std::pair<uint32_t, uint32_t>, 16>
    GetUsedAddressRanges(const Regs::VertexAttributes& attribute_config, uint32_t min_index, uint32_t end_index) const {
        std::array<std::pair<uint32_t, uint32_t>, 16> ret {};

        for (int i = 0; i < attribute_config.GetNumTotalAttributes(); ++i) {
            if (!vertex_attribute_sources[i]) {
                continue;
            }

            auto stride = vertex_attribute_strides[i];

            if (stride == 0) {
                throw std::runtime_error("Unsupported zero-byte vertex stride");
            }

//            ret[i].first = vertex_attribute_source_addrs[i] + stride * min_index;
//            ret[i].second = stride * (end_index - min_index);
            ret[i].first = vertex_attribute_source_addrs[i];
            ret[i].second = stride * end_index;
        }

        return ret;
    }

private:
    template<typename T>
    void ComponentLoader(int idx, uint32_t vertex, decltype(VertexShader::InputVertex::attr[0])& attr) const {
        // Copy state to local temporaries. This avoids repeated reads from them in the loop
        auto stride = vertex_attribute_strides[idx];
        auto element_size = vertex_attribute_element_size[idx];
        auto source_memory = vertex_attribute_sources[idx];

        for (unsigned int comp = 0; comp < vertex_attribute_elements[idx]; ++comp) {
            PAddr data_addr = stride * vertex + comp * element_size;

            float srcval;
            if constexpr (std::is_same_v<T, int8_t>) {
                srcval = static_cast<int8_t>(Memory::Read<uint8_t>(source_memory, data_addr));
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                srcval = Memory::Read<uint8_t>(source_memory, data_addr);
            } else if constexpr (std::is_same_v<T, int16_t>) {
                srcval = static_cast<int16_t>(Memory::Read<uint16_t>(source_memory, data_addr));
            } else if constexpr (std::is_same_v<T, float>) {
                uint32_t u32val = Memory::Read<uint32_t>(source_memory, data_addr);
                memcpy(&srcval, &u32val, sizeof(u32val));
            }
            attr[comp] = float24::FromFloat32(srcval);
//            LOG_TRACE(HW_GPU, "Loaded component %x of attribute %x for vertex %x (index %x) from 0x%08x + 0x%08lx + 0x%04lx: %f",
//                        comp, i, vertex, index,
//                        attribute_config.GetPhysicalBaseAddress(),
//                        vertex_attribute_sources[i] - attribute_config.GetPhysicalBaseAddress(),
//                        vertex_attribute_strides[i] * vertex + comp * vertex_attribute_element_size[i],
//                        input.attr[i][comp].ToFloat32());
        }
    }

    VertexShader::InputVertex GetDefaultAttributes(const Context& context, const Regs::VertexAttributes& attribute_config) const {
        VertexShader::InputVertex input;

        for (int i = 0; i < attribute_config.GetNumTotalAttributes(); ++i) {
            auto& attr = input.attr[i];

            if (vertex_attribute_elements[i]) {
                // Default values used when the attribute loader is active but doesn't initialize all attribute elements
                attr[0] = float24::FromFloat32(0.0f);
                attr[1] = float24::FromFloat32(0.0f);
                attr[2] = float24::FromFloat32(0.0f);
                attr[3] = float24::FromFloat32(1.0f);
            } else if (attribute_config.fixed_attribute_mask & (1 << i)) {
                // Fixed vertex attributes set by the application
                memcpy(&attr, context.vertex_attribute_defaults[i].data(), sizeof(context.vertex_attribute_defaults[i]));
            }
        }
        return input;
    }
public: // TODO: Needed for hardware vertex loading
    // Merger of fixed vertex attributes and fallback values
    VertexShader::InputVertex default_attributes;

    // Information about internal vertex attributes
    uint32_t vertex_attribute_source_addrs[16]; // Keep around for inspection in debugging sessions, only
    Memory::HostMemoryBackedPages vertex_attribute_sources[16];
    uint32_t vertex_attribute_strides[16];
    uint32_t vertex_attribute_formats[16];

    // HACK: Initialize vertex_attribute_elements to zero to prevent infinite loops below.
    // This is one of the hacks required to deal with uninitalized vertex attributes.
    // TODO: Fix this properly.
    uint32_t vertex_attribute_elements[16] = {};
    uint32_t vertex_attribute_element_size[16];
};

} // namespace Pica
