#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Mutex.hpp>

struct owfTunables
{
	inline static soup::Mutex mtx;
	inline static std::vector<uint32_t> set;

	static bool has(uint32_t hash);
	static bool hasLocked(uint32_t hash);
	//static bool isProhibition(uint32_t hash) { return !getProhibitionName(hash).empty(); }
	static std::string getProhibitionName(uint32_t hash);
};
