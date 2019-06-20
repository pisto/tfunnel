#pragma once

#include <utility>
#include <memory>
#include <unordered_map>
#include <set>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <stdexcept>
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
	static const char* protoname;
};
//helper macro to specialize socket_opcodes
#define define_socket_opcodes(socket, n, d, c, name)\
        template<> struct socket_opcodes<socket> {\
            static constexpr const opcodes new_socket = n, data = d, close_socket = c;\
            static constexpr const char* protoname = name;\
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
	proxied_socket(uint64_t id) try : socket(asio, protocol_type::v6()), id(id) {
		if (verbose) collect_ostream(std::cerr) << description() << " : opened" << std::endl;
	}
	catch (const boost::system::system_error& e) {
		send_output(opcodes::close_socket, id);
		if (verbose) collect_ostream(std::cerr) << description() << " : aborted(" << e.what() << ')' << std::endl;
	}

	//constructor for client end, use a socket new socket and generate an id
	proxied_socket(socket&& s) try : socket(std::move(s)), id(index.v++) {
		auto ep = this->local_endpoint();
		new_connection_data ncdata{ ep.address().to_v6().to_bytes(), ep.port() };
		send_output(opcodes::new_socket, id, sizeof(ncdata), &ncdata);
		if (verbose) collect_ostream(std::cerr) << description() << " : opened" << std::endl;
	}
	catch (const boost::system::system_error& e) {
		send_output(opcodes::close_socket, id);
		if (verbose) collect_ostream(std::cerr) << description() << " : aborted" << std::endl;
	}

	virtual std::string description() {
		std::ostringstream ret;
		ret << opcodes::protoname << '(' << id << ") ";
		try {
			if (port) ret << try_cast_ipv4(this->remote_endpoint());
			else {
				auto port = this->local_endpoint().port();
				if (port) ret << "[proxy]:" << this->local_endpoint().port();
				else ret << "[proxy]:-";
			}
		}
		catch(const boost::system::system_error&) { ret << '-'; }
		ret << " => ";
		try { ret << try_cast_ipv4(port ? this->local_endpoint() : this->remote_endpoint()); }
		catch(const boost::system::system_error&) { ret << '-'; }
		return ret.str();
	}

	virtual void remember() {
		all.insert({ id, std::weak_ptr(this->shptr()) });
	}

	virtual void forget() {
		all.erase(uint64_t(id));
	}

	virtual void spawn_lifecycle(typename socket::endpoint_type remote) {
		boost::asio::spawn(lifecycle_strand, [this_ = this->shptr(), remote](boost::asio::yield_context yield) {
			if (!remote.address().is_unspecified()) try {
				this_->async_connect(remote, yield);
				if (verbose) collect_ostream(std::cerr) << this_->description() << " : connected" << std::endl;
			} catch (const boost::system::system_error& e) {
					if (e.code() == boost::asio::error::operation_aborted) return;
					if (verbose) collect_ostream(std::cerr) << this_->description() << " : connection failed("
					                                        << e.what() << ')' <<std::endl;
					return;
			}
			this_->on_connect();
			bool graceful_eof = false;
			try {
				while (1) {
					this_->async_wait(boost::asio::socket_base::wait_read, yield);
					size_t datalen = std::min(this_->available(), header::MAX_LEN);
					if (!this_->on_read(datalen, yield)) break;
					auto data = allocate_output(opcodes::data, this_->id, datalen);
					try {
						if (this_->receive(boost::asio::buffer(data, datalen)) != datalen)
							throw std::logic_error("socket receive returned less data than promised");
					} catch (const boost::system::system_error& e) {
						abort_output(datalen);
						if (!datalen && e.code() == boost::asio::error::eof) break;
						throw;
					} catch (...) {
						abort_output(datalen);
						throw;
					}
					commit_output();
					if (verbose) {
						if (port) collect_ostream(std::cerr) << this_->description() << " : dataread(" << datalen
						                                     << "=>)" << std::endl;
						else collect_ostream(std::cerr) << this_->description() << " : dataread(<=" << datalen << ')'
						                                << std::endl;
					}
				}
				graceful_eof = true;
			} catch (const boost::system::system_error& e) {
				if (e.code() == boost::asio::error::operation_aborted) return;
				if (verbose) collect_ostream(std::cerr) << this_->description() << " : read error(" << e.what() << ')'
				                                        << std::endl;
			}
			this_->local_eof(graceful_eof, yield);
			if (graceful_eof) this_->shutdown(boost::asio::socket_base::shutdown_receive, ignore_ec);
			while (this_->got_remote_eof.blocked()) this_->got_remote_eof.async_wait(yield[ignore_ec]);
		});
	}

	virtual bool connected() {
		boost::system::error_code ec;
		auto endpoint = this->remote_endpoint(ec);
		return !ec && !endpoint.address().is_unspecified();
	}

	virtual void choke(bool on) {
		read_choked.blocked(on);
	}

	virtual void write(std::shared_ptr<char[]> data, size_t len) = 0;

	virtual void remote_eof(bool graceful) {
		if (!graceful) {
			forget();
			send_close_notice = false;
		}
		got_remote_eof.blocked(false);
	}

	virtual void kill() {
		forget();
		choke(false);
		this->cancel();
		got_remote_eof.blocked(false);
	}

	virtual ~proxied_socket() {
		all.erase(uint64_t(id));
		if (send_close_notice) send_output(opcodes::close_socket, id);
		if (verbose) collect_ostream(std::cerr) << description() << " : closed" << std::endl;
	}

protected:

	asio_semaphore got_remote_eof{ asio, true };

	template<typename C = proxied_socket> auto shptr() {
		return std::dynamic_pointer_cast<C>(this->shared_from_this());
	}

	virtual void on_connect() {}

	virtual bool on_read(size_t len, boost::asio::yield_context& yield) {
		if (dead) throw boost::system::system_error(boost::asio::error::operation_aborted);
		auto [generation, outstanding] = get_output_statistics();
		if (generation != last_output_generation) {
			my_outstanding = len;
			last_output_generation = generation;
		} else my_outstanding += len;
		//choke if output is close to buffer_size, and our output is significant (> 10%)
		if (outstanding > buffer_size / 2 && (10 * my_outstanding) / outstanding > 1) {
			choke(true);
			exec_on_new_output_generation([this_ = std::weak_ptr(shptr())]{
				auto locked = this_.lock();
				if (locked) locked->choke(false);
			});
		}
		bool blocked_once = false;
		while (read_choked.blocked()) {
			if (verbose && !blocked_once) collect_ostream(std::cerr) << description() << " : choked" << std::endl;
			blocked_once = true;
			read_choked.async_wait(yield[ignore_ec]);
		}
		if (verbose && blocked_once) collect_ostream(std::cerr) << description() << " : unchoked" << std::endl;
		if (dead) throw boost::system::system_error(boost::asio::error::operation_aborted);
		return true;
	};

	virtual void on_write(size_t len) {}

	virtual void local_eof(bool graceful, boost::asio::yield_context& yield) {
		if (verbose && graceful)
			collect_ostream(std::cerr) << description() << " : eof(" << (port ? "=>" : "<=") << ')' << std::endl;
	}

private:
	boost::asio::io_context::strand lifecycle_strand{ asio };
	bool dead = false, send_close_notice = true;
	uint64_t last_output_generation = 0;
	size_t my_outstanding = 0;
	asio_semaphore read_choked{ asio, false };
	static inline std::unordered_map<uint64_t, std::weak_ptr<proxied_socket>> all;
	static inline struct { uint64_t v : tfunnel::header::ID_BITS; } index;
};


