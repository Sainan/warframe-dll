#pragma once

#include "owf_web.hpp"

struct owfUdpProxy
{
	inline static soup::SharedPtr<soup::Worker> downstream;
	inline static soup::SocketAddr downstream_addr;
	inline static soup::SharedPtr<soup::Socket> upstream;
	inline static soup::SocketAddr upstream_addr;

	static bool bind()
	{
		return g_serv.bindUdp(6951, [](soup::Socket& s, soup::SocketAddr&& addr, std::string&& data) SOUP_EXCAL
		{
			downstream = g_serv.getShared(s);
			downstream_addr = addr;

			const bool init = !upstream;
			if (init)
			{
				upstream = g_serv.addSocket();
			}

#if LOGGING
			conout << "owfUdpProxy: " << downstream_addr.toString() << " -> " << upstream_addr.toString() << ": " << soup::string::bin2hex(data) << std::endl;
#endif
			if (upstream->udpClientSend(upstream_addr, data))
			{
				upstreamRecv();
			}
		});
	}

	static void upstreamRecv()
	{
		upstream->udpRecv([](soup::Socket&, soup::SocketAddr&& addr, std::string&& data, soup::Capture&&)
		{
			if (upstream && upstream_addr == addr)
			{
#if LOGGING
				conout << "owfUdpProxy: " << upstream_addr.toString() << " -> " << downstream_addr.toString() << ": " << soup::string::bin2hex(data) << std::endl;
#endif
				static_cast<soup::Socket*>(downstream.get())->udpServerSend(downstream_addr, data);
				upstreamRecv();
			}
			else
			{
#if LOGGING
				conout << "owfUdpProxy: Discarding packet from " << addr.toString() << ": " << soup::string::bin2hex(data) << std::endl;
#endif
			}
		});
	}
};
