#include "owf_nrsauth.hpp"

#include <mutex>

#include <aes.hpp>
#include <joaat.hpp>
#include <ObfusString.hpp>
#include <rand.hpp>
#include <sha1.hpp>
#include <StringRefWriter.hpp>
#include <time.hpp>

#include "main.hpp"
#include "owf_tunables.hpp"

using namespace soup;

/*static*/ time_t owfNrsAuth::getNextTransmissionTime()
{
	std::lock_guard lock(g_client_tunables_mtx);
	return last_transmission + g_client_tunables.getInt(joaat::compileTimeHash("nrsauth_interval"), 1);
}

/*static*/ void owfNrsAuth::transmit()
{
	last_transmission = time::unixSeconds();

	std::string ticket_info;
	{
		StringRefWriter sw(ticket_info);
		sw.i64_le(last_transmission);
		sw.str_nt(accountId);
		sw.str_nt(g_bootstrapper_title);
	}

	std::string ticket_mac;
	{
		ObfusString mackey("ElqI0K4YC0lPpfDE");
		sha1::HmacState hmac(mackey.data(), mackey.size());
		hmac.append(ticket_info.data(), ticket_info.size());
		hmac.finalise();
		ticket_mac = hmac.getDigest();
	}

	std::string ticket = ticket_info + ticket_mac;

	// Create random IV
	uint8_t iv[16];
	soup::rand.fill(iv);

	// Encrypt ticket
	{
		aes::pkcs7Pad(ticket);
		std::lock_guard lock(g_client_tunables_mtx);
		const auto aeskey = g_client_tunables.strings.at(joaat::compileTimeHash("nrsauth_aeskey"));
		aes::cbcEncrypt((uint8_t*)ticket.data(), ticket.size(), (const uint8_t*)aeskey.data(), aeskey.size(), iv);
	}

	IpAddr addr;
	if (addr.fromString(owfNrsAuth::address))
	{
		uint16_t port;
		{
			std::lock_guard lock(g_client_tunables_mtx);
			port = g_client_tunables.getInt(joaat::compileTimeHash("nrsauth_port"));
		}
		if (port)
		{
			Socket s;
			s.udpClientSend(addr, port, std::string((const char*)iv, sizeof(iv)) + ticket);
		}
	}
}
