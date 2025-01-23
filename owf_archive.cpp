#include "owf_archive.hpp"

#include <deflate.hpp>
#include <MemoryRefReader.hpp>

#include "owf_archive_data.hpp"

static std::string archive_data;

void owfArchive::load(const char* data, size_t size)
{
	archive_data = soup::deflate::decompress(data, size).decompressed;
}

void owfArchive::loadBuiltin()
{
	return load(compressed_archive_data, sizeof(compressed_archive_data));
}

const char* owfArchive::find(uint32_t key, uint32_t& out_len)
{
	soup::MemoryRefReader r(archive_data);
	while (r.hasMore())
	{
		uint32_t e_key;
		r.u32le(e_key);
		r.u32le(out_len);
		if (e_key == key)
		{
			return archive_data.data() + r.getPosition();
		}
		r.skip(out_len);
	}
	return nullptr;
}
