#pragma once

#include <boost/asio.hpp>

inline boost::system::error_code& ignore_ec() {
	static boost::system::error_code ignored;
	return ignored;
}

inline boost::asio::ip::address try_cast_ipv4(boost::asio::ip::address addr) try {
	return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, addr.to_v6());
} catch (const boost::asio::ip::bad_address_cast&) {
	return addr;
}

template<typename Proto> auto try_cast_ipv4(boost::asio::ip::basic_endpoint<Proto> ep) {
	return boost::asio::ip::basic_endpoint<Proto>(try_cast_ipv4(ep.address()), ep.port());
}

#ifndef PROXY_ONLY

#include <netinet/in.h>

template<typename socket> bool setsockopt(socket& s, int level, int opt, int value = 1) {
	int fd = s.native_handle();
	return !setsockopt(fd, level, opt, &value, sizeof(value));
};

#endif

/*
 * Collect writes to an std::ostream and flush them all together. Useful when sending output
 * to stdout/stderr in parallel program where writes should be "atomic".
 */

#include <sstream>
#include <iostream>
#include <utility>

struct collect_ostream: std::ostringstream {
	std::ostream& out;

	collect_ostream(std::ostream& out): out(out) {}
	~collect_ostream() { out << str(); }

	template<typename T> std::ostream& operator<<(T&& t) const {
		return static_cast<std::ostringstream&>(const_cast<collect_ostream&>(*this)) << std::forward<T>(t);
	}

};
