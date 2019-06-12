#include <string>
#include <iostream>
#include <fstream>
#include <boost/program_options.hpp>
#include <boost/asio.hpp>
#include "env.hpp"

namespace boost {
using namespace std;
void assertion_failed(const char* expr, const char* function, const char* file, long line) {
	throw logic_error("Boost assert failed: "s + expr + ", at " + file + ":" + to_string(line) + " in " + function);
}

void assertion_failed_msg(const char* expr, const char* msg, const char* function, const char* file, long line) {
	throw logic_error(
			"Boost assert failed ("s + msg + "): " + "" + expr + ", at " + file + ":" + to_string(line) + " in " +
			function);
}
}

using namespace boost::asio;

io_context asio;
posix::stream_descriptor input(asio, STDIN_FILENO), output(asio, STDOUT_FILENO);
io_context::strand input_strand(asio), output_strand(asio);

//configuration
namespace tfunnel {
uint16_t port, udp_timeout, udp_timeout_stream;
}

int main(int argc, char** argv) try {
	using namespace tfunnel;
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
#ifndef PROXY_ONLY
	{
		//default to netfilter NAT values if available
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout") >> udp_timeout)) udp_timeout = 30;
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout_stream") >> udp_timeout_stream))
			udp_timeout_stream = 120;
		using namespace boost::program_options;
		options_description options("tfunnel options");
		options.add_options()
				(",p", value(&port)->default_value(0), "start in client mode and listen on this port")
				("udp_timeout", value(&udp_timeout)->default_value(udp_timeout),
						"timeout for unanswered UDP connections")
				("udp_timeout_stream", value(&udp_timeout_stream)->default_value(udp_timeout_stream),
						"timeout for answered UDP connections")
				("help", "print help");
		variables_map vm;
		store(parse_command_line(argc, argv, options), vm);
		notify(vm);
		if (vm.count("help")) {
			std::cout << options;
			return 0;
		}
	}

	if (port) {
		send_packet(std::make_shared<header>(header::handshake(true)));
		spawn(input_strand, read_remote<true>);
		io_context::strand tcp_listen_strand(asio), udp_front_strand(asio);
		spawn(tcp_listen_strand, tcp_listen_loop);
		spawn(udp_front_strand, udp_front_loop);
		signal_set signals(asio, SIGINT, SIGTERM);
		signals.async_wait([](const boost::system::error_code&, int) {
			send_packet(std::make_shared<header>(DIE, 0, 0));
			post(output_strand, [] { _Exit(0); });
		});
		asio.run();
	} else {
#else
	{
#endif
		send_packet(std::make_shared<header>(header::handshake(false)));
		spawn(input_strand, read_remote<false>);
		asio.run();
	}
	_Exit(0);
} catch (const std::exception& e) {
	collect_ostream(std::cerr) << "Fatal error: " << e.what() << std::endl;
	_Exit(1);
}
