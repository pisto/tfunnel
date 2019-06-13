#pragma once

#include <boost/asio.hpp>

inline boost::system::error_code& ignore_ec() {
	static boost::system::error_code ignored;
	return ignored;
}

#ifndef PROXY_ONLY

#include <netinet/in.h>

template<typename socket> bool setsockopt(socket& s, int level, int opt) {
	int fd = s.native_handle(), yes = 1;
	return !setsockopt(fd, level, opt, &yes, sizeof(yes));
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
