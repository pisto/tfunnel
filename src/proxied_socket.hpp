#pragma once

#include <utility>
#include <memory>
#include <unordered_map>
#include <set>
#include <cmath>
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
	proxied_socket(uint64_t id) try : socket(asio, protocol_type::v6()), id(id), strand_w(asio), strand_r(asio) {}
	catch (const boost::system::system_error& e) {
		send_packet(std::make_shared<header>(opcodes::close_socket, id, 0));
	}

	//constructor for client end, use a socket new socket and generate an id
	proxied_socket(socket&& s): socket(std::move(s)), id(id), strand_w(asio), strand_r(asio) {
		std::shared_ptr<char[]> packet(new char[sizeof(header) + sizeof(new_connection_data)]);
		*reinterpret_cast<header*>(packet.get()) = header(opcodes::new_socket, id, sizeof(new_connection_data));
		auto ep = this->local_endpoint();
		*reinterpret_cast<new_connection_data*>(packet.get() + sizeof(header)) = { ep.address().to_v6().to_bytes(), ep.port() };
		send_packet(std::move(packet));
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
				if (!remote.address().is_unspecified()) this_->async_connect(remote, yield);
				while (1) {
					this_->async_wait(boost::asio::ip::udp::socket::wait_read, yield);
					size_t datalen = std::min(this_->available(), header::MAX_LEN);
					std::shared_ptr<char[]> packet(new char[sizeof(header) + datalen]);
					datalen = this_->receive(boost::asio::buffer(packet.get() + sizeof(header), datalen));
					if (!this_->on_read(datalen)) break;
					*reinterpret_cast<header*>(packet.get()) = header(opcodes::data, this_->id, datalen);
					send_packet(std::move(packet));
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

	virtual void local_eof(bool graceful) {
		if (graceful) {
			hold_for_remote_eof.emplace(shptr());
			this->shutdown(boost::asio::socket_base::shutdown_receive, ignore_ec());
		} else this->close(ignore_ec());
	}

	virtual bool on_read(size_t len) = 0;
	virtual void on_write(size_t len) {}

	virtual ~proxied_socket() {
		all.erase(uint64_t(id));
		send_packet(std::make_shared<header>(opcodes::close_socket, id, 0));
	}

protected:
	boost::asio::io_context::strand strand_w, strand_r;

	template<typename C = proxied_socket> auto shptr() {
		return std::dynamic_pointer_cast<C>(this->shared_from_this());
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

	using proxied_socket::proxied_socket;

	virtual void write(std::shared_ptr<char[]> data, size_t len) override {
		boost::asio::async_write(*this, boost::asio::buffer(data.get(), len), boost::asio::bind_executor(strand_w,
				[data, this_ = shptr()](boost::system::error_code ec, size_t len) {
					if (ec == boost::asio::error::operation_aborted) return;
					if (ec) this_->local_eof(false);
					else this_->on_write(len);
				})
		);
	}

	virtual bool on_read(size_t len) override {
		return len ?: (local_eof(true), false);
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

	virtual void local_eof(bool graceful) override {
		if (graceful) {
			local_graceful_eof = true;
			send_packet(std::make_shared<header>(TCP_EOF, id, 0));
		}
		proxied_socket::local_eof(graceful);
	}

private:
	bool local_graceful_eof = false, remote_graceful_eof = false;

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
				[data, this_ = shptr()](boost::system::error_code ec, size_t len) {
					if (ec == boost::asio::error::operation_aborted) return;
					if (ec) this_->local_eof(false);
					else this_->on_write(len);
				})
		);
	}

	virtual bool on_read(size_t len) override { return true; }

};

}
