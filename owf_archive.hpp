#pragma once

#include <cstdint>
#include <string>

class owfArchive
{
public:
	void load(const char* data, size_t size);
	void loadBuiltin();
	bool loadHotfix(const char* data, size_t size, uint32_t version_hash);
	const char* find(uint32_t key, uint32_t& out_len) const;
private:
	std::string data;
};
inline owfArchive g_archive;
