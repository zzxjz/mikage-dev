find_package(SPIRV-Tools REQUIRED)

add_library(vulkan-util STATIC
    device_manager.cpp
    glsl_helper.cpp
    memory_types.cpp
    )
target_include_directories(vulkan-util INTERFACE ..)
target_link_libraries(vulkan-util PUBLIC Vulkan::Vulkan)
target_link_libraries(vulkan-util PUBLIC glslang::glslang)
target_link_libraries(vulkan-util PUBLIC glslang::glslang-default-resource-limits)
target_link_libraries(vulkan-util PUBLIC glslang::SPIRV)
target_link_libraries(vulkan-util PRIVATE SPIRV-Tools) # not linked by upstream glslang
target_link_libraries(vulkan-util PRIVATE range-v3::range-v3)
target_link_libraries(vulkan-util PRIVATE spdlog::spdlog)
set_property(TARGET vulkan-util PROPERTY POSITION_INDEPENDENT_CODE ON)
