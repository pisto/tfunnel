cmake_minimum_required(VERSION 3.5)
project(tfunnel)

set(CMAKE_CXX_STANDARD 17)

add_definitions(-DBOOST_ENABLE_ASSERT_HANDLER -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")

    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost REQUIRED program_options system coroutine)
    add_executable(tfunnel src/main.cpp src/read_remote.cpp src/tcp_listen_loop.cpp src/udp_front_loop.cpp src/icmp_for_udp.cpp)
    target_link_libraries(tfunnel -static Boost::program_options Boost::system Boost::coroutine)
    target_compile_options(tfunnel PRIVATE -Wall)

else()

    message(WARNING "Compiling for non-Linux OS: only the proxy end of tfunnel will be built")
    if(NOT CMAKE_BUILD_TYPE)
        message(status "Setting build type to 'Release' as none was specified.")
        set(CMAKE_BUILD_TYPE Release)
    endif()

    find_package(Boost REQUIRED system coroutine)
    add_executable(tfunnel-proxy-only src/main.cpp src/read_remote.cpp)
    target_compile_definitions(tfunnel-proxy-only PRIVATE PROXY_ONLY=1)
    target_link_libraries(tfunnel-proxy-only Boost::system Boost::coroutine)

endif()
