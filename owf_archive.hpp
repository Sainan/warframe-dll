#pragma once

#include <cstdint>

struct owfArchive
{
	static void load(const char* data, size_t size);
	static void loadBuiltin();
	static const char* find(uint32_t key, uint32_t& out_len);
};
