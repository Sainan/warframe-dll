#include "owf_archive.hpp"

#include <cat.hpp>
#include <catTreeReader.hpp>
#include <deflate.hpp>
#include <joaat.hpp>
#include <MemoryRefReader.hpp>
#include <ObfusString.hpp>
#include <string.hpp>

#include "owf_archive_data.inc"

using namespace soup;

void owfArchive::load(const char* data, size_t size)
{
	MemoryRefReader r(data, size);
	uint64_t decompressed_size = 0;
	r.u64_dyn_b(decompressed_size);
	r.u64_dyn_b(this->creation);
	const auto off = r.getPosition();
	this->data = deflate::decompress(data + off, size - off, decompressed_size).decompressed;
}

void owfArchive::loadBuiltin()
{
	return load(compressed_archive_data, sizeof(compressed_archive_data));
}

bool owfArchive::loadHotfix(const char* data, size_t size, uint32_t version_hash)
{
	MemoryRefReader r(data, size);
	uint32_t target_version_hash;
	if (r.u32_le(target_version_hash) && target_version_hash == version_hash)
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
		r.u32_le(e_key);
		r.u32_le(out_len);
		if (e_key == key)
		{
			return this->data.data() + r.getPosition();
		}
		r.skip(out_len);
	}
	return nullptr;
}

uint64_t owfArchive::getVersionedInt(uint32_t path, uint64_t version) const
{
	uint32_t size;
	if (auto data = this->find(path, size))
	{
		MemoryRefReader r(data, size);
		uint64_t ver, val;
		while (r.u64_dyn_b(ver) && r.u64_dyn_b(val))
		{
			if (version >= ver)
			{
				return val;
			}
		}
	}
	return 0;
}

std::unordered_map<std::string, std::string> owfArchive::getCoreDict(const std::string& lang) const
{
	MemoryRefReader r(nullptr, 0);
	r.data = (const uint8_t*)this->find(soup::joaat::concat(soup::joaat::concat(soup::joaat::compileTimeHash("OpenWF/translations/core/"), lang), ObfusString(".cat.txt").str()), *(uint32_t*)&r.size);
	if (!r.data)
	{
		r.data = (const uint8_t*)this->find(soup::joaat::compileTimeHash("OpenWF/translations/core/en.cat.txt"), *(uint32_t*)&r.size);
	}

	if (auto root = soup::cat::parse(r))
	{
		soup::catTreeReader tr;
		return tr.toMap(root.get(), false);
	}
	return {};
}

std::unordered_map<std::string, std::string> owfArchive::getWebuiDict(const std::string& lang) const
{
	std::string buf = string::fromFile(ObfusString("OpenWF/webui-dict.cat.txt").str());
	MemoryRefReader r(buf.data(), buf.size());
	if (buf.empty())
	{
		r.data = (const uint8_t*)this->find(soup::joaat::concat(soup::joaat::concat(soup::joaat::compileTimeHash("OpenWF/translations/webui/"), lang), ObfusString(".cat.txt").str()), *(uint32_t*)&r.size);
		if (!r.data)
		{
			r.data = (const uint8_t*)this->find(soup::joaat::compileTimeHash("OpenWF/translations/webui/en.cat.txt"), *(uint32_t*)&r.size);
		}
	}

	if (auto root = soup::cat::parse(r))
	{
		soup::catTreeReader tr;
		return tr.toMap(root.get(), false);
	}
	return {};
}
