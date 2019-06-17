#include <iostream>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "env.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

void tcp_listen_loop(yield_context yield) {
	ip::tcp::acceptor tcp_listen(asio, ip::tcp::v6());
	tcp_listen.set_option(socket_base::reuse_address(true));
	if (!setsockopt(tcp_listen, SOL_SOCKET, SO_MARK, 3))
		throw system_error(errno, generic_category(), "cannot set fwmark=3 on TCP listen socket");
	if (!setsockopt(tcp_listen, SOL_SOCKET, SO_REUSEPORT))
		throw system_error(errno, generic_category(), "cannot set SO_REUSEPORT on TCP listen socket");
	if (!setsockopt(tcp_listen, SOL_IPV6, IPV6_TRANSPARENT))
		throw system_error(errno, generic_category(), "cannot set IPV6_TRANSPARENT on TCP listen socket");
	tcp_listen.bind(ip::tcp::endpoint(ip::tcp::v6(), port));
	tcp_listen.listen();
	/*
	 * TCP accept loop. Nothing complicated here, except error handling (no idea about all the possible
	 * errors returned).
	 */
	while (1)
		try {
			ip::tcp::socket accepted(asio);
			tcp_listen.async_accept(accepted, yield);
			auto remote = accepted.remote_endpoint(), local = accepted.local_endpoint();
			try {
				if (!setsockopt(accepted, SOL_SOCKET, SO_MARK, 3))
					throw system_error(errno, generic_category(), "cannot set fwmark=3");
				auto proxied = std::make_shared<proxied_tcp>(std::move(accepted));
				proxied->remember();
				proxied->spawn_connect_read({});
				if (verbose) collect_ostream(std::cerr) << "TCP " << try_cast_ipv4(remote) << " => "
				                                        << try_cast_ipv4(local) << " : opened" << std::endl;
			} catch (const system_error& e) {
				collect_ostream(std::cerr) << "TCP " << try_cast_ipv4(remote) << " => " << try_cast_ipv4(local)
				                           << " : error opening (" << e.what() << ')' << std::endl;
				continue;
			}
		} catch (const system_error& e) {
			using namespace boost::asio::error;
			switch (e.code().value()) {
				case operation_aborted: return;
				case no_descriptors:
				case no_permission:
				case connection_aborted:
				case network_down:
				case host_unreachable:
				case network_unreachable:
					collect_ostream(std::cerr) << "TCP : cannot accept connection (" << e.what() << ')' << std::endl;
					break;
				default: throw;
			}
		}
}

}
