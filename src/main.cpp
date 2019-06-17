#include <string>
#include <iostream>
#include <fstream>
#include <cmath>
#include <vector>
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
bool verbose = false;

static std::vector<char> outbuff_r, outbuff_w;
static size_t outbuff_r_offset = 0;
static void consume_output();
static void on_output_write(boost::system::error_code ec, size_t len) {
	if (ec) {
		collect_ostream(std::cerr) << "Fatal error: cannot send to remote: " << ec.message() << std::endl;
		_Exit(1);
	}
	outbuff_r_offset += len;
	if (outbuff_r.size() - outbuff_r_offset > 0) {
		consume_output();
		return;
	}
	outbuff_r.clear();
	outbuff_r_offset = 0;
	std::swap(outbuff_r, outbuff_w);
	if (outbuff_r.size()) consume_output();
}
static void consume_output() {
	auto size = outbuff_r.size() - outbuff_r_offset;
	//in case stdout is slow, block on write to avoid OOM
	if (size + outbuff_w.size() > 10 * 1024 * 1024) {
		collect_ostream(std::cerr) << "Remote is slow, throttling" << std::endl;
		boost::system::error_code ec;
		write(output, buffer(outbuff_r.data() + outbuff_r_offset, size), ec);
		on_output_write(ec, size);
	} else output.async_write_some(buffer(outbuff_r.data() + outbuff_r_offset, size), on_output_write);
}

void send_output(opcodes opcode, uint64_t id, uint16_t len, const void* data) {
	#if 0
	static uint64_t sendid = 0;
	collect_ostream(std::cerr) << (port ? "cs(" : "ps(") << sendid++ << ',' << int(opcode) << ',' << id << ',' << len << ')' << std::endl;
	#endif
	union {
		header h;
		char buff[0];
	} h;
	h.h.opcode = opcode;
	h.h.id = id;
	h.h.len = len;
	outbuff_w.insert(outbuff_w.end(), h.buff, h.buff + sizeof(h));
	if (len) outbuff_w.insert(outbuff_w.end(), (const char*)data, len + (const char*)data);
	if (outbuff_r.size()) return;
	std::swap(outbuff_r, outbuff_w);
	consume_output();
}

}

int main(int argc, char** argv) try {
	using namespace tfunnel;
	signal(SIGHUP, SIG_IGN);
#ifndef PROXY_ONLY
	{
		//default to netfilter NAT values if available
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout") >> udp_timeout)) udp_timeout = 30;
		if (!(std::ifstream("/proc/sys/net/netfilter/nf_conntrack_udp_timeout_stream") >> udp_timeout_stream))
			udp_timeout_stream = 120;
		using namespace boost::program_options;
		options_description options("tfunnel options");
		options.add_options()
				("verbose,v", "enable verbose output")
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
		if (vm.count("verbose")) verbose = true;
	}

	io_context::strand input_strand(asio);
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
	collect_ostream(std::cerr) << "Fatal error: " << e.what() << std::endl;
	_Exit(1);
}
