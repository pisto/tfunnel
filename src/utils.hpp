#pragma once

#include <boost/asio.hpp>

inline boost::system::error_code& ignore_ec() {
	static boost::system::error_code ignored;
	return ignored;
}

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
