#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <fwd.hpp>
#include <Mutex.hpp>

class owfRepo
{
public:
	void loadArchive(const char* data, size_t size);
	void loadBuiltinArchive();
	static bool readHotfixHeader(const char* data, size_t size, uint32_t version_hash, uint64_t& timestamp);
	void loadHotfixNoVerify(const char* data, size_t size);
	bool loadHotfix(const char* data, size_t size, uint32_t version_hash);

	const char* find(uint32_t key, size_t& out_len) const;
	uint64_t getVersionedInt(uint32_t path, uint64_t ver) const;
	soup::Pattern getVersionedPattern(uint32_t path, uint64_t ver) const;
	std::unordered_map<std::string, std::string> getCoreDict(const std::string& lang) const;
	std::unordered_map<std::string, std::string> getWebuiDict(const std::string& lang) const;

	uint64_t timestamp;
	uint64_t hotfix;
protected:
	std::unordered_map<uint32_t, std::string> data;
};

inline soup::Mutex g_repo_mtx;
inline owfRepo g_repo;
inline std::unordered_map<std::string, std::string> g_core_dict;
