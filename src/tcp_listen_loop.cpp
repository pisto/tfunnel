#include <iostream>
#include <netinet/in.h>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "env.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

namespace {
template<typename socket> bool sockopt(socket& s, int level, int opt) {
	int fd = s.native_handle(), yes = 1;
	return !setsockopt(fd, level, opt, &yes, sizeof(yes));
};
}

void tcp_listen_loop(yield_context yield) {
	ip::tcp::acceptor tcp_listen(asio, ip::tcp::endpoint(ip::tcp::v6(), port));
	if (!sockopt(tcp_listen, SOL_IPV6, IPV6_TRANSPARENT))
		throw std::system_error(errno, std::generic_category(), "cannot set option IPV6_TRANSPARENT on TCP socket");
	/*
	 * TCP accept loop. Nothing complicated here, except error handling (no idea about all the possible
	 * errors returned).
	 */
	while (1)
		try {
			ip::tcp::socket accepted(asio);
			tcp_listen.async_accept(accepted, yield);
			auto proxied = std::make_shared<proxied_tcp>(std::move(accepted));
			proxied->remember();
			proxied->spawn_read();
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
					collect_ostream(std::cerr) << "error in new TCP connection: " << e.what() << std::endl;
					break;
				default: throw;
			}
		}
}

}
