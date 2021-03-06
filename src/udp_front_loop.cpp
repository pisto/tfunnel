#include <iostream>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "env.hpp"
#include "proxied_socket.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

namespace {

/*
 * Proxied UDP connection for the client. Adds on top of proxied_udp the following features:
 * - lookup by local and remote peers (see comment below in udp_front_loop)
 * - timeout for unused connections
 */

struct proxied_udp_client: proxied_udp {

	using endpoints_tuple = std::tuple<ip::udp::endpoint, ip::udp::endpoint>;
	static inline std::map<endpoints_tuple, std::weak_ptr<proxied_udp_client>> tuples_all;

	static auto find(const ip::udp::endpoint& local, const ip::udp::endpoint& remote) {
		auto found = tuples_all.find(endpoints_tuple{ local, remote });
		return found != tuples_all.end() ? found->second.lock() : nullptr;
	}

	const endpoints_tuple endpoints;
	using clock = std::chrono::steady_clock;
	using timepoint = std::chrono::time_point<clock>;
	timepoint last_activity = clock::now();
	bool is_stream = false;
	steady_timer timeout{ asio, std::chrono::seconds(udp_timeout_stream) };

	proxied_udp_client(ip::udp::socket&& s): proxied_udp(std::move(s)),
	                                         endpoints(local_endpoint(), remote_endpoint()) {}

	void spawn_lifecycle(ip::udp::endpoint remote) override {
		wait_timeout();
		proxied_udp::spawn_lifecycle(remote);
	}

	void remember() override {
		proxied_udp::remember();
		tuples_all.emplace(endpoints, std::weak_ptr(shptr<proxied_udp_client>()));
	}

	void forget() override {
		tuples_all.erase(endpoints);
		proxied_udp::forget();
	}

	bool on_read(size_t len, boost::asio::yield_context& yield) override {
		last_activity = clock::now();
		return proxied_udp::on_read(len, yield);
	}
	void on_write(size_t len) override {
		is_stream = true;
		last_activity = clock::now();
		proxied_udp::on_write(len);
	}

	void remote_eof(bool graceful) override {
		if (!graceful) try {
			send_udp_port_unreachable(std::get<0>(endpoints), std::get<1>(endpoints));
		} catch (const system_error& e) {
			collect_ostream(std::cerr) << "ICMP " << try_cast_ipv4(std::get<1>(endpoints)) << " <= "
			                           << try_cast_ipv4(std::get<0>(endpoints))
			                           << " : cannot send UDP port unreachable message (" << e.what() << ')'
			                           << std::endl;
		}
		proxied_udp::remote_eof(graceful);
		//make the socket linger for 1 sec to avoid reopening another UDP socket immediately if we keep receiving data
		timeout.expires_after(std::chrono::seconds(1));
		timeout.async_wait([this_ = shptr()](error_code){});
	}

	void wait_timeout() {
		timeout.async_wait([this_weak = std::weak_ptr(shptr<proxied_udp_client>())](const error_code& ec) {
			if (ec) return;
			auto this_ = this_weak.lock();
			if (!this_) return;
			auto from_last_activity = clock::now() - this_->last_activity;
			auto interval = this_->is_stream ? udp_timeout_stream : udp_timeout;
			if (from_last_activity > std::chrono::seconds(interval)) {
				if (verbose) collect_ostream(std::cerr) << this_->description() << " : timed out" << std::endl;
				this_->kill();
				return;
			}
			this_->timeout.expires_at(this_->last_activity + std::chrono::seconds(interval));
			this_->wait_timeout();
		});
	}

	virtual ~proxied_udp_client() {
		tuples_all.erase(endpoints);
	}

};

}

