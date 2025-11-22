#include "owf_hotkeys.hpp"

#include <iostream>

#include <joaat.hpp>
#include <json.hpp>
#include <Key.hpp>
#include <ObfusString.hpp>

#include <lauxlib.h>

#include "owf_repo.hpp"
#include "owf_scripting.hpp"

using namespace soup;

void load_hotkeys()
{
	// Due to the way load_hotkeys is called, g_repo_mtx does not need to be locked.
	// Furthermore, g_repo_mtx cannot be locked during lua_pcall as it would make owf_repo_find fail because g_repo_mtx is not recursive.
	auto L = luaL_newstate();
	owfScript::openLibs(L);
	size_t size;
	auto data = g_repo.find(soup::joaat::compileTimeHash("OpenWF/helpers/pre_load_hotkeys.pluto"), size);
	if (luaL_loadbuffer(L, data, size, nullptr) != LUA_OK
		|| lua_pcall(L, 0, 0, 0) != LUA_OK
		)
	{
		owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error").str());
	}
	lua_close(L);

	std::vector<owfHotkey> hks;
	try
	{
		auto jr = json::decodeFile(ObfusString("OpenWF/Hotkeys.json").str());
		SOUP_ASSERT(jr);
		for (const auto& jc : jr->asArr().children)
		{
			auto& jHk = jc->asObj();
			auto& hk = hks.emplace_back();
			auto& jKey = jHk.at(ObfusString("key"));
			if (jKey.isStr())
			{
				hk.vk = soup::string_to_virtual_key(jKey.reinterpretAsStr().value.data(), jKey.reinterpretAsStr().value.size());
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
			hk.was_pressed = hk.isPressed();
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
