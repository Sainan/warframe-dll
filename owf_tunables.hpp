#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mutex.hpp>

struct owfServerTunables
{
	std::vector<uint32_t> bools;

	bool load(const char* data, size_t size);

	bool getBool(uint32_t hash) const noexcept
	{
		return std::find(bools.begin(), bools.end(), hash) != bools.end();
	}
	//static bool isProhibition(uint32_t hash) { return !getProhibitionName(hash).empty(); }
	static std::string getProhibitionName(uint32_t hash);
};

struct owfClientTunables
{
	std::unordered_map<uint32_t, std::vector<uint32_t>> strarrs;

	bool load(const char* data, size_t size);
	bool loadMsgpack(const char* data, size_t size);

	bool isStringInArray(uint32_t hash, uint32_t str_hash) const noexcept;
};

struct owfOtaTunables
{
	int remote_ip_mode = 0; // 0 = Disallow, 1 = Blacklisting, 2 Whitelisting
	std::vector<uint32_t> remote_ip_list;

	bool load(const char* data, size_t size);
};

inline soup::Mutex g_server_tunables_mtx;
inline owfServerTunables g_server_tunables;

inline soup::Mutex g_client_tunables_mtx;
inline owfClientTunables g_client_tunables;

inline soup::Mutex g_ota_tunables_mtx;
inline owfOtaTunables g_ota_tunables;
