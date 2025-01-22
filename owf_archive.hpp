#pragma once

#include <cstdint>

struct owfArchive
{
	static const char* find(uint32_t key, uint32_t& out_len);
};
