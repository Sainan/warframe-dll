#pragma once

#include <ctime>
#include <string>

struct owfNrsAuth
{
	inline static std::string accountId;
	inline static std::string address;
	inline static time_t last_transmission = 0;

	static bool shouldTransmit() noexcept
	{
		return last_transmission != 0;
	}

	static time_t getNextTransmissionTime();

	static void clear() noexcept
	{
		address.clear();
		last_transmission = 0;
	}

	static void transmit();
};
