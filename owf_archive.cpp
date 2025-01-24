#include "owf_archive.hpp"

#include <deflate.hpp>
#include <MemoryRefReader.hpp>

#include "owf_archive_data.hpp"

void owfArchive::load(const char* data, size_t size)
{
	this->data = soup::deflate::decompress(data, size).decompressed;
}

void owfArchive::loadBuiltin()
{
	return load(compressed_archive_data, sizeof(compressed_archive_data));
}

const char* owfArchive::find(uint32_t key, uint32_t& out_len) const
{
	soup::MemoryRefReader r(this->data);
	while (r.hasMore())
	{
		uint32_t e_key;
		r.u32le(e_key);
		r.u32le(out_len);
		if (e_key == key)
		{
			return this->data.data() + r.getPosition();
		}
		r.skip(out_len);
	}
	return nullptr;
}
