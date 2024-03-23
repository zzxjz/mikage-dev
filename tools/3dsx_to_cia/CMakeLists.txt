add_executable(3dsx_to_cia main.cpp)
target_link_libraries(3dsx_to_cia PRIVATE framework platform cryptopp::cryptopp Boost::boost Boost::program_options)
