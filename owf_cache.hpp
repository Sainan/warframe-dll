#pragma once

#include <cstdint>

struct TocHeader
{
	uint32_t magic;
	uint32_t version;
};

struct TocEntry
{
	uint64_t cacheOffset;
	uint64_t timestamp;
	uint32_t compressedLen;
	uint32_t length;
	uint32_t reserved;
	uint32_t parentDirIndex;
	char name[64];
};

struct TocFile
{
	TocHeader header;
	TocEntry entries[1];
};
