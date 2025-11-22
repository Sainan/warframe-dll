#include "owf_label_replacements.hpp"

#include <joaat.hpp>
#include <ObfusString.hpp>
#include <string.hpp>

#include <lauxlib.h>
#include <lstate.h>

#include "owf_repo.hpp"
#include "owf_scripting.hpp"

using namespace soup;

static lua_State* L;

void load_label_replacements()
{
	{
		size_t size;
		const char* data;
		{
			std::lock_guard lock(g_repo_mtx); // Cannot be locked during lua_pcall as it would make owf_repo_find fail because g_repo_mtx is not recursive.
			data = g_repo.find(soup::joaat::compileTimeHash("OpenWF/helpers/pre_load_label_replacements.pluto"), size);
		}
		auto L = luaL_newstate();
		owfScript::openLibs(L);
		if (luaL_loadbuffer(L, data, size, nullptr) != LUA_OK
			|| lua_pcall(L, 0, 0, 0) != LUA_OK
			)
		{
			owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error").str());
		}
		lua_close(L);
	}

	std::lock_guard lock(label_replacements_mtx);

	if (L)
	{
		lua_close(L);
	}

	L = luaL_newstate();
	owfScript::openLibs(L);

	ObfusString path("OpenWF/Label Replacements.pluto");
	if (luaL_loadfile(L, path.c_str()) != LUA_OK
		|| lua_pcall(L, 0, 1, 0) != LUA_OK
		)
	{
		owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error").str());

		lua_close(L);
		L = nullptr;
	}
}

void do_label_replacements(GameString* str, GameString* loctag)
{
	std::lock_guard lock(label_replacements_mtx);
	if (L)
	{
		// Stack now: func
		lua_pushvalue(L, -1);
		// Stack now: func, func
		lua_pushlstring(L, loctag->getData(), loctag->getSize());
		// Stack now: func, func, loctag
		lua_pushlstring(L, str->getData(), str->getSize());
		// Stack now: func, func, loctag, str
		lua_pcall(L, 2, 1, 0);
		// Stack now: func, res
		if (lua_type(L, -1) == LUA_TSTRING)
		{
			size_t len;
			const char* data = lua_tolstring(L, -1, &len);
			if (len != str->getSize() || memcmp(data, str->getData(), len) != 0)
			{
				if (len == loctag->getSize() && memcmp(data, loctag->getData(), len) == 0)
				{
					std::swap(*str, *loctag);
				}
				else
				{
					const auto ps = fossilise_string(data, len);
					str->setUnownedData(ps->data, len);
				}
			}
		}
		lua_pop(L, 1);
		// Stack now: func
	}
}
