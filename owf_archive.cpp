#include "owf_archive.hpp"

#include <deflate.hpp>
#include <MemoryRefReader.hpp>

#include "owf_archive_data.hpp"

using namespace soup;

void owfArchive::load(const char* data, size_t size)
{
	this->data = deflate::decompress(data, size).decompressed;
}

void owfArchive::loadBuiltin()
{
	return load(compressed_archive_data, sizeof(compressed_archive_data));
}

bool owfArchive::loadHotfix(const char* data, size_t size, uint32_t version_hash)
{
	MemoryRefReader r(data, size);
	uint32_t target_version_hash;
	if (r.u32le(target_version_hash) && target_version_hash == version_hash)
	{
		const auto off = r.getPosition();
		this->load(data + off, size - off);
		return true;
	}
	return false;
}

const char* owfArchive::find(uint32_t key, uint32_t& out_len) const
{
	MemoryRefReader r(this->data);
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
