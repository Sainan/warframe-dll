#pragma once

#include <cstdint>
#include <string>

#include <Mutex.hpp>

struct owfArchive
{
	void load(const char* data, size_t size);
	void loadBuiltin();
	bool loadHotfix(const char* data, size_t size, uint32_t version_hash);
	const char* find(uint32_t key, uint32_t& out_len) const;

	std::string data;
};

inline soup::Mutex g_archive_mtx;
inline owfArchive g_archive;
