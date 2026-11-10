#pragma once

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

namespace DSUReceiveDeadline
{
inline constexpr unsigned Milliseconds = 100;

inline void Configure(boost::asio::ip::udp::socket& socket)
{
#ifdef _WIN32
	// A shutdown wake-up datagram can be lost or blocked. Keep the blocking
	// receive bounded without adding a delay to packets that arrive normally.
	const DWORD timeout = Milliseconds;
	if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
		reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
	{
		throw boost::system::system_error(::WSAGetLastError(), boost::asio::error::get_system_category());
	}
#else
	(void)socket;
#endif
}

inline bool IsIdleTimeout(const boost::system::error_code& error)
{
#ifdef _WIN32
	return error == boost::asio::error::timed_out;
#else
	return false;
#endif
}
}
