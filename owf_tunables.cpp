#include "owf_tunables.hpp"

#include <joaat.hpp>
#include <json.hpp>

#include "owf_archive.hpp"

using namespace soup;

bool owfTunables::load(const char* data, size_t size)
{
	bools.clear();
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
			if (e.second->isBool())
			{
				if (e.second->reinterpretAsBool().value)
				{
					bools.emplace_back(joaat::hash(e.first->reinterpretAsStr().value));
				}
			}
			else if (e.second->isArr())
			{
				std::vector<uint32_t> arr;
				for (const auto& c : e.second->reinterpretAsArr())
				{
					if (c.isStr())
					{
						arr.emplace_back(joaat::hash(c.asStr().value));
					}
				}
				strarrs.emplace(joaat::hash(e.first->reinterpretAsStr().value), std::move(arr));
			}
		}
	}
	return true;
}

bool owfTunables::isStringInArray(uint32_t hash, uint32_t str_hash) const noexcept
{
	if (auto e = strarrs.find(hash); e != strarrs.end())
	{
		return std::find(e->second.begin(), e->second.end(), str_hash) != e->second.end();
	}
	return false;
}

std::string owfTunables::getProhibitionName(uint32_t hash)
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
