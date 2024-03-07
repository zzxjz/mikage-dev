add_executable(install_cia main.cpp)
target_link_libraries(install_cia PRIVATE framework platform cryptopp boost_program_options boost_filesystem boost_system)
target_link_libraries(install_cia PRIVATE fmt::fmt)
target_link_libraries(install_cia PRIVATE cryptopp::cryptopp-static)
