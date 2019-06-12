#include <memory>
#include <iostream>
#include "env.hpp"
#include "protocol.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

template<bool client> void read_remote(yield_context yield) try {
	//signature
	header h;
	async_read(input, buffer((void*)&h, sizeof(header)), yield);
	if (h != header::handshake(!client)) throw std::runtime_error("remote does not speak the same protocol");
	while (1) {
		//read header, some validation and read data
		std::shared_ptr<char[]> data;
		new_connection_data ncdata;
		async_read(input, buffer((void*)&h, sizeof(header)), yield);
		switch (h.opcode) {
			case DIE:
				if (client || h.id || h.len) goto invalid;
				break;
			case TCP_DATA:
			case UDP_DATA:
				data = std::shared_ptr<char[]>(new char[h.len]);
				async_read(input, buffer(data.get(), h.len), yield);
				break;
			case TCP_NEW:
			case UDP_NEW:
				if (client || h.len != sizeof(ncdata)) goto invalid;
				async_read(input, buffer((void*)&ncdata, sizeof(ncdata)), yield);
				break;
			default: if (h.len) goto invalid;
		}

		if (h.opcode == DIE) _Exit(0);
		else if (TCP_OPS_START <= h.opcode && h.opcode < TCP_OPS_END) {
			auto socket = proxied_tcp::find(h.id);
			if (h.opcode == TCP_NEW) {
				if (socket) goto invalid;
				auto proxied = std::make_shared<proxied_tcp>(ip::tcp::socket(asio), uint64_t(h.id));
				proxied->remember();
				proxied->spawn_read({ ip::address_v6(ncdata.ipv6), ncdata.port });
			}
			else if (!socket);
			else if (h.opcode == TCP_DATA) socket->write(std::move(data), h.len);
			else if (h.opcode == TCP_EOF) socket->remote_eof(true);
			else if (h.opcode == TCP_CLOSE) socket->remote_eof(false);
			else goto invalid;
			continue;
		}
		else if (UDP_OPS_START <= h.opcode && h.opcode < UDP_OPS_END) {
			auto socket = proxied_udp::find(h.id);
			if (h.opcode == UDP_NEW) {
				if (socket) goto invalid;
				auto proxied = std::make_shared<proxied_udp>(ip::udp::socket(asio), uint64_t(h.id));
				proxied->remember();
				proxied->spawn_read({ ip::address_v6(ncdata.ipv6), ncdata.port });
				continue;
			}
			else if (!socket);
			else if (h.opcode == UDP_DATA) socket->write(std::move(data), h.len);
			else if (h.opcode == UDP_CLOSE) {
				if (!client) socket->remote_eof(false);
				else {
					//do not close client udp socket to avoid reopening another immediately if the client keeps retrying, warn the user
					auto local = socket->local_endpoint(), remote = socket->remote_endpoint();
					try {
						auto local4 = ip::make_address_v4(ip::v4_mapped, local.address().to_v6());
						auto remote4 = ip::make_address_v4(ip::v4_mapped, remote.address().to_v6());
						collect_ostream(std::cerr) << "Warning: proxy stopped forwarding UDP traffic "
						                           << remote4 << ':' << remote.port() << " <=> "
						                           << local4 << ':' << local.port() << std::endl;
					} catch (const ip::bad_address_cast&) {
						auto local6 = local.address().to_v6(), remote6 = local.address().to_v6();
						collect_ostream(std::cerr) << "Warning: proxy stopped forwarding UDP traffic ["
						                           << remote6 << "]:" << remote.port() << " <=> ["
						                           << local6 << "]:" << local.port() << std::endl;
					}
				}
			} else goto invalid;
			continue;
		}
		invalid: throw std::logic_error("invalid data from remote");
	}
} catch (const system_error& e) {
	if (e.code() == boost::asio::error::operation_aborted) return;
	if (e.code() == boost::asio::error::eof) {
		if (client) throw system_error(e.code(), "the proxy closed the connection");
		asio.stop();
		return;
	}
	throw;
}

template void read_remote<false>(yield_context yield);
#ifndef PROXY_ONLY
template void read_remote<true>(yield_context yield);
#endif

}
