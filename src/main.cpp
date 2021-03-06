#include <string>
#include <iostream>
#include <fstream>
#include <cmath>
#include <fcntl.h>
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

//configuration
namespace tfunnel {
uint16_t port, udp_timeout, udp_timeout_stream;
uint32_t buffer_size;
bool verbose = false;
}

int main(int argc, char** argv) try {
	using namespace tfunnel;
	signal(SIGHUP, SIG_IGN);
	{
		#ifndef PROXY_ONLY
		//default to netfilter NAT values if available
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout") >> udp_timeout)) udp_timeout = 30;
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout_stream") >> udp_timeout_stream))
			udp_timeout_stream = 120;
		#endif
		using namespace boost::program_options;
		options_description options("tfunnel options");
		options.add_options()
				("verbose,v", "enable verbose output")
				#ifndef PROXY_ONLY
				(",p", value(&port)->default_value(0), "start in client mode and listen on this port")
				("udp_timeout", value(&udp_timeout)->default_value(udp_timeout),
						"timeout for unanswered UDP connections")
				("udp_timeout_stream", value(&udp_timeout_stream)->default_value(udp_timeout_stream),
						"timeout for answered UDP connections")
				#endif
				("buffer_size", value(&tfunnel::buffer_size)->default_value(10 * 1024 * 1024), "buffer size limit")
				("help", "print help");
		variables_map vm;
		store(parse_command_line(argc, argv, options), vm);
		notify(vm);
		if (vm.count("help")) {
			std::cout << options;
			return 0;
		}
		if (vm.count("verbose")) verbose = true;
		if (tfunnel::buffer_size < 64 * 1024) {
			collect_ostream(std::cerr) << "buffer_size is too low, setting to 64KB" << std::endl;
			tfunnel::buffer_size = 64 * 1024;
		}
	}

	io_context::strand input_strand(asio);
#if defined(F_GETPIPE_SZ) && defined(F_SETPIPE_SZ)
	auto setpipesize = [](int fd) {
		if (auto currsize = fcntl(fd, F_GETPIPE_SZ);
				currsize > 0 && uint32_t(currsize) < std::min(tfunnel::buffer_size, uint32_t(INT_MAX)))
			fcntl(fd, F_SETPIPE_SZ, int(tfunnel::buffer_size));
	};
	setpipesize(output.native_handle());
	setpipesize(input.native_handle());
#endif
#ifndef PROXY_ONLY
	if (port) {
		header h = header::handshake(true);
		send_output(opcodes(h.opcode), h.id);
		spawn(input_strand, read_remote<true>);
		io_context::strand tcp_listen_strand(asio), udp_front_strand(asio);
		spawn(tcp_listen_strand, tcp_listen_loop);
		spawn(udp_front_strand, udp_front_loop);
		asio.run();
	} else {
#else
	{
#endif
		header h = header::handshake(false);
		send_output(opcodes(h.opcode), h.id);
		spawn(input_strand, read_remote<false>);
		asio.run();
	}
	_Exit(0);
} catch (const std::exception& e) {
	collect_ostream(std::cerr) << "Fatal error in " << (tfunnel::port ? "client" : "proxy") << ": " << e.what()
	                           << std::endl;
	_Exit(1);
}