/*
 * Proxied TCP connection. Adds on top of proxied_socket the following features:
 * - the write() methods for a guaranteed sequential write
 * - choking of the remote connection
 * - communicating the local EOF condition to the remote end
 */
define_socket_opcodes(boost::asio::ip::tcp::socket, TCP_NEW, TCP_DATA, TCP_CLOSE, "TCP");
struct proxied_tcp: proxied_socket<boost::asio::ip::tcp::socket> {

	using proxied_socket::proxied_socket;

	virtual void choke(bool on) override {
		proxied_socket::choke(force_choked || on);
	}

	virtual void choked_from_remote(bool on) {
		force_choked = on;
		choke(on);
	}

	virtual void write(std::shared_ptr<char[]> data, size_t len) override {
		writebuff_w.insert(writebuff_w.end(), data.get(), data.get() + len);
		commit_write();
	}

	virtual char* allocate_write(size_t len) {
		auto oldsize = writebuff_w.size();
		writebuff_w.resize(oldsize + len);
		return &writebuff_w[oldsize];
	}

	virtual void commit_write() {
		if (!remote_choked_read && writebuff_r.size() - writebuff_r_offset + writebuff_w.size() > buffer_size) {
			send_output(TCP_CHOKE, id);
			remote_choked_read = true;
		}
		if (!connected() || writer_active) return;
		std::swap(writebuff_r, writebuff_w);
		consume();
	}

