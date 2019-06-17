#pragma once

#include <utility>
#include <memory>
#include <unordered_map>
#include <set>
#include <cmath>
#include <chrono>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "env.hpp"
#include "protocol.hpp"

namespace tfunnel {

/*
 * Proxied socket base implementation. Handles registration with the remote and reads (spawn a coroutine),
 * handles eof/close from local/remote.
 *
 * This template should be inherited from the actual proxied socket concrete class. The template parameter 'socket'
 * is the concrete Boost.Asio socket type. You must provide a specialization for the traits socket_opcodes<socket>
 * with the protocol opcodes for a new socket (new_socket), data (data), and socket close (close_socket).
 */

template<typename socket> struct socket_opcodes {
	//define these
	static const opcodes new_socket, data, close_socket;
};
//helper macro to specialize socket_opcodes
#define define_socket_opcodes(socket, n, d, c)\
        template<> struct socket_opcodes<socket> {\
            static constexpr const opcodes new_socket = n, data = d, close_socket = c;\
        }

template<typename socket> struct proxied_socket: std::enable_shared_from_this<proxied_socket<socket>>, socket {

	using opcodes = socket_opcodes<socket>;
	using socket_type = socket;
	using protocol_type = typename socket::protocol_type;
	const uint64_t id : tfunnel::header::ID_BITS;

	static auto find(uint64_t id) {
		auto found = all.find(id);
		return found != all.end() ? found->second.lock() : nullptr;
	}

	//constructor for proxy end, set the id and create a new socket
	proxied_socket(uint64_t id) try : socket(asio, protocol_type::v6()), id(id), strand_w(asio), strand_r(asio),
	                                  connected(false) {}
	catch (const boost::system::system_error& e) {
		send_output(opcodes::close_socket, id);
	}

	//constructor for client end, use a socket new socket and generate an id
	proxied_socket(socket&& s): socket(std::move(s)), id(index.v++), strand_w(asio), strand_r(asio) {
		auto ep = this->local_endpoint();
		new_connection_data ncdata{ ep.address().to_v6().to_bytes(), ep.port() };
		send_output(opcodes::new_socket, id, sizeof(ncdata), &ncdata );
		boost::system::error_code ec;
		this->remote_endpoint(ec);
		if ((connected = !ec)) on_connect();
	}

	virtual void remember() {
		all.insert({ id, std::weak_ptr(this->shptr()) });
	}

	virtual void forget() {
		all.erase(uint64_t(id));
	}

	void spawn_connect_read(typename socket::endpoint_type remote) {
		boost::asio::spawn(strand_r, [this_ = this->shptr(), remote](boost::asio::yield_context yield) {
			try {
				if (!remote.address().is_unspecified()) {
					this_->async_connect(remote, yield);
					this_->connected = true;
					this_->on_connect();
				}
				while (1) {
					this_->async_wait(boost::asio::socket_base::wait_read, yield);
					size_t datalen = std::min(this_->available(), header::MAX_LEN);
					std::shared_ptr<char[]> packet(new char[datalen]);
					datalen = this_->receive(boost::asio::buffer(packet.get(), datalen));
					if (!this_->on_read(datalen)) break;
					send_output(opcodes::data, this_->id, datalen, packet.get());
				}
			} catch (const boost::system::system_error& e) {
				if (e.code() != boost::asio::error::operation_aborted)
					this_->local_eof(e.code() == boost::asio::error::eof);
			}
		});
	}

	virtual void write(std::shared_ptr<char[]> data, size_t len) = 0;

	virtual void remote_eof(bool graceful) {
		boost::asio::post(strand_w, [graceful, this_ = shptr()] {
			hold_for_remote_eof.erase(this_);
			if (graceful) this_->shutdown(boost::asio::socket_base::shutdown_send, ignore_ec());
			else this_->close(ignore_ec());
		});
		if (!graceful) forget();
	}

	virtual ~proxied_socket() {
		all.erase(uint64_t(id));
		send_output(opcodes::close_socket, id);
	}

protected:
	boost::asio::io_context::strand strand_w, strand_r;
	bool connected;

	template<typename C = proxied_socket> auto shptr() {
		return std::dynamic_pointer_cast<C>(this->shared_from_this());
	}

	virtual void on_connect() {}
	virtual bool on_read(size_t len) = 0;
	virtual void on_write(size_t len) {}

	virtual void local_eof(bool graceful) {
		if (graceful) {
			hold_for_remote_eof.emplace(shptr());
			this->shutdown(boost::asio::socket_base::shutdown_receive, ignore_ec());
		} else this->close(ignore_ec());
	}

private:
	static inline std::set<std::shared_ptr<proxied_socket>> hold_for_remote_eof;
	static inline std::unordered_map<uint64_t, std::weak_ptr<proxied_socket>> all;
	static inline struct { uint64_t v : tfunnel::header::ID_BITS; } index;
};


/*
 * Proxied TCP connection. Adds on top of proxied_socket the following features:
 * - the write() method, to transfer a full buffer with boost::asio::async_write()
 * - communicating the local EOF condition to the remote end
 */
define_socket_opcodes(boost::asio::ip::tcp::socket, TCP_NEW, TCP_DATA, TCP_CLOSE);
struct proxied_tcp: proxied_socket<boost::asio::ip::tcp::socket> {

