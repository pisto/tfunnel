#include <memory>
#include <vector>
#include <iostream>
#include <forward_list>
#include "env.hpp"
#include "protocol.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

//writing to remote
namespace {


std::vector<char> outbuff_r, outbuff_w;
size_t outbuff_r_offset = 0;
uint64_t generation = 0;
std::forward_list<std::function<void()>> new_generation_execs;

void new_output_generation() {
	outbuff_r.clear();
	outbuff_r_offset = 0;
	std::swap(outbuff_r, outbuff_w);
	generation++;
	for (auto& f: new_generation_execs) f();
	new_generation_execs.clear();
}

void on_output_write(boost::system::error_code ec, size_t len);

void consume_output() {
	auto size = outbuff_r.size() - outbuff_r_offset;
	//in case stdout is slow, block on write to avoid OOM
	if (size + outbuff_w.size() > buffer_size) {
		collect_ostream(std::cerr) << "Slow upload link to " << (port ? "proxy" : "client") << ", throttling"
		                           << std::endl;
		boost::system::error_code ec;
		write(output, buffer(outbuff_r.data() + outbuff_r_offset, size), ec);
		on_output_write(ec, size);
	} else output.async_write_some(buffer(outbuff_r.data() + outbuff_r_offset, size), on_output_write);
}

void on_output_write(boost::system::error_code ec, size_t len) {
	if (ec) {
		collect_ostream(std::cerr) << "Fatal error: cannot send to " << (port ? "proxy" : "client") << ": "
		                           << ec.message() << std::endl;
		_Exit(1);
	}
	outbuff_r_offset += len;
	if (outbuff_r.size() - outbuff_r_offset > 0) {
		consume_output();
		return;
	}
	new_output_generation();
	if (outbuff_r.size()) consume_output();
}

#ifdef DEBUG_MESSAGES
uint64_t sendid = 0;
#endif

}


void send_output(opcodes opcode, uint64_t id, uint16_t len, const void* data) {
	#ifdef DEBUG_MESSAGES
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
	commit_output();
}

char* allocate_output(opcodes opcode, uint64_t id, uint16_t len) {
	#ifdef DEBUG_MESSAGES
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
	auto oldsize = outbuff_w.size();
	outbuff_w.resize(oldsize + len);
	return &outbuff_w[oldsize];
}

void commit_output() {
	if (outbuff_r.size()) return;
	new_output_generation();
	consume_output();
}

void abort_output(uint16_t len) {
	outbuff_w.resize(outbuff_w.size() - sizeof(header) - len);
}

std::tuple<uint64_t, size_t> get_output_statistics() {
	return std::make_tuple(generation, outbuff_w.size());
}

void exec_on_new_output_generation(std::function<void()> f) {
	new_generation_execs.emplace_front(std::move(f));
}



//reading from remote
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
			typename proxied_socket_type::endpoint_type remote(ip::address_v6(ncdata.ipv6), ncdata.port);
			try {
				s = std::make_shared<proxied_socket_type>(uint64_t(h.id));
				s->remember();
				s->spawn_lifecycle(remote);
			} catch (const system_error& e) {
				collect_ostream(std::cerr) << s->description() << " : cannot open connection on proxy (" << e.what()
				                           << ')' << std::endl;
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
	if (h != header::handshake(!client)) throw std::runtime_error(client ? "proxy does not speak the same protocol"
	                                                                     : "client does not speak the same protocol");
	while (1) {
		async_read(input, buffer((void*)&h, sizeof(header)), yield);
		#ifdef DEBUG_MESSAGES
		static uint64_t recvid = 1;
		collect_ostream(std::cerr) << (port ? "cr(" : "pr(") << recvid++ << ',' << int(h.opcode) << ',' << h.id << ',' << h.len << ')' << std::endl;
		#endif
		switch (h.opcode) {
			case TCP_CHOKE:
			case TCP_UNCHOKE:
				if (auto socket = std::dynamic_pointer_cast<proxied_tcp>(proxied_tcp::find(h.id)))
					socket->choked_from_remote(h.opcode == TCP_CHOKE);
				break;
			case TCP_EOF:
				if (h.len) invalid_data(client);
				if (auto socket = proxied_tcp::find(h.id)) socket->remote_eof(true);
				break;
			case TCP_DATA: {
				posix::stream_descriptor::bytes_readable fionread;
				error_code ec;
				input.io_control(fionread, ec);
				if (!ec && fionread.get() >= h.len)
					if (auto socket = std::dynamic_pointer_cast<proxied_tcp>(proxied_tcp::find(h.id))) {
						auto data = socket->allocate_write(h.len);
						read(input, buffer(data, h.len));
						socket->commit_write();
						break;
					}
			}
			case TCP_NEW:
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
	if (e.code() == boost::asio::error::eof) _Exit(0);
	throw;
}


template void read_remote<false>(yield_context yield);
#ifndef PROXY_ONLY
template void read_remote<true>(yield_context yield);
#endif

}
