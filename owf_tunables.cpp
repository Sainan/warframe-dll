#include "owf_tunables.hpp"

#include <joaat.hpp>
#include <json.hpp>
#include <MemoryRefReader.hpp>

#include "owf_archive.hpp"

using namespace soup;

bool owfServerTunables::load(const char* data, size_t size)
{
	bools.clear();

	auto jr = json::decode(data, size);
	if (!jr || !jr->isObj())
	{
		return false;
	}
	for (const auto& e : jr->reinterpretAsObj().children)
	{
		if (e.first->isStr())
		{
			if (e.second->isBool())
			{
				if (e.second->reinterpretAsBool().value)
				{
					bools.emplace_back(joaat::hash(e.first->reinterpretAsStr().value));
				}
			}
			else if (e.second->isStr())
			{
				strings.emplace(joaat::hash(e.first->reinterpretAsStr().value), e.second->reinterpretAsStr().value);
			}
		}
	}
	return true;
}

std::string owfServerTunables::getProhibitionName(uint32_t hash)
{
	g_archive_mtx.lock();
	uint32_t size;
	auto data = g_archive.find(soup::joaat::compileTimeHash("OpenWF/prohibition_names.json"), size);
	g_archive_mtx.unlock();
	if (data)
	{
		if (auto jr = soup::json::decode(std::string(data, size)))
		{
			for (const auto& e : jr->asObj().children)
			{
				if (soup::joaat::hash(e.first->asStr()) == hash)
				{
					return e.second->asStr();
				}
			}
		}
	}
	return {};
}

bool owfClientTunables::load(const char* data, size_t size)
{
	ints.clear();
	strarrs.clear();

	auto jr = json::decode(data, size);
	if (!jr || !jr->isObj())
	{
		return false;
	}
	for (const auto& e : jr->reinterpretAsObj().children)
	{
		if (e.first->isStr())
		{
			if (e.second->isInt())
			{
				ints.emplace(joaat::hash(e.first->reinterpretAsStr().value), e.second->reinterpretAsInt().value);
			}
			else if (e.second->isArr())
			{
				std::vector<uint32_t> arr;
				for (const auto& c : e.second->reinterpretAsArr())
				{
					if (c.isStr())
					{
						arr.emplace_back(joaat::hash(c.reinterpretAsStr().value));
					}
				}
				strarrs.emplace(joaat::hash(e.first->reinterpretAsStr().value), std::move(arr));
			}
		}
	}
	return true;
}

bool owfClientTunables::loadMsgpack(const char* data, size_t size)
{
	ints.clear();
	strarrs.clear();

	MemoryRefReader r(data, size);
	auto jr = json::msgpackDecode(r);
	if (!jr || !jr->isObj())
	{
		return false;
	}
	for (const auto& e : jr->reinterpretAsObj().children)
	{
		if (e.first->isInt())
		{
			/*if (e.second->isBool())
			{
				if (e.second->reinterpretAsBool().value)
				{
					bools.emplace_back(e.first->reinterpretAsInt().value);
				}
			}
			else*/ if (e.second->isInt())
			{
				ints.emplace(e.first->reinterpretAsInt().value, e.second->reinterpretAsInt().value);
			}
			else if (e.second->isArr())
			{
				std::vector<uint32_t> arr;
				for (const auto& c : e.second->reinterpretAsArr())
				{
					if (c.isInt())
					{
						arr.emplace_back(c.reinterpretAsInt().value);
					}
				}
				strarrs.emplace(e.first->reinterpretAsInt().value, std::move(arr));
			}
		}
	}
	return true;
}

uint32_t owfClientTunables::getInt(uint32_t hash) const noexcept
{
	if (auto e = ints.find(hash); e != ints.end())
	{
		return e->second;
	}
	return 0;
}

bool owfClientTunables::isStringInArray(uint32_t hash, uint32_t str_hash) const noexcept
{
	if (auto e = strarrs.find(hash); e != strarrs.end())
	{
		return std::find(e->second.begin(), e->second.end(), str_hash) != e->second.end();
	}
	return false;
}
