#include "owf_repo.hpp"

#include <cat.hpp>
#include <catTreeReader.hpp>
#include <deflate.hpp>
#include <joaat.hpp>
#include <MemoryRefReader.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <string.hpp>

#include "main.hpp" // BOOTSTRAPPER_TITLE

using namespace soup;

void owfRepo::loadArchive(const char* data, size_t size)
{
	uint64_t decompressed_size = 0;
	{
		MemoryRefReader r(data, size);
		r.u64_dyn_bp(this->timestamp);
		r.u64_dyn_bp(decompressed_size);
		const auto off = r.getPosition();
		data += off;
		size -= off;
	}
	std::string tar = deflate::decompress(data, size, decompressed_size).decompressed;
	MemoryRefReader r(tar);
	while (r.hasMore())
	{
		uint32_t key; r.u32_le(key);
		uint64_t len; r.u64_dyn_bp(len);
		std::string val = tar.substr(r.getPosition(), len);
		if (auto e = this->data.find(key); e != this->data.end())
		{
			e->second = std::move(val);
		}
		else
		{
			this->data.emplace(key, std::move(val));
		}
		r.skip(len);
	}
}

bool owfRepo::readHotfixHeader(const char* data, size_t size, uint64_t& timestamp)
{
	MemoryRefReader r(data, size);
	uint32_t target_version_hash;
	if (r.u32_le(target_version_hash) && target_version_hash == soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE))
	{
		r.skip(1);
		r.u64_dyn_bp(timestamp);
		return true;
	}
	return false;
}

bool owfRepo::loadHotfix(const char* data, size_t size)
{
	MemoryRefReader r(data, size);
	uint32_t target_version_hash;
	if (r.u32_le(target_version_hash) && target_version_hash == soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE))
	{
		r.u8(hotfix);
		const auto off = r.getPosition();
		this->loadArchive(data + off, size - off);
		return true;
	}
	return false;
}

const char* owfRepo::find(uint32_t key, size_t& out_len) const
{
	if (auto e = this->data.find(key); e != this->data.end())
	{
		out_len = e->second.size();
		return e->second.data();
	}
	return nullptr;
}

uint64_t owfRepo::getVersionedU64(uint32_t path, uint64_t version) const
{
	size_t size;
	if (auto data = this->find(path, size))
	{
		MemoryRefReader r(data, size);
		uint64_t ver, val;
		while (r.u64_dyn_bp(ver) && r.u64_dyn_bp(val))
		{
			if (version >= ver)
			{
				return val;
			}
		}
	}
	return 0;
}

int64_t owfRepo::getVersionedI64(uint32_t path, uint64_t version) const
{
	size_t size;
	if (auto data = this->find(path, size))
	{
		MemoryRefReader r(data, size);
		uint64_t ver;
		int64_t val;
		while (r.u64_dyn_bp(ver) && r.i64_dyn_bp(val))
		{
			if (version >= ver)
			{
				return val;
			}
		}
	}
	return 0;
}

Pattern owfRepo::getVersionedPattern(uint32_t path, uint64_t version) const
{
	size_t size;
	if (auto data = this->find(path, size))
	{
		MemoryRefReader r(data, size);
		uint64_t ver;
		Pattern val;
		while (r.u64_dyn_bp(ver) && val.io(r))
		{
			if (version >= ver)
			{
				return val;
			}
		}
	}
	return Pattern();
}

std::unordered_map<std::string, std::string> owfRepo::getCoreDict(const std::string& lang) const
{
	MemoryRefReader r(nullptr, 0);
	r.data = (const uint8_t*)this->find(soup::joaat::concat(soup::joaat::concat(soup::joaat::compileTimeHash("OpenWF/translations/core/"), lang), ObfusString(".cat.txt").str()), r.size);
	if (!r.data)
	{
		r.data = (const uint8_t*)this->find(soup::joaat::compileTimeHash("OpenWF/translations/core/en.cat.txt"), r.size);
	}

	if (auto root = soup::cat::parse(r))
	{
		soup::catTreeReader tr;
		return tr.toMap(root.get(), false);
	}
	return {};
}

std::unordered_map<std::string, std::string> owfRepo::getWebuiDict(const std::string& lang) const
{
	std::string buf = string::fromFile(ObfusString("OpenWF/webui-dict.cat.txt").str());
	MemoryRefReader r(buf.data(), buf.size());
	if (buf.empty())
	{
		r.data = (const uint8_t*)this->find(soup::joaat::concat(soup::joaat::concat(soup::joaat::compileTimeHash("OpenWF/translations/webui/"), lang), ObfusString(".cat.txt").str()), r.size);
		if (!r.data)
		{
			r.data = (const uint8_t*)this->find(soup::joaat::compileTimeHash("OpenWF/translations/webui/en.cat.txt"), r.size);
		}
	}

	if (auto root = soup::cat::parse(r))
	{
		soup::catTreeReader tr;
		return tr.toMap(root.get(), false);
	}
	return {};
}

const char* /*[16]*/ owfRepo::getExpectedCodeVersionForManifestHash(const char manifest_hash[22]) const
{
	size_t size;
	if (const char* data = this->find(soup::joaat::compileTimeHash("OpenWF/hash_to_code_version.json"), size))
	{
		while (size != 0)
		{
			if (memcmp(data, manifest_hash, 22) == 0)
			{
				data += 22; size -= 22;
				return data;
			}
			data += 22; size -= 22;
			data += 16; size -= 16;
		}
	}
	return nullptr;
}

std::string get_core_string(std::string key)
{
	if (auto e = g_core_dict.find(key); e != g_core_dict.end())
	{
		return e->second;
	}
#if PRIVATE
	if (g_core_dict.empty())
	{
		key.append(" (could not be resolved because core dict is not initialised yet)");
	}
#endif
	return key;
}
