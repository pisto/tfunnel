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
extern boost::asio::io_context::strand input_strand, output_strand;

namespace tfunnel {

extern uint16_t port, udp_timeout, udp_timeout_stream;

void tcp_listen_loop(boost::asio::yield_context yield);
void udp_front_loop(boost::asio::yield_context yield);

template<bool client> void read_remote(boost::asio::yield_context yield);

template<typename T> void send_packet(std::shared_ptr<T> data) {
	using namespace boost::asio;
	using boost::system::error_code;
	auto h = reinterpret_cast<header*>(data.get());
	async_write(output, buffer(data.get(), h->len + sizeof(header)), bind_executor(output_strand,
			[data](error_code ec, size_t) {
				if (!ec) return;
				collect_ostream(std::cerr) << "Fatal error: cannot send to remote: " << ec.message() << std::endl;
				_Exit(1);
			}));
}

}
