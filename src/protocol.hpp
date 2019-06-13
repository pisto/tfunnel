#pragma once

#include <cstdint>
#include <array>

namespace tfunnel {

enum opcodes {

	HELLO_FROM_CLIENT = 0,
	HELLO_FROM_PROXY,

	TCP_NEW,
	TCP_DATA,
	TCP_EOF,
	TCP_CLOSE,

	UDP_NEW,
	UDP_DATA,
	UDP_CLOSE,

	DIE,

	OPS_END,
};

struct header {
	static constexpr const uint64_t HELLO_MAGIC = 0xDEADBEEF, PROTOCOL_VERSION = 0;
	static constexpr const uint8_t OPCODE_BITS = 4, ID_BITS = 44, LEN_BITS = 16;
	static constexpr const size_t MAX_LEN = (1 << LEN_BITS) - 1;

	uint64_t opcode : OPCODE_BITS, id : ID_BITS, len : LEN_BITS;

	header() = default;
	constexpr explicit header(opcodes opcode, uint64_t id, uint16_t len): opcode(opcode), id(id), len(len) {}

	operator uint64_t() const { return *reinterpret_cast<const uint64_t*>(this); }

	static constexpr header handshake(bool client) {
		return header(client ? HELLO_FROM_CLIENT : HELLO_FROM_PROXY, HELLO_MAGIC ^ PROTOCOL_VERSION, 0);
	}
};

struct new_connection_data {
	std::array<uint8_t, 16> ipv6;
	uint16_t port;
};
static_assert(sizeof(new_connection_data) == 18);

}
