cmake_minimum_required(VERSION 3.5)
project(tfunnel)

set(CMAKE_CXX_STANDARD 17)

add_definitions(-DBOOST_ENABLE_ASSERT_HANDLER -DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED program_options system coroutine)
include_directories(${Boost_INCLUDE_DIRS})

add_executable(tfunnel src/main.cpp src/read_remote.cpp src/tcp_listen_loop.cpp src/udp_front_loop.cpp)
target_link_libraries(tfunnel -pthread -static Boost::program_options Boost::system Boost::coroutine)

add_executable(tfunnel-proxy-only EXCLUDE_FROM_ALL src/main.cpp src/read_remote.cpp)
target_compile_definitions(tfunnel-proxy-only PRIVATE PROXY_ONLY=1)
target_compile_options(tfunnel-proxy-only PRIVATE -Os)
target_link_libraries(tfunnel-proxy-only -s -pthread -static Boost::system Boost::coroutine)
