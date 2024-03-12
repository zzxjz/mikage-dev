# 3.11 added support for interface libraries
# 3.13 detects and target-izes all of our boost dependencies properly
cmake_minimum_required(VERSION 3.13)

include(ExternalProject)

project(mikage)

enable_testing()

if (NOT ANDROID)
    add_link_options("-fuse-ld=mold")
endif()

option(USE_ASAN "Enable address sanitizer" OFF)
if (USE_ASAN)
    add_compile_definitions(BOOST_USE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/externals/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# TODO: Move these flags into a "boost" cmake target
add_definitions(-DBOOST_FILESYSTEM_NO_DEPRECATED -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fstrict-aliasing -Wpedantic -Wfatal-errors -Wdocumentation -Weverything -Wextra -Wconversion -Wno-c++98-compat -Wno-c++98-c++11-compat-pedantic -Wno-c++98-compat-pedantic")
# TODO: Consider -Wsuggest-override (since GCC 5.1)
# TODO: When using clang, use -Wno-gnu-statement-expression: Necessary to prevent warning spam when using clang and the assert macro
# TODO: When using clang, -Weverything
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fstrict-aliasing -Wpedantic -Wfatal-errors -Wdocumentation")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fstrict-aliasing -Wpedantic")

set(BOOST_COMPONENTS context filesystem iostreams program_options thread)
find_package(Boost 1.80.0 EXACT CONFIG COMPONENTS ${BOOST_COMPONENTS})
if (NOT Boost_FOUND)
    find_package(Boost 1.80.0 REQUIRED COMPONENTS ${BOOST_COMPONENTS})
endif ()

find_package(fmt REQUIRED)

find_package(range-v3 REQUIRED)

find_package(SDL2 REQUIRED)

find_package(spdlog REQUIRED)

find_package(cryptopp REQUIRED)

find_package(Pistache)
if (Pistache_FOUND)
    add_compile_definitions(ENABLE_PISTACHE=1)
else()
    add_compile_definitions(ENABLE_PISTACHE=0)
endif()

find_package(Catch2 REQUIRED)

find_package(Tracy REQUIRED)

find_package(xxHash REQUIRED)

find_package(shaderc REQUIRED)

# TODO: These don't actually work for some reason :<
#set_property(TARGET proj PROPERTY CXX_STANDARD 14)
#set_property(TARGET proj PROPERTY CXX_STANDARD_REQUIRED ON)

if(NOT ANDROID)
    find_package(libunwind REQUIRED)
endif ()

# TODO: find_package-ify
add_subdirectory(externals/teakra)
set_target_properties(teakra_c PROPERTIES EXCLUDE_FROM_ALL 1)

add_subdirectory(source)
add_subdirectory(data/nand_archives)
if(NOT ANDROID)
    add_subdirectory(tools/3dsx_to_cia)
    #add_subdirectory(tools/embed_title)
    #add_subdirectory(tools/install_cia)
endif()
