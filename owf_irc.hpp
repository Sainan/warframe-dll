#pragma once

#include <queue>

#include <netConnectTask.hpp>
#include <SharedPtr.hpp>
#include <Socket.hpp>
#include <time.hpp>

inline uint16_t g_irc_port;
inline std::string g_irc_upstream_host;
inline soup::SharedPtr<soup::Socket> g_irc_downstream;
inline soup::SharedPtr<soup::Socket> g_irc_upstream;
inline std::queue<std::string> g_irc_upstream_send_queue;

inline void irc_downstream_recv(soup::Socket& s)
{
	s.callback_recv_on_close = true;
	s.recv([](soup::Socket& s, std::string&& data, soup::Capture&&)
	{
#if LOGGING
		//conout << "irc_downstream_recv: " << data << std::endl;
#endif
		if (g_irc_upstream)
		{
			if (data.empty())
			{
				g_irc_upstream->close();
				g_irc_upstream.reset();
				return;
			}
			g_irc_upstream->send(std::move(data));
		}
		else
		{
			if (data.empty())
			{
				return;
			}
#if LOGGING
			//conout << "irc_downstream_recv - added to send queue" << std::endl;
#endif
			g_irc_upstream_send_queue.emplace(std::move(data));
		}
		irc_downstream_recv(s);
	});
}

inline void irc_upstream_recv(soup::Socket& s)
{
	s.callback_recv_on_close = true;
	s.recv([](soup::Socket& s, std::string&& data, soup::Capture&&)
	{
#if LOGGING
		//conout << "irc_upstream_recv: " << data << std::endl;
#endif
		if (g_irc_downstream)
		{
			if (data.empty())
			{
				g_irc_downstream->close();
				g_irc_downstream.reset();
				return;
			}
			g_irc_downstream->send(std::move(data));
		}
		else
		{
#if LOGGING
			conout << "irc_upstream_recv - no downstream ?!" << std::endl;
#endif
		}
		irc_upstream_recv(s);
	});
}

struct owfConnectToIrcTask : public soup::Task
{
	soup::netConnectTask connector;
	time_t started_tls_handshake_at;

	owfConnectToIrcTask()
		: connector(g_irc_upstream_host, 6697)
	{
#if LOGGING
		conout << "owfConnectToIrcTask: Connecting to " << g_irc_upstream_host << ":6697" << std::endl;
#endif
		g_irc_upstream.reset();
	}

	void onTick() final
	{
		if (!g_irc_upstream)
		{
			if (connector.tickUntilDone())
			{
				if (connector.wasSuccessful())
				{
					g_irc_upstream = connector.getSocket();
					g_irc_upstream->enableCryptoClient(g_irc_upstream_host, [](soup::Socket& s, soup::Capture&& cap, std::string&&) SOUP_EXCAL
					{
#if LOGGING
						conout << "owfConnectToIrcTask: Securely connected" << std::endl;
#endif
						while (!g_irc_upstream_send_queue.empty())
						{
#if LOGGING
							//conout << "owfConnectToIrcTask: send queued: " << g_irc_upstream_send_queue.front() << std::endl;
#endif
							s.send(g_irc_upstream_send_queue.front());
							g_irc_upstream_send_queue.pop();
						}
						irc_upstream_recv(s);
						cap.get<owfConnectToIrcTask*>()->setWorkDone();
					}, this, {}, &soup::Socket::certchain_validator_default, {}, true);
					started_tls_handshake_at = soup::time::unixSeconds();
				}
				else
				{
					setWorkDone();
				}
			}
		}
		else
		{
			if (soup::time::unixSecondsSince(started_tls_handshake_at) > 30)
			{
				g_irc_upstream->close();
				g_irc_upstream.reset();
				setWorkDone();
			}
		}
	}
};
