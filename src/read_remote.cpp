#include <memory>
#include <iostream>
#include "env.hpp"
#include "protocol.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

namespace {

void invalid_data(bool client) {
	throw std::logic_error(client ? "invalid data from proxy" : "invalid data from client");
}

template<typename proxied_socket_type>
void handle_new_data_close(bool client, const header& h, yield_context yield) {
	using ops = typename proxied_socket_type::opcodes;

	auto s = proxied_socket_type::find(h.id);
	switch (h.opcode) {
		case ops::new_socket:
		{
			new_connection_data ncdata;
			if (client || h.len != sizeof(ncdata) || s) invalid_data(client);
			async_read(input, buffer((void*)&ncdata, sizeof(ncdata)), yield);
			try {
				s = std::make_shared<proxied_socket_type>(uint64_t(h.id));
				s->remember();
				s->spawn_connect_read({ ip::address_v6(ncdata.ipv6), ncdata.port });
			}
			catch (const system_error& e) {
				collect_ostream(std::cerr) << "Warning: cannot open socket on proxy: " << e.what() << std::endl;
				return;
			}
		}
			break;
		case ops::data:
		{
			std::shared_ptr<char[]> data(new char[h.len]);
			async_read(input, buffer(data.get(), h.len), yield);
			if (s) s->write(std::move(data), h.len);
		}
			break;
		case ops::close_socket:
			if (s) s->remote_eof(false);
			break;
		default: invalid_data(client);
	}
}

}

template<bool client> void read_remote(yield_context yield) try {
	header h;
	async_read(input, buffer((void*)&h, sizeof(header)), yield);
	if (h != header::handshake(!client)) throw std::runtime_error("remote does not speak the same protocol");
	while (1) {
		async_read(input, buffer((void*)&h, sizeof(header)), yield);
		switch (h.opcode) {
			case DIE:
				if (client || h.id || h.len) invalid_data(client);
				_Exit(0);
			case TCP_EOF: {
				if (h.len) invalid_data(client);
				auto socket = proxied_tcp::find(h.id);
				if (socket) socket->remote_eof(true);
				break;
			}
			case TCP_NEW:
			case TCP_DATA:
			case TCP_CLOSE:
				handle_new_data_close<proxied_tcp>(client, h, yield);
				break;
			case UDP_NEW:
			case UDP_DATA:
			case UDP_CLOSE:
				handle_new_data_close<proxied_udp>(client, h, yield);
				break;
			default: invalid_data(client);
		}
	}
} catch (const system_error& e) {
	if (e.code() == boost::asio::error::operation_aborted) return;
	if (e.code() == boost::asio::error::eof) {
		if (client) throw system_error(e.code(), "the proxy closed the connection");
		_Exit(0);
	}
	throw;
}


template void read_remote<false>(yield_context yield);
#ifndef PROXY_ONLY
template void read_remote<true>(yield_context yield);
#endif

}
