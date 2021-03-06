#include <cerrno>
#include <array>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/udp.h>
#include "env.hpp"

using namespace boost::asio;
using namespace boost::system;

namespace tfunnel {

namespace {

template<typename T> uint16_t in_checksum(const T& payload) {
	union {
		uint32_t total;
		struct { uint16_t as_word, carry; };
	} sum{ 0 };
	for (uint16_t word: reinterpret_cast<const std::array<uint16_t, sizeof(T) / 2>&>(payload)) sum.total += word;
	if (sizeof(T) % 2) sum.total += reinterpret_cast<const uint8_t*>(&payload)[sizeof(T) - 1];
	while (sum.carry) sum.total = sum.as_word + sum.carry;
	return ~sum.as_word;
}

void send_udp_port_unreachable_v4(const boost::asio::ip::udp::endpoint& local,
                                  const boost::asio::ip::udp::endpoint& remote) {
	boost::asio::ip::icmp::socket raw(asio, boost::asio::ip::icmp::v4());
	if (!setsockopt(raw, SOL_SOCKET, SO_MARK, 3))
		throw system_error(errno, generic_category(), "cannot set fwmark=3 on ICMP socket");
	if (!setsockopt(raw, SOL_IP, IP_TRANSPARENT))
		throw system_error(errno, generic_category(), "cannot set option IP_TRANSPARENT on raw socket");

	struct [[gnu::packed]] {
		icmphdr icmp;
		iphdr ip;
		udphdr udp;
	} packet;
	memset(&packet, 0, sizeof(packet));
	packet.icmp.type = ICMP_DEST_UNREACH;
	packet.icmp.code = ICMP_PORT_UNREACH;
	packet.ip.version = 4;
	packet.ip.ihl = sizeof(packet.ip) / 4;
	packet.ip.tot_len = htons(sizeof(packet.ip) + sizeof(packet.udp));
	packet.ip.ttl = 255;
	packet.ip.protocol = IPPROTO_UDP;
	packet.ip.saddr = htonl(remote.address().to_v4().to_uint());
	packet.ip.daddr = htonl(local.address().to_v4().to_uint());
	packet.udp.source = htons(remote.port());
	packet.udp.dest = htons(local.port());
	packet.udp.len = htons(sizeof(packet.udp));

	packet.ip.check = in_checksum(packet.ip);
	packet.icmp.checksum = in_checksum(packet);

	msghdr msgh;
	memset(&msgh, 0, sizeof(msgh));

	sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_addr.s_addr = packet.ip.saddr;
	msgh.msg_name = &to;
	msgh.msg_namelen = sizeof(to);

	iovec iovec_buff;
	memset(&iovec_buff, 0, sizeof(iovec_buff));
	iovec_buff.iov_base = &packet;
	iovec_buff.iov_len = sizeof(packet);
	msgh.msg_iov = &iovec_buff;
	msgh.msg_iovlen = 1;

	union {
		char buf[CMSG_SPACE(sizeof(in_pktinfo))];
		struct cmsghdr align;
	} cmsg_buff;
	memset(&cmsg_buff, 0, sizeof(cmsg_buff));
	msgh.msg_control = cmsg_buff.buf;
	msgh.msg_controllen = sizeof(cmsg_buff.buf);

	cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
	cmsg->cmsg_level = SOL_IP;
	cmsg->cmsg_type = IP_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
	auto from = (in_pktinfo*)CMSG_DATA(cmsg);
	from->ipi_spec_dst.s_addr = packet.ip.daddr;

	if (sendmsg(raw.native_handle(), &msgh, 0) == -1) {
		int errno_now = errno;
		collect_ostream(std::cerr) << "ICMP " << remote.address() << " <= " << local.address()
		                           << " : cannot send (" << error_code(errno_now, system_category()).message()
		                           << ')' << std::endl;
	}

}

void send_udp_port_unreachable_v6(const boost::asio::ip::udp::endpoint& local,
                                  const boost::asio::ip::udp::endpoint& remote) {
	boost::asio::ip::icmp::socket raw(asio, boost::asio::ip::icmp::v6());
	if (!setsockopt(raw, SOL_SOCKET, SO_MARK, 3))
		throw system_error(errno, generic_category(), "cannot set fwmark=3 on ICMP socket");
	if (!setsockopt(raw, SOL_IPV6, IPV6_TRANSPARENT))
		throw system_error(errno, generic_category(), "cannot set option IPV6_TRANSPARENT on raw socket");
	if (!setsockopt(raw, SOL_RAW, IPV6_CHECKSUM, offsetof(icmp6_hdr, icmp6_cksum)))
		throw system_error(errno, system_category(), "cannot set option IPV6_CHECKSUM on ICMP socket");

	struct [[gnu::packed]] {
		icmp6_hdr icmp;
		ip6_hdr ip;
		udphdr udp;
	} packet;
	memset(&packet, 0, sizeof(packet));
	packet.icmp.icmp6_type = ICMP6_DST_UNREACH;
	packet.icmp.icmp6_code = ICMP6_DST_UNREACH_NOPORT;
	packet.ip.ip6_flow = 0x60;
	packet.ip.ip6_plen = htons(sizeof(packet.udp));
	packet.ip.ip6_nxt = IPPROTO_UDP;
	packet.ip.ip6_hlim = 255;
	auto ip6_src = remote.address().to_v6().to_bytes(), ip6_dst = local.address().to_v6().to_bytes();
	memcpy(&packet.ip.ip6_src, &ip6_src, sizeof(packet.ip.ip6_src));
	memcpy(&packet.ip.ip6_dst, &ip6_dst, sizeof(packet.ip.ip6_dst));
	packet.udp.source = htons(remote.port());
	packet.udp.dest = htons(local.port());
	packet.udp.len = htons(sizeof(packet.udp));

	struct [[gnu::packed]] {
		boost::asio::ip::address_v6::bytes_type src, dst;
		uint32_t udplen;
		uint8_t zero[3], next_header;
		udphdr udp;
	} ipv6_pseudo;
	memset(&ipv6_pseudo, 0, sizeof(ipv6_pseudo));
	ipv6_pseudo.src = ip6_src;
	ipv6_pseudo.dst = ip6_dst;
	ipv6_pseudo.udplen = htonl(sizeof(packet.udp));
	ipv6_pseudo.next_header = IPPROTO_UDP;
	ipv6_pseudo.udp = packet.udp;
	packet.udp.check = in_checksum(ipv6_pseudo);

	msghdr msgh;
	memset(&msgh, 0, sizeof(msgh));

	sockaddr_in6 to;
	memset(&to, 0, sizeof(to));
	memcpy(&to.sin6_addr.s6_addr, &packet.ip.ip6_src, sizeof(to.sin6_addr.s6_addr));
	msgh.msg_name = &to;
	msgh.msg_namelen = sizeof(to);

	iovec iovec_buff;
	memset(&iovec_buff, 0, sizeof(iovec_buff));
	iovec_buff.iov_base = &packet;
	iovec_buff.iov_len = sizeof(packet);
	msgh.msg_iov = &iovec_buff;
	msgh.msg_iovlen = 1;

	union {
		char buf[CMSG_SPACE(sizeof(in6_pktinfo))];
		struct cmsghdr align;
	} cmsg_buff;
	memset(&cmsg_buff, 0, sizeof(cmsg_buff));
	msgh.msg_control = cmsg_buff.buf;
	msgh.msg_controllen = sizeof(cmsg_buff.buf);

	cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
	cmsg->cmsg_level = SOL_IPV6;
	cmsg->cmsg_type = IPV6_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
	auto from = (in6_pktinfo*)CMSG_DATA(cmsg);
	memcpy(&from->ipi6_addr.s6_addr, &packet.ip.ip6_dst, sizeof(from->ipi6_addr.s6_addr));

	if (sendmsg(raw.native_handle(), &msgh, 0) == -1) {
		int errno_now = errno;
		collect_ostream(std::cerr) << "ICMP " << remote.address() << " <= " << local.address()
		                           << " : cannot send (" << error_code(errno_now, system_category()).message()
		                           << ')' << std::endl;
	}
}

}

void send_udp_port_unreachable(boost::asio::ip::udp::endpoint local,
                               boost::asio::ip::udp::endpoint remote) {
	local = try_cast_ipv4(local), remote = try_cast_ipv4(remote);
	if (local.address().is_v4() && remote.address().is_v4()) send_udp_port_unreachable_v4(local, remote);
	else send_udp_port_unreachable_v6(local, remote);
}

}
