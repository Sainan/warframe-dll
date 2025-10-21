#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <Mutex.hpp>

struct owfArchive
{
	void load(const char* data, size_t size);
	void loadBuiltin();
	bool loadHotfix(const char* data, size_t size, uint32_t version_hash);
	const char* find(uint32_t key, uint32_t& out_len) const;
	uint64_t getVersionedInt(uint32_t path, uint64_t ver) const;
	std::unordered_map<std::string, std::string> getCoreDict(const std::string& lang) const;
	std::unordered_map<std::string, std::string> getWebuiDict(const std::string& lang) const;

	uint64_t creation;
	std::string data;
};

inline soup::Mutex g_archive_mtx;
inline owfArchive g_archive;
inline std::unordered_map<std::string, std::string> g_core_dict;
