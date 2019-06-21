#pragma once

#include <cstdint>
#include <array>

namespace tfunnel {

enum opcodes {

	HELLO_FROM_CLIENT = 0,
	HELLO_FROM_PROXY,

	TCP_NEW,
	TCP_DATA,
	TCP_CHOKE,
	TCP_UNCHOKE,
	TCP_EOF,
	TCP_CLOSE,

	UDP_NEW,
	UDP_DATA,
	UDP_CLOSE,

	OPS_END,
};

struct header {
	static constexpr const uint16_t HELLO_MAGIC = 0xDBEF, PROTOCOL_VERSION = 2;
	static constexpr const uint8_t OPCODE_BITS = 4, ID_BITS = 14, LEN_BITS = 14;
	static constexpr const size_t MAX_LEN = (1 << LEN_BITS) - 1;

	uint32_t opcode : OPCODE_BITS, id : ID_BITS, len : LEN_BITS;

	header() = default;
	constexpr explicit header(opcodes opcode, uint16_t id, uint16_t len): opcode(opcode), id(id), len(len) {}

	operator uint32_t() const { return *reinterpret_cast<const uint32_t*>(this); }

	static constexpr header handshake(bool client) {
		return header(client ? HELLO_FROM_CLIENT : HELLO_FROM_PROXY, HELLO_MAGIC ^ PROTOCOL_VERSION, 0);
	}
};

struct new_connection_data {
	std::array<uint8_t, 16> ipv6;
	uint16_t port;
};

static_assert(sizeof(header) == 4, "the compiler is messing up the header struct size");
static_assert(sizeof(new_connection_data) == 18, "the compiler is messing up the new_connection_data struct size");

}