	virtual void remote_eof(bool graceful) override {
		proxied_socket::remote_eof(graceful);
		graceful_remote_eof |= graceful;
		if (connected() && !writer_active) check_forward_eof();
	}

	virtual void kill() override {
		choked_from_remote(false);
		proxied_socket::kill();
	}

protected:

	virtual void on_connect() override {
		if (writebuff_w.empty()) {
			check_forward_eof();
			return;
		}
		std::swap(writebuff_r, writebuff_w);
		consume();
		proxied_socket::on_connect();
	}

	virtual bool on_read(size_t len, boost::asio::yield_context& yield) override {
		return proxied_socket::on_read(len, yield) && len;
	}

	virtual void local_eof(bool graceful, boost::asio::yield_context& yield) override {
		if (graceful) send_output(TCP_EOF, id);
		proxied_socket::local_eof(graceful, yield);
	}

private:
	bool writer_active = false, graceful_remote_eof = false, eof_forwarded = false;
	std::vector<char> writebuff_r, writebuff_w;
	size_t writebuff_r_offset = 0;
	bool remote_choked_read = false, force_choked = false;

	void check_forward_eof() {
		if (eof_forwarded || !connected() || got_remote_eof.blocked()) return;
		eof_forwarded = true;
		if (graceful_remote_eof) shutdown(boost::asio::socket_base::shutdown_send);
		else {
			kill();
			set_option(boost::asio::socket_base::linger(true, 0));
			close();
		}
	}

	void on_write_completed(const boost::system::error_code& ec, size_t len) {
		writer_active = false;
		if (ec == boost::asio::error::operation_aborted) return;
		if (ec) {
			if (verbose) collect_ostream(std::cerr) << description() << " : write error(" << ec.message() << ')'
			                                        << std::endl;
			if (!remote_choked_read) {
				send_output(TCP_CHOKE, id);
				remote_choked_read = true;
			}
			kill();
			return;
		}
		if (verbose) {
			if (port) collect_ostream(std::cerr) << description() << " : datawrite(<=" << len << ')' << std::endl;
			else collect_ostream(std::cerr) << description() << " : datawrite(" << len << "=>)" << std::endl;
		}
		on_write(len);
		writebuff_r_offset += len;
		if (remote_choked_read && writebuff_r.size() - writebuff_r_offset + writebuff_w.size() < buffer_size / 10) {
			send_output(TCP_UNCHOKE, id);
			remote_choked_read = false;
		}
		if (writebuff_r.size() - writebuff_r_offset > 0) {
			consume();
			return;
		}
		writebuff_r.clear();
		writebuff_r_offset = 0;
		std::swap(writebuff_r, writebuff_w);
		if (writebuff_r.size()) consume();
		else check_forward_eof();
	}

	void consume() {
		writer_active = true;
		this->async_write_some(
				boost::asio::buffer(writebuff_r.data() + writebuff_r_offset, writebuff_r.size() - writebuff_r_offset),
				[this_ = shptr<proxied_tcp>()](const boost::system::error_code& ec, size_t len) {
					this_->on_write_completed(ec, len);
				}
		);
	}

};


/*
 * Proxied UDP connection. Adds on top of proxied_socket the following features:
 * - write() to send a packet, die on first error
 */
define_socket_opcodes(boost::asio::ip::udp::socket, UDP_NEW, UDP_DATA, UDP_CLOSE, "UDP");
struct proxied_udp: proxied_socket<boost::asio::ip::udp::socket> {

	using proxied_socket::proxied_socket;

	virtual void spawn_lifecycle(boost::asio::ip::udp::endpoint remote) override {
		if (!remote.address().is_unspecified()) {
			connect(remote);
			on_connect();
		}
		proxied_socket::spawn_lifecycle({});
	}

	virtual void write(std::shared_ptr<char[]> data, size_t len) override {
		async_send(boost::asio::buffer(data.get(), len),
				[data, this_ = shptr<proxied_udp>()](boost::system::error_code ec, size_t len) {
					if (ec == boost::asio::error::operation_aborted) return;
					if (ec) {
						this_->kill();
						if (verbose) collect_ostream(std::cerr) << this_->description() << " : write error("
						                                        << ec.message() << ')' << std::endl;
					} else this_->on_write(len);
				}
		);
	}

	virtual void remote_eof(bool graceful) override {
		if (!graceful) kill();
		proxied_socket::remote_eof(graceful);
	}

protected:
	virtual void local_eof(bool graceful, boost::asio::yield_context& yield) override {
		if (!graceful) kill();
		proxied_socket::local_eof(graceful, yield);
	}

};

}