	proxied_tcp(uint64_t id): proxied_socket(id) {
		hold_strand_w.async_wait(boost::asio::bind_executor(strand_w, +[](boost::system::error_code){}));
	}

	proxied_tcp(boost::asio::ip::tcp::socket&& s): proxied_socket(std::move(s)) {
		hold_strand_w.async_wait(boost::asio::bind_executor(strand_w, +[](boost::system::error_code){}));
	}

	virtual void write(std::shared_ptr<char[]> data, size_t len) override {
		writebuff_w.insert(writebuff_w.end(), data.get(), data.get() + len);
		if (!connected || writebuff_r.size()) return;
		hold_strand_w.async_wait(boost::asio::bind_executor(strand_w, +[](boost::system::error_code){}));
		std::swap(writebuff_r, writebuff_w);
		consume();
	}

	virtual void remote_eof(bool graceful) override {
		if (!graceful && !(local_graceful_eof && remote_graceful_eof))
			//cause a TCP RST
			boost::asio::post(strand_w, [this_ = shptr()] {
				this_->set_option(boost::asio::socket_base::linger(true, 0), ignore_ec());
			});
		else remote_graceful_eof = true;
		proxied_socket::remote_eof(graceful);
	}

protected:

	virtual void on_connect() override {
		if (!writebuff_w.size()) {
			hold_strand_w.cancel();
			return;
		}
		std::swap(writebuff_r, writebuff_w);
		consume();
	}

	virtual bool on_read(size_t len) override {
		return len ?: (local_eof(true), false);
	}

	virtual void local_eof(bool graceful) override {
		if (graceful) {
			local_graceful_eof = true;
			send_output(TCP_EOF, id);
		}
		if (!graceful) hold_strand_w.cancel();
		proxied_socket::local_eof(graceful);
	}

private:
	bool local_graceful_eof = false, remote_graceful_eof = false;
	//XXX ugly hack to keep strand_w busy during sequential writes
	boost::asio::steady_timer hold_strand_w{ asio, std::chrono::hours(1000000) };
	std::vector<char> writebuff_r, writebuff_w;
	size_t writebuff_r_offset = 0;

	void on_send(boost::system::error_code ec, size_t len) {
		if (ec) {
			if (ec != boost::asio::error::operation_aborted) local_eof(false);
			writebuff_r.clear();
			writebuff_w.clear();
			writebuff_r_offset = 0;
			hold_strand_w.cancel();
			return;
		}
		on_write(len);
		writebuff_r_offset += len;
		if (writebuff_r.size() - writebuff_r_offset > 0) {
			consume();
			return;
		}
		writebuff_r.clear();
		writebuff_r_offset = 0;
		std::swap(writebuff_r, writebuff_w);
		if (writebuff_r.size()) consume();
		else hold_strand_w.cancel();
	}

	void consume() {
		this->async_write_some(
				boost::asio::buffer(writebuff_r.data() + writebuff_r_offset, writebuff_r.size() - writebuff_r_offset),
				[this_ = shptr<proxied_tcp>()](boost::system::error_code ec, size_t len) { this_->on_send(ec, len); }
		);
	}

};


/*
 * Proxied UDP connection. Adds on top of proxied_socket the following features:
 * - write() to send a packet
 */
define_socket_opcodes(boost::asio::ip::udp::socket, UDP_NEW, UDP_DATA, UDP_CLOSE);
struct proxied_udp: proxied_socket<boost::asio::ip::udp::socket> {

	using proxied_socket::proxied_socket;

	virtual void write(std::shared_ptr<char[]> data, size_t len) override {
		async_send(boost::asio::buffer(data.get(), len), boost::asio::bind_executor(strand_w,
				[data, this_ = shptr<proxied_udp>()](boost::system::error_code ec, size_t len) {
					if (ec == boost::asio::error::operation_aborted) return;
					if (ec) this_->local_eof(false);
					else this_->on_write(len);
				})
		);
	}

protected:
	virtual bool on_read(size_t len) override { return true; }

};

}
