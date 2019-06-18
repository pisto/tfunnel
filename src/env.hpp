#pragma once

#include <cstdlib>
#include <memory>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "utils.hpp"
#include "protocol.hpp"

extern boost::asio::io_context asio;
extern boost::asio::posix::stream_descriptor input, output;

namespace tfunnel {

extern uint16_t port, udp_timeout, udp_timeout_stream;
extern bool verbose;

void tcp_listen_loop(boost::asio::yield_context yield);
void udp_front_loop(boost::asio::yield_context yield);

template<bool client> void read_remote(boost::asio::yield_context yield);

void send_output(opcodes opcode, uint64_t id, uint16_t len, const void* data);
inline void send_output(opcodes opcode, uint64_t id) { send_output(opcode, id, 0, 0); }
char* allocate_output(opcodes opcode, uint64_t id, uint16_t len);
void commit_output();

void send_udp_port_unreachable(boost::asio::io_context::strand& strand,
                               boost::asio::ip::udp::endpoint local,
                               boost::asio::ip::udp::endpoint remote);

}
