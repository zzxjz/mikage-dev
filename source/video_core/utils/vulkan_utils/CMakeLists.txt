add_library(vulkan-util STATIC
    device_manager.cpp
    glsl_helper.cpp
    memory_types.cpp
    )
target_include_directories(vulkan-util INTERFACE ..)
target_link_libraries(vulkan-util PUBLIC vulkan)
target_link_libraries(vulkan-util PUBLIC shaderc::shaderc)
target_link_libraries(vulkan-util PRIVATE range-v3::range-v3)
target_link_libraries(vulkan-util PRIVATE spdlog::spdlog)
set_property(TARGET vulkan-util PROPERTY POSITION_INDEPENDENT_CODE ON)
