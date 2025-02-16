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

struct TocFileMapping
{
	TocFile* toc;
	size_t toc_size;

	TocFileMapping(const std::filesystem::path& path)
		: toc((TocFile*)soup::filesystem::createFileMapping(path, toc_size))
	{
		SOUP_ASSERT(toc != nullptr);
	}

	~TocFileMapping()
	{
		soup::filesystem::destroyFileMapping(toc, toc_size);
	}

	[[nodiscard]] uint32_t findIndex(const char* name, size_t len, uint32_t parent) const noexcept
	{
		const uint32_t num_entries = (toc_size - sizeof(TocHeader)) / sizeof(TocEntry);
		for (uint32_t i = 0; i != num_entries; ++i)
		{
			if (toc->entries[i].parentDirIndex == parent
				&& memcmp(toc->entries[i].name, name, len) == 0 && toc->entries[i].name[len] == '\0'
				&& toc->entries[i].timestamp != 0 // Ignore deleted files
				)
			{
				return 1 + i;
			}
		}
		return 0;
	}

	[[nodiscard]] uint32_t findIndex(const char* path, size_t len) const noexcept
	{
		++path; // Skip '/'
		--len;

		uint32_t parent = 0;
		for (const char* sep; (sep = strchr(path, '/')) != nullptr; path = sep + 1)
		{
			parent = findIndex(path, sep - path, parent);
			len -= (sep - path) + 1;
		}
		return findIndex(path, len, parent);
	}

	[[nodiscard]] TocEntry* findEntry(const char* path, size_t len) const noexcept
	{
		if (auto i = findIndex(path, len))
		{
			return &toc->entries[i - 1];
		}
		return nullptr;
	}
};
