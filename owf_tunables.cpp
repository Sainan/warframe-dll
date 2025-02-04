#include "owf_tunables.hpp"

#include <mutex>

#include <joaat.hpp>
#include <json.hpp>

#include "owf_archive.hpp"

bool owfTunables::has(uint32_t hash)
{
	std::lock_guard lock(mtx);
	return hasLocked(hash);
}

bool owfTunables::hasLocked(uint32_t hash)
{
	return std::find(set.begin(), set.end(), hash) != set.end();
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
