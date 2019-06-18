cmake_minimum_required(VERSION 3.5)
project(tfunnel)

set(CMAKE_CXX_STANDARD 17)

add_definitions(-DBOOST_ENABLE_ASSERT_HANDLER -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")

    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost REQUIRED program_options system coroutine)
    add_executable(tfunnel src/main.cpp src/remote.cpp src/tcp_listen_loop.cpp src/udp_front_loop.cpp src/icmp_for_udp.cpp)
    target_compile_options(tfunnel PRIVATE -Wall)
    target_link_options(tfunnel PUBLIC -static)
    target_link_libraries(tfunnel Boost::program_options Boost::system Boost::coroutine)
    install(TARGETS tfunnel DESTINATION bin)
    install(SCRIPT CMakeInstallSetcap.txt)

else()

    message(WARNING "Compiling for non-Linux OS: only the proxy end of tfunnel will be built")
    if(NOT CMAKE_BUILD_TYPE)
        message(status "Setting build type to 'Release' as none was specified.")
        set(CMAKE_BUILD_TYPE Release)
    endif()

    find_package(Boost REQUIRED system coroutine)
    add_executable(tfunnel-proxy-only src/main.cpp src/remote.cpp)
    target_compile_definitions(tfunnel-proxy-only PRIVATE PROXY_ONLY=1)
    target_link_libraries(tfunnel-proxy-only Boost::system Boost::coroutine)

endif()
