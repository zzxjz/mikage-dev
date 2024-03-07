# Adding an interface library to propagate include directories
add_library(framework exceptions.cpp profiler.cpp)
target_include_directories(framework INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

target_link_libraries(framework PUBLIC fmt::fmt)
target_link_libraries(framework INTERFACE range-v3::range-v3)
target_link_libraries(framework PUBLIC Boost::boost)
if(libunwind_FOUND)
    target_link_libraries(framework PRIVATE libunwind::unwind)
endif ()
