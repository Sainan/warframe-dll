#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mutex.hpp>

struct owfServerTunables
{
	std::vector<uint32_t> bools;
	std::unordered_map<uint32_t, std::string> strings;

	bool load(const char* data, size_t size, bool delta = false);

	bool getBool(uint32_t hash) const noexcept
	{
		return std::find(bools.begin(), bools.end(), hash) != bools.end();
	}
	//static bool isProhibition(uint32_t hash) { return !getProhibitionName(hash).empty(); }
	static std::string getProhibitionName(uint32_t hash);
};

struct owfClientTunables
{
	std::unordered_map<uint32_t, uint32_t> ints;
	std::unordered_map<uint32_t, std::vector<uint32_t>> strarrs;

	bool load(const char* data, size_t size);
	bool loadMsgpack(const char* data, size_t size);

	uint32_t getInt(uint32_t hash) const noexcept;
	bool isStringInArray(uint32_t hash, uint32_t str_hash) const noexcept;
};

inline soup::Mutex g_server_tunables_mtx;
inline owfServerTunables g_server_tunables;

inline soup::Mutex g_client_tunables_mtx;
inline owfClientTunables g_client_tunables;