void udp_front_loop(yield_context yield) {
	ip::udp::socket udp_front(asio, ip::udp::v6());
	udp_front.set_option(socket_base::reuse_address(true));
	if (!setsockopt(udp_front, SOL_SOCKET, SO_MARK, 3))
		throw system_error(errno, generic_category(), "cannot set fwmark=3 on UDP front socket");
	if (!setsockopt(udp_front, SOL_IPV6, IPV6_TRANSPARENT))
		throw system_error(errno, generic_category(), "cannot set IPV6_TRANSPARENT on UDP front socket");
	if (!setsockopt(udp_front, SOL_IPV6, IPV6_RECVORIGDSTADDR))
		throw system_error(errno, generic_category(), "cannot set IPV6_RECVORIGDSTADDR on UDP front socket");
	if (!setsockopt(udp_front, SOL_IP, IP_RECVORIGDSTADDR))
		throw system_error(errno, generic_category(), "cannot set IP_RECVORIGDSTADDR on UDP front socket");
	udp_front.bind(ip::udp::endpoint(ip::udp::v6(), port));
	/*
	 * UDP accept loop. Wait for read readiness, then use recvmsg() to get the ancillary IPV6_ORIGDSTADDR
	 * address, bind and connect a new socket. There is an unavoidable race condition between socket
	 * creation and multiple datagram being sent on a new connection, so this loop needs to lookup sockets
	 * by endpoints tuple, and possibly send datagrams over an already existing proxied socket.
	 */
	while (true) try {
		iovec iovec_buff;
		static char ancillary[1024];
		sockaddr_in6 from, * to_v6 = 0;
		sockaddr_in* to_v4 = 0;
		msghdr msgh;
		memset(&msgh, 0, sizeof(msgh));
		msgh.msg_name = &from;
		msgh.msg_namelen = sizeof(from);
		msgh.msg_iov = &iovec_buff;
		msgh.msg_iovlen = 1;
		msgh.msg_control = ancillary;
		msgh.msg_controllen = sizeof(ancillary);
		memset(&iovec_buff, 0, sizeof(iovec_buff));

		udp_front.async_wait(ip::udp::socket::wait_read, yield);
		int datalen = std::min(udp_front.available(), header::MAX_LEN);
		std::shared_ptr<char[]> packet(new char[datalen]);
		iovec_buff.iov_base = packet.get();
		iovec_buff.iov_len = datalen;
		if ((datalen = TEMP_FAILURE_RETRY(recvmsg(udp_front.native_handle(), &msgh, 0))) == -1)
			throw system_error(errno, system_category(), "cannot recvmsg() for UDP");
		if (msgh.msg_flags & MSG_TRUNC || msgh.msg_flags & MSG_CTRUNC)
			throw std::logic_error("not enough buffer space for UDP recvmsg");
		for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL; cmsg = CMSG_NXTHDR(&msgh, cmsg))
			if (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_ORIGDSTADDR)
				to_v6 = (sockaddr_in6*)CMSG_DATA(cmsg);
			else if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_ORIGDSTADDR)
				to_v4 = (sockaddr_in*)CMSG_DATA(cmsg);
		if (!to_v6 && !to_v4) throw std::logic_error("cannot obtain origin address in new UDP packet");

		using ipv6 = ip::address_v6;
		ip::udp::endpoint remote(ipv6(reinterpret_cast<ipv6::bytes_type&>(from.sin6_addr)), ntohs(from.sin6_port));
		ip::udp::endpoint local;
		if (to_v6) local = { ipv6(reinterpret_cast<ipv6::bytes_type&>(to_v6->sin6_addr)), ntohs(to_v6->sin6_port) };
		else {
			using ipv4 = ip::address_v4;
			local = { ip::make_address_v6(ip::v4_mapped, ipv4(ntohl(to_v4->sin_addr.s_addr))), ntohs(to_v4->sin_port) };
		}
		std::shared_ptr<proxied_udp_client> proxied;
		proxied = proxied_udp_client::find(local, remote);
		if (!proxied) try {
			ip::udp::socket udpsock(asio, ip::udp::v6());
			udpsock.set_option(socket_base::reuse_address(true));
			if (!setsockopt(udpsock, SOL_SOCKET, SO_MARK, 3))
				throw system_error(errno, generic_category(), "cannot set fwmark=3");
			if (!setsockopt(udpsock, SOL_IPV6, IPV6_TRANSPARENT))
				throw system_error(errno, generic_category(), "cannot set option IPV6_TRANSPARENT");
			udpsock.bind(local);
			udpsock.connect(remote);
			proxied = std::make_shared<proxied_udp_client>(std::move(udpsock));
			proxied->remember();
			proxied->spawn_lifecycle({});
		} catch (const system_error& e) {
			collect_ostream(std::cerr) << "UDP " << try_cast_ipv4(remote) << " => "
			                           << try_cast_ipv4(local) << " : cannot initialize connection (" << e.what() << ')'
			                           << std::endl;
			continue;
		}
		send_output(UDP_DATA, proxied->id, datalen, packet.get());
	} catch (const system_error& e) {
		//XXX not sure about possible error codes here...
		using namespace boost::asio::error;
		switch (e.code().value()) {
			case operation_aborted: return;
			case network_down:
			case host_unreachable:
			case network_unreachable:
				collect_ostream(std::cerr) << "UDP : cannot receive packet (" << e.what() << ')' << std::endl;
				break;
			default: throw;
		}
	}
}

}
