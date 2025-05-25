#include "owf_hotkeys.hpp"

#include <iostream>

#include <json.hpp>
#include <Key.hpp>
#include <ObfusString.hpp>

using namespace soup;

void load_hotkeys()
{
	std::vector<owfHotkey> hks;
	try
	{
		auto jr = json::decodeFile(ObfusString("OpenWF/Hotkeys.json").str());
		for (const auto& jc : jr->asArr().children)
		{
			auto& jHk = jc->asObj();
			auto& hk = hks.emplace_back();
			auto& jKey = jHk.at(ObfusString("key"));
			if (jKey.isStr())
			{
				hk.vk = 0;
				if (jKey.reinterpretAsStr().value.size() == 1)
				{
					hk.vk = soup::char_to_virtual_key(jKey.reinterpretAsStr().value[0]);
				}
				if (!hk.vk)
				{
					std::string msg = ObfusString("Invalid key: ").str();
					msg.append(jKey.asStr());
					soup::throwAssertionFailed(msg.c_str());
				}
			}
			else
			{
				hk.vk = jKey.asInt();
			}
			hk.has_ctrl = jHk.contains(ObfusString("ctrl"));
			hk.ctrl = hk.has_ctrl && jHk.at(ObfusString("ctrl")).asBool();
			hk.has_shift = jHk.contains(ObfusString("shift"));
			hk.shift = hk.has_shift && jHk.at(ObfusString("shift")).asBool();
			hk.has_alt = jHk.contains(ObfusString("alt"));
			hk.alt = hk.has_alt && jHk.at(ObfusString("alt")).asBool();
			hk.script = jHk.at(ObfusString("script")).asStr();
		}
	}
	catch (std::exception& e)
	{
		std::cout << ObfusString("Failed to load Hotkeys.json: ").str() << e.what() << std::endl;
	}
	hotkeys_mtx.lock();
	hotkeys = std::move(hks);
	hotkeys_mtx.unlock();
}
