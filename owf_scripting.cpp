#include "owf_scripting.hpp"

#include <iostream>
#include <mutex>

#include <crc32c.hpp>
#include <filesystem.hpp>
#include <joaat.hpp>
#include <JsonObject.hpp>
#include <MemoryRefReader.hpp>
#include <Module.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <SharedLibrary.hpp>
#include <StringWriter.hpp>

#include <lualib.h>
#include <lauxlib.h>
#include <lstate.h>

#include "owf_archive.hpp"
#include "owf_cache.hpp"
#include "owf_config.hpp"
#include "owf_label_replacements.hpp"
#include "owf_luau.hpp"
#include "owf_structs.hpp"
#include "owf_tunables.hpp"

using namespace soup;

extern void owf_broadcast_message(std::string&& msg);

static uint32_t wf_fnv_32(const char* str) noexcept
{
	uint32_t hash = 0xF42E1C3E; // They use this non-standard initial value
	for (; *str; ++str)
	{
		hash ^= (uint8_t)*str;
		hash *= 16777619u;
	}
	return hash;
}

static void lua_pushpointer(lua_State* L, void* ptr)
{
	if (ptr != nullptr)
	{
		lua_pushinteger(L, reinterpret_cast<intptr_t>(ptr));
	}
	else
	{
		lua_pushnil(L);
	}
}

template <typename T>
static T lua_checkpointer(lua_State* L, int i)
{
	auto ptr = reinterpret_cast<T>(luaL_checkinteger(L, 1));
	if (!ptr)
	{
		ObfusString err("Unexpected nullptr");
		luaL_error(L, err.c_str());
	}
	return ptr;
}

static ObfusString runtime_script_name("OpenWF Script Runtime");

static std::unordered_map<uint32_t, uintptr_t> lua_exe_scan_cache;

void owfScript::logNl(std::string msg)
{
	msg.push_back('\n');
	owfScript::log(std::move(msg));
}

void owfScript::log(std::string msg)
{
	std::cout << msg;

	size_t script_log_olen;
	size_t script_log_nlen;
	{
		std::lock_guard lock(script_log_mtx);
		script_log_olen = script_log.size();
		script_log.append(msg);
		script_log_nlen = script_log.size();
	}

	if (!bgscript)
	{
		JsonObject obj;
		obj.add(ObfusString("script_log_olen"), static_cast<int64_t>(script_log_olen));
		obj.add(ObfusString("script_log_nlen"), static_cast<int64_t>(script_log_nlen));
		obj.add(ObfusString("script_log_app"), std::move(msg));
		owf_broadcast_message(obj.encode());
	}
}

static std::string concat_arguments(lua_State* L)
{
	std::string msg;
	const int n = lua_gettop(L);
	for (int i = 0; i++ != n; )
	{
		size_t len;
		const char* str = luaL_tolstring(L, i, &len);
		msg.append(str, len);
		msg.push_back('\t');
	}
	if (!msg.empty())
	{
		msg.pop_back();
	}
	return msg;
}

void owfScript::openLibs(lua_State* L)
{
	luaL_openlibs(L);

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owfScript::logNl(concat_arguments(L));
		return 0;
	});
	{ ObfusString name("print"); lua_setglobal(L, name.c_str()); }

	{ ObfusString name("io"); lua_getglobal(L, name.c_str()); }
	{ ObfusString name("write"); lua_pushlstring(L, name.data(), name.size()); }
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owfScript::log(concat_arguments(L));
		return 0;
	});
	lua_settable(L, -3);

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::cout << concat_arguments(L);
		return 0;
	});
	{ ObfusString name("write_to_console"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::cout << concat_arguments(L) << '\n';
		return 0;
	});
	{ ObfusString name("print_to_console"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::lock_guard lock(script_log_mtx);
		lua_pushinteger(L, script_log.size());
		return 1;
	});
	{ ObfusString name("owf_get_script_log_len"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto i = luaL_checkinteger(L, 1);
		std::lock_guard lock(script_log_mtx);
		const auto data = script_log.data();
		const auto size = script_log.size();
		if (i < size)
		{
			lua_pushlstring(L, data + i, size - i);
			return 1;
		}
		return 0;
	});
	{ ObfusString name("owf_get_script_log_sub"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::lock_guard lock(g_archive_mtx);
		uint32_t size;
		if (auto data = g_archive.find(soup::joaat::hash(luaL_checkstring(L, 1)), size))
		{
			lua_pushlstring(L, data, size);
			return 1;
		}
		return 0;
	});
	{ ObfusString name("owf_archive_find"); lua_setglobal(L, name.c_str()); }

	if (string_pool)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushstring(L, resolve_string_handle(luaL_checkinteger(L, 1)));
			return 1;
		});
		{ ObfusString name("resolve_string_handle"); lua_setglobal(L, name.c_str()); }
	}

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushlstring(L, build_label, 16);
		return 1;
	});
	{ ObfusString name("owf_get_build_label"); lua_setglobal(L, name.c_str()); }

#if PRIVATE
	lua_pushboolean(L, true);
	lua_setglobal(L, "OWF_PRIVATE_BUILD");
#endif
}

owfScript::owfScript()
{
	auto L = luaL_newstate();
	this->main = L;
	L->l_G->user_data = this;

	owfScript::openLibs(L);

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, reinterpret_cast<owfScript*>(L->l_G->user_data)->stop_requested);
		return 1;
	});
	{ ObfusString name("owf_get_stop_requested"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (reinterpret_cast<owfScript*>(L->l_G->user_data)->stop_requested)
		{
			ObfusString err("Stop requested");
			luaL_error(L, err.c_str());
		}
		lua_yield(L, 0);
		return 0;
	});
	{ ObfusString name("yield"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, lang_code);
		return 1;
	});
	{ ObfusString name("get_lang_code"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (DWORD pid; GetWindowThreadProcessId(GetForegroundWindow(), &pid), pid == GetCurrentProcessId())
		{
			int vk = 0;
			if (lua_type(L, 1) == LUA_TSTRING)
			{
				vk = (int)*luaL_checkstring(L, 1);
			}
			if ((vk < 'A' || vk > 'Z')
				&& (vk < '0' || vk > '9')
				&& vk != ' '
				)
			{
				vk = (int)luaL_checkinteger(L, 1);
			}
			lua_pushboolean(L, (GetAsyncKeyState(vk) & 0x8000) != 0);
		}
		else
		{
			lua_pushboolean(L, false);
		}
		return 1;
	});
	{ ObfusString name("owf_is_key_down"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr);
		return 1;
	});
	{ ObfusString name("get_regionmgr"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? *regionmgr->game_rules : nullptr);
		return 1;
	});
	{ ObfusString name("get_gamerules"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, flashmgr);
		return 1;
	});
	{ ObfusString name("get_flashmgr"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, gamedata);
		return 1;
	});
	{ ObfusString name("get_gamedata"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, profilemgr);
		return 1;
	});
	{ ObfusString name("get_profilemgr"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, gClient);
		return 1;
	});
	{ ObfusString name("get_client"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, matchingservice);
		return 1;
	});
	{ ObfusString name("get_matchingservice"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? regionmgr->GetLocalPlayer() : nullptr);
		return 1;
	});
	{ ObfusString name("get_local_player"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? regionmgr->GetGameCamera() : nullptr);
		return 1;
	});
	{ ObfusString name("get_game_camera"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->getAvatar());
		return 1;
	});
	{ ObfusString name("player_get_avatar"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->controlling_camera);
		return 1;
	});
	{ ObfusString name("player_get_controlling_camera"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->controlling_camera = lua_toboolean(L, 2);
		return 0;
	});
	{ ObfusString name("player_set_controlling_camera"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto entity = reinterpret_cast<Entity*>(luaL_checkinteger(L, 1));
		lua_pushnumber(L, entity->pos_x);
		lua_pushnumber(L, entity->pos_y);
		lua_pushnumber(L, entity->pos_z);
		return 3;
	});
	{ ObfusString name("entity_get_pos"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<BaseAvatar*>(luaL_checkinteger(L, 1))->getDamageController());
		return 1;
	});
	{ ObfusString name("baseavatar_get_damage_controller"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<BaseAvatar*>(luaL_checkinteger(L, 1))->getInventoryController());
		return 1;
	});
	{ ObfusString name("baseavatar_get_inventory_controller"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera() = lua_toboolean(L, 2);
		return 0;
	});
	{ ObfusString name("avatar_set_followed_by_camera"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera());
		return 1;
	});
	{ ObfusString name("avatar_get_followed_by_camera"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<LotusInventoryController*>(luaL_checkinteger(L, 1))->GetWeaponInHand(luaL_checkinteger(L, 2)));
		return 1;
	});
	{ ObfusString name("inventory_get_weapon_in_hand"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<LotusInventoryController*>(luaL_checkinteger(L, 1))->GetActivePowerSuit());
		return 1;
	});
	{ ObfusString name("inventory_get_active_powersuit"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<WeaponEx*>(luaL_checkinteger(L, 1))->GetActiveImpactBehavior());
		return 1;
	});
	{ ObfusString name("weaponex_get_active_impact_behavior"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t len;
		const char* str = luaL_checklstring(L, 1, &len);
		const auto cache_key = soup::joaat::hashRange(str, len);
		if (auto e = lua_exe_scan_cache.find(cache_key); e != lua_exe_scan_cache.end())
		{
			lua_pushinteger(L, e->second);
		}
		else
		{
			const auto res = Module(nullptr).range.scan(Pattern(str, len)).as<uintptr_t>();
			lua_exe_scan_cache.emplace(cache_key, res);
			lua_pushinteger(L, res);
		}
		return 1;
	});
	{ ObfusString name("mem_scan_exe"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int8_t*>(L, 1));
		return 1;
	});
	{ ObfusString name("mem_read_i8"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int16_t*>(L, 1));
		return 1;
	});
	{ ObfusString name("mem_read_i16"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int32_t*>(L, 1));
		return 1;
	});
	{ ObfusString name("mem_read_i32"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int64_t*>(L, 1));
		return 1;
	});
	{ ObfusString name("mem_read_i64"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		*lua_checkpointer<int64_t*>(L, 1) = luaL_checkinteger(L, 2);
		return 0;
	});
	{ ObfusString name("mem_write_i64"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, *lua_checkpointer<float*>(L, 1));
		return 1;
	});
	{ ObfusString name("mem_read_f32"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		*lua_checkpointer<float*>(L, 1) = luaL_checknumber(L, 2);
		return 0;
	});
	{ ObfusString name("mem_write_f32"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->type = LUAU_NIL;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_nil"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->value.as_bool = lua_toboolean(L, 1);
		luau_L->outtop->type = LUAU_BOOL;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_bool"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->value.as_float = static_cast<float>(luaL_checkinteger(L, 1));
		luau_L->outtop->type = LUAU_NUMBER;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_int"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->value.as_float = static_cast<float>(luaL_checknumber(L, 1));
		luau_L->outtop->type = LUAU_NUMBER;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_float"); lua_setglobal(L, name.c_str()); }

	if (luau_pushstring)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
			{
				luaL_error(L, ObfusString("insufficient space"));
			}
			const char* str = luaL_checkstring(L, 1);
			luau_pushstring(luau_L, str);
			return 0;
		});
		{ ObfusString name("ivkr_push_string"); lua_setglobal(L, name.c_str()); }
	}

	if (luau_pushpointer)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
			{
				luaL_error(L, ObfusString("insufficient space"));
			}
			luau_pushpointer(luau_L, reinterpret_cast<void*>(luaL_checkinteger(L, 1)));
			return 0;
		});
		{ ObfusString name("ivkr_push_pointer"); lua_setglobal(L, name.c_str()); }
	}

	if (luau_pushobject)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
			{
				luaL_error(L, ObfusString("insufficient space"));
			}
			auto obj = reinterpret_cast<Object*>(luaL_checkinteger(L, 1));
			luau_pushobject(luau_L, obj);
			/*luau_obj_buf[3] = &obj->self_pointer;
			luau_L->outtop->value.as_uintptr = reinterpret_cast<uintptr_t>(&luau_obj_buf[0]);
			luau_L->outtop->type = LUAU_USERDATA;
			luau_L->outtop++;*/
			/*if (***(void****)(luau_L->outtop[-1].value.as_uintptr + 0x18) != obj)
			{
				ObfusString err("invalid object");
				luaL_error(L, err.c_str());
			}*/
			return 0;
		});
		{ ObfusString name("ivkr_push_object"); lua_setglobal(L, name.c_str()); }
	}

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->value.as_uintptr = luaL_checkinteger(L, 1);
		luau_L->outtop->type = LUAU_USERDATA;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_userdata"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_L->outtop->value.as_uintptr = luaL_checkinteger(L, 1);
		luau_L->outtop->type = LUAU_LIGHTUSERDATA;
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_lightuserdata"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		*luau_L->outtop = *luau_L->getValue(luaL_checkinteger(L, 1));
		luau_L->outtop++;
		return 0;
	});
	{ ObfusString name("ivkr_push_value"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto f = lua_checkpointer<luau_CFunction>(L, 1);
		const auto nargs = (int)luaL_checkinteger(L, 2);

		luau_L->intop = luau_L->outtop - nargs;
		int nresults;
		luau_error_msg.clear();
		__try
		{
			nresults = f(luau_L);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (luau_error_msg.empty())
			{
				luau_error_msg = ObfusString("low-level exception").str();
			}
		}
		SOUP_IF_UNLIKELY (!luau_error_msg.empty())
		{
			luaL_error(L, luau_error_msg.c_str());
		}

		if (nargs != 0)
		{
			for (int i = 0; i != nresults; ++i)
			{
				luau_L->intop[i] = luau_L->intop[i + nargs];
			}
		}
		luau_L->outtop = luau_L->intop + nresults;
		lua_pushinteger(L, nresults);
		return 1;
	});
	{ ObfusString name("ivkr_call"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		luau_L->outtop -= luaL_optinteger(L, 1, 1);
		return 0;
	});
	{ ObfusString name("ivkr_pop"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_BOOL)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushboolean(L, (--luau_L->outtop)->value.as_bool);
		return 1;
	});
	{ ObfusString name("ivkr_pop_bool"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_NUMBER)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushnumber(L, (--luau_L->outtop)->value.as_float);
		return 1;
	});
	{ ObfusString name("ivkr_pop_number"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_STRING)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushstring(L, (--luau_L->outtop)->getString());
		return 1;
	});
	{ ObfusString name("ivkr_pop_string"); lua_setglobal(L, name.c_str()); }

	// Unused
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushinteger(L, (--luau_L->outtop)->value.as_uintptr);
		return 1;
	});
	{ ObfusString name("ivkr_pop_userdata"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushinteger(L, luau_L->outtop[-1].value.as_uintptr);
		return 1;
	});
	{ ObfusString name("ivkr_get_userdata"); lua_setglobal(L, name.c_str()); }

	// Unused
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushpointer(L, *(void**)((--luau_L->outtop)->value.as_uintptr + 0x18));
		return 1;
	});
	{ ObfusString name("ivkr_pop_pointer"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushpointer(L, (--luau_L->outtop)->getObject());
		return 1;
	});
	{ ObfusString name("ivkr_pop_object"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_FUNCTION || !reinterpret_cast<luau_Closure*>(luau_L->outtop[-1].value.as_uintptr)->isC)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushpointer(L, reinterpret_cast<void*>(reinterpret_cast<luau_Closure*>((--luau_L->outtop)->value.as_uintptr)->func));
		return 1;
	});
	{ ObfusString name("ivkr_pop_c_function"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, luau_L->getValue(luaL_checkinteger(L, 1))->type);
		return 1;
	});
	{ ObfusString name("ivkr_type"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, luau_L->getValue(luaL_checkinteger(L, 1))->type == LUAU_TABLE);
		return 1;
	});
	{ ObfusString name("ivkr_istable"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto type = luau_L->getValue(luaL_checkinteger(L, 1))->type;
		lua_pushboolean(L, type == LUAU_LIGHTUSERDATA || type == LUAU_USERDATA);
		return 1;
	});
	{ ObfusString name("ivkr_isuserdata"); lua_setglobal(L, name.c_str()); }

	if (luau_gettable)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			try
			{
				lua_pushinteger(L, luau_gettable(luau_L, luaL_checkinteger(L, 1)));
			}
			catch (const int&)
			{
				luaL_error(L, luau_error_msg.c_str());
			}
			return 1;
		});
		{ ObfusString name("ivkr_gettable"); lua_setglobal(L, name.c_str()); }
	}

	if (luau_createtable)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_createtable(luau_L, 0, 0);
			return 0;
		});
		{ ObfusString name("ivkr_newtable"); lua_setglobal(L, name.c_str()); }
	}

	if (luau_settable)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			try
			{
				luau_settable(luau_L, luaL_checkinteger(L, 1));
			}
			catch (const int&)
			{
				luaL_error(L, luau_error_msg.c_str());
			}
			return 0;
		});
		{ ObfusString name("ivkr_settable"); lua_setglobal(L, name.c_str()); }
	}

#if PRIVATE
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_Integer i = 0;
		lua_newtable(L);
		for (const auto& entry : swig_types)
		{
			lua_pushinteger(L, ++i);
			lua_pushstring(L, entry.second->name);
			lua_settable(L, -3);
		}
		return 1;
	});
	{ ObfusString name("ivkr_get_types"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			lua_Integer i = 0;
			lua_newtable(L);
			for (auto method = e->second->methods; method->hash != 0; ++method)
			{
				lua_pushinteger(L, ++i);
				lua_pushinteger(L, method->hash);
				lua_settable(L, -3);
			}
			return 1;
		}
		return 0;
	});
	{ ObfusString name("ivkr_get_methods"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			lua_Integer i = 0;
			lua_newtable(L);
			for (auto attr = e->second->attributes; attr->hash != 0; ++attr)
			{
				lua_pushinteger(L, ++i);
				lua_pushinteger(L, attr->hash);
				lua_settable(L, -3);
			}
			return 1;
		}
		return 0;
	});
	{ ObfusString name("ivkr_get_attributes"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			lua_pushstring(L, *e->second->parent_ptr_name);
			return 1;
		}
		return 0;
	});
	{ ObfusString name("ivkr_get_parent"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_newtable(L);
		lua_Integer n = 0;
		for (const auto& first : swig_enums)
		{
			for (auto i = first; i->name != nullptr; ++i)
			{
				lua_pushinteger(L, ++n);
				lua_newtable(L);
				{
					lua_pushstring(L, "name");
					lua_pushstring(L, i->name);
					lua_settable(L, -3);
				}
				{
					lua_pushstring(L, "value");
					lua_pushinteger(L, i->value);
					lua_settable(L, -3);
				}
				lua_settable(L, -3);
			}
		}
		return 1;
	});
	{ ObfusString name("ivkr_get_enums"); lua_setglobal(L, name.c_str()); }
#endif

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		void* res = nullptr;
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			res = reinterpret_cast<void*>(e->second->ctor);
		}
		lua_pushpointer(L, res);
		return 1;
	});
	{ ObfusString name("ivkr_find_ctor"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		void* res = nullptr;
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			res = reinterpret_cast<void*>(e->second->findMethod(lua_type(L, 2) == LUA_TNUMBER ? luaL_checkinteger(L, 2) : wf_fnv_32(luaL_checkstring(L, 2))));
		}
		lua_pushpointer(L, res);
		return 1;
	});
	{ ObfusString name("ivkr_find_method"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		void* res = nullptr;
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			res = reinterpret_cast<void*>(e->second->findGetter(wf_fnv_32(luaL_checkstring(L, 2))));
		}
		lua_pushpointer(L, res);
		return 1;
	});
	{ ObfusString name("ivkr_find_getter"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		void* res = nullptr;
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			res = reinterpret_cast<void*>(e->second->findSetter(wf_fnv_32(luaL_checkstring(L, 2))));
		}
		lua_pushpointer(L, res);
		return 1;
	});
	{ ObfusString name("ivkr_find_setter"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto target = luaL_checkstring(L, 1);
		for (const auto& first : swig_enums)
		{
			for (auto i = first; i->name != nullptr; ++i)
			{
				if (strcmp(i->name, target) == 0)
				{
					lua_pushinteger(L, i->value);
					return 1;
				}
			}
		}
		return 0;
	});
	{ ObfusString name("ivkr_get_enum_value"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, owfOverlay::getWidth());
		lua_pushinteger(L, owfOverlay::getHeight());
		return 2;
	});
	{ ObfusString name("owf_overlay_get_size"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = owfOverlay::addRect(
			luaL_checkinteger(L, 1),
			luaL_checkinteger(L, 2),
			luaL_checkinteger(L, 3),
			luaL_checkinteger(L, 4),
			luaL_checkinteger(L, 5),
			luaL_checkinteger(L, 6),
			luaL_checkinteger(L, 7)
		);
		static_cast<owfScript*>(L->l_G->user_data)->overlay_items.emplace(id);
		lua_pushlightuserdata(L, id);
		return 1;
	});
	{ ObfusString name("owf_overlay_add_rect"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = owfOverlay::addText(
			luaL_checkinteger(L, 1),
			luaL_checkinteger(L, 2),
			pluto_checkstring(L, 3),
			luaL_checkinteger(L, 4) == 5 ? &RasterFont::simple5() : &RasterFont::simple8(),
			luaL_checkinteger(L, 5),
			luaL_checkinteger(L, 6),
			luaL_checkinteger(L, 7),
			luaL_optinteger(L, 8, 1)
		);
		static_cast<owfScript*>(L->l_G->user_data)->overlay_items.emplace(id);
		lua_pushlightuserdata(L, id);
		return 1;
	});
	{ ObfusString name("owf_overlay_add_text"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = (owfOverlay::DrawItem*)lua_touserdata(L, 1);
		SOUP_IF_UNLIKELY (!id)
		{
			luaL_typeerror(L, 1, lua_typename(L, LUA_TLIGHTUSERDATA));
		}
		if (lua_toboolean(L, 2) ^ (id->type >= 0))
		{
			id->type *= -1;
		}
		return 0;
	});
	{ ObfusString name("owf_overlay_set_visibility"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = (owfOverlay::DrawItem*)lua_touserdata(L, 1);
		SOUP_IF_UNLIKELY (!id)
		{
			luaL_typeerror(L, 1, lua_typename(L, LUA_TLIGHTUSERDATA));
		}
		id->r = luaL_checkinteger(L, 2);
		id->g = luaL_checkinteger(L, 3);
		id->b = luaL_checkinteger(L, 4);
		return 0;
	});
	{ ObfusString name("owf_overlay_set_colour"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = (owfOverlay::Text*)lua_touserdata(L, 1);
		SOUP_IF_UNLIKELY (!id || id->getType() != owfOverlay::DrawItem::TEXT)
		{
			luaL_typeerror(L, 1, lua_typename(L, LUA_TLIGHTUSERDATA));
		}
		std::lock_guard lock(owfOverlay::mtx);
		id->text = pluto_checkstring(L, 2);
		return 0;
	});
	{ ObfusString name("owf_overlay_set_text"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto id = (owfOverlay::DrawItem*)lua_touserdata(L, 1);
		SOUP_IF_UNLIKELY (!id)
		{
			luaL_typeerror(L, 1, lua_typename(L, LUA_TLIGHTUSERDATA));
		}
		auto& overlay_items = static_cast<owfScript*>(L->l_G->user_data)->overlay_items;
		if (auto e = overlay_items.find(id); e != overlay_items.end())
		{
			overlay_items.erase(e);
			owfOverlay::remove(id);
		}
		return 0;
	});
	{ ObfusString name("owf_overlay_remove"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owfOverlay::redraw();
		return 0;
	});
	{ ObfusString name("owf_overlay_update"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, ([](lua_State* L) -> int
	{
		const auto font = luaL_checkinteger(L, 1) == 5 ? &RasterFont::simple5() : &RasterFont::simple8();
		auto [width, height] = font->measure(pluto_checkstring(L, 2));
		lua_pushinteger(L, width);
		lua_pushinteger(L, height);
		return 2;
	}));
	{ ObfusString name("owf_measure_text"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, fov_override);
		return 1;
	});
	{ ObfusString name("owf_config_get_fov_override"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pause_always_stops_time = lua_toboolean(L, 1);
		return 0;
	});
	{ ObfusString name("set_pause_always_stops_time"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->blocked_chat_prefixes.emplace(pluto_checkstring(L, 1));
		return 0;
	});
	{ ObfusString name("chat_block_prefix"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->blocked_chat_prefixes.erase(pluto_checkstring(L, 2));
		return 0;
	});
	{ ObfusString name("chat_unblock_prefix"); lua_setglobal(L, name.c_str()); }

	if (luauD_call)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (/*ChatRedux_table &&*/ ChatRedux_SystemMessage_method)
			{
				const auto message = luaL_checkstring(L, 1);

				const auto call_top = luau_L->outtop;

				luau_L->outtop->value.as_uintptr = ChatRedux_SystemMessage_method;
				luau_L->outtop->type = LUAU_FUNCTION;
				luau_L->outtop++;
				luau_L->outtop->value.as_uintptr = ChatRedux_table;
				luau_L->outtop->type = LUAU_TABLE;
				luau_L->outtop++;
				luau_pushstring(luau_L, message);

				luau_error_msg.clear();
				__try
				{
					luauD_call(luau_L, call_top, 0);
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					if (luau_error_msg.empty())
					{
						luau_error_msg = ObfusString("low-level exception").str();
					}
				}
				luau_L->outtop = call_top;
				SOUP_IF_UNLIKELY (!luau_error_msg.empty())
				{
					luaL_error(L, luau_error_msg.c_str());
				}
			}
			return 0;
		});
		{ ObfusString name("chat_system_reply"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			const auto nargs = luaL_checkinteger(L, 1);
			const auto nresults = luaL_checkinteger(L, 2);

			const auto call_top = luau_L->outtop - (nargs + 1);

			luau_error_msg.clear();
			__try
			{
				luauD_call(luau_L, call_top, nresults);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (luau_error_msg.empty())
				{
					luau_error_msg = ObfusString("low-level exception").str();
				}
			}
			luau_L->outtop = call_top + nresults;
			SOUP_IF_UNLIKELY (!luau_error_msg.empty())
			{
				luaL_error(L, luau_error_msg.c_str());
			}
			return 0;
		});
		{ ObfusString name("ivkr_call2"); lua_setglobal(L, name.c_str()); }
	}

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto scr = static_cast<owfScript*>(L->l_G->user_data);
		if (!scr->events.empty())
		{
			lua_newtable(L);
			{
				pluto_pushstring(L, ObfusString("type").str());
				lua_pushinteger(L, scr->events.front().type);
				lua_settable(L, -3);
			}
			switch (scr->events.front().type)
			{
			case OWF_EVT_BLOCKED_CHAT_MESSAGE:
				pluto_pushstring(L, ObfusString("text").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;

			case OWF_EVT_CUSTOM_ROUTE_SERVED:
				pluto_pushstring(L, ObfusString("path").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;

			case OWF_EVT_CALLBACK:
				pluto_pushstring(L, ObfusString("name").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;

			case OWF_EVT_SCRIPT_TRIGGERED:
				pluto_pushstring(L, ObfusString("data").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;
			}
			scr->events.pop_front();
			return 1;
		}
		return 0;
	});
	{ ObfusString name("owf_internal_next_event"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, active_input_filter);
		return 1;
	});
	{ ObfusString name("get_active_input_filter"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, owfTunables::has(soup::joaat::hash(luaL_checkstring(L, 1))));
		return 1;
	});
	{ ObfusString name("owf_tunables_has"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->custom_routes.emplace(soup::joaat::hash(luaL_checkstring(L, 1)), CustomRoute{ pluto_checkstring(L, 2), pluto_checkstring(L, 3) });
		return 0;
	});
	{ ObfusString name("owf_register_custom_route"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t tag_len;
		auto tag = luaL_checklstring(L, 1, &tag_len);

		std::string name = static_cast<owfScript*>(L->l_G->user_data)->name; // owf_script_get_path
		name.append(tag, tag_len);
		static_cast<owfScript*>(L->l_G->user_data)->callbacks.emplace(std::move(name));
		return 0;
	});
	{ ObfusString name("owf_register_callback"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto script = luaL_checkstring(L, 1);
		const auto func = luaL_checkstring(L, 2);
		const auto block = lua_toboolean(L, 3);

		uint32_t hash = 0;
		hash = joaat::partialStr(script, hash);
		hash = joaat::partialStr(func, hash);
		joaat::finalise(hash);

		static_cast<owfScript*>(L->l_G->user_data)->subscribed_script_triggers.emplace(hash, block);

		return 0;
	});
	{ ObfusString name("owf_subscribe_to_script_trigger"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, static_cast<float>(luaL_checkinteger(L, 1)) + static_cast<float>(luaL_checkinteger(L, 2)));
		return 1;
	});
	{ ObfusString name("fltm_int_add"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, static_cast<float>(luaL_checkinteger(L, 1)) * static_cast<float>(luaL_checkinteger(L, 2)));
		return 1;
	});
	{ ObfusString name("fltm_int_mul"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, static_cast<float>(luaL_checknumber(L, 1)) + static_cast<float>(luaL_checknumber(L, 2)));
		return 1;
	});
	{ ObfusString name("fltm_float_add"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, static_cast<float>(luaL_checknumber(L, 1)) * static_cast<float>(luaL_checknumber(L, 2)));
		return 1;
	});
	{ ObfusString name("fltm_float_mul"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owf_broadcast_message(pluto_checkstring(L, 1));
		return 0;
	});
	{ ObfusString name("owf_broadcast_message"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, static_cast<owfScript*>(L->l_G->user_data)->name);
		return 1;
	});
	{ ObfusString name("owf_script_get_path"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto cp_name = pluto_checkstring(L, 1);
		const auto cp_hash = joaat::hash(cp_name);
		size_t pathlen;
		const char* path = luaL_checklstring(L, 2, &pathlen);

		CachePair* cp;
		if (auto e = open_cache_pairs.find(cp_hash); e != open_cache_pairs.end())
		{
			cp = e->second;
		}
		else
		{
			try
			{
				cp = new CachePair(ObfusString("Cache.Windows/").str() + cp_name);
			}
			catch (const std::bad_alloc&)
			{
				cp = nullptr;
			}
			SOUP_IF_UNLIKELY (!cp || !cp->toc || !cp->cache)
			{
				delete cp;
				luaL_error(L, ObfusString("failed to open cache pair '%s'"), cp_name.c_str());
			}
			open_cache_pairs.emplace(cp_hash, cp);
		}

		if (auto entry = cp->findEntry(path, pathlen))
		{
			lua_pushlstring(L, (const char*)cp->cache + entry->cacheOffset, entry->compressedLen);
			lua_pushinteger(L, entry->length);
			return 2;
		}
		return 0;
	});
	{ ObfusString name("owf_cache_find"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		for (auto& e : open_cache_pairs)
		{
			delete e.second;
		}
		open_cache_pairs.clear();
		return 0;
	});
	{ ObfusString name("owf_cache_close"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t size;
		const char* data = luaL_checklstring(L, 1, &size);
		auto cm = new (lua_newuserdata(L, sizeof(CacheManifest))) CacheManifest{};
		{
			lua_newtable(L);
			{
				pluto_pushstring(L, ObfusString("__gc").str());
				lua_pushcfunction(L, [](lua_State* L) -> int
				{
					std::destroy_at<>((CacheManifest*)lua_touserdata(L, 1));
					return 0;
				});
				lua_settable(L, -3);
			}
			lua_setmetatable(L, -2);
		}
		MemoryRefReader mr(data, size);
		mr.skip(20);
		uint32_t num_entries;
		mr.u32le(num_entries);
		cm->entries.reserve(num_entries);
		for (uint32_t i = 0; i != num_entries; ++i)
		{
			std::string path;
			mr.str_lp<u32le_t>(path);
			CacheManifest::Entry& e = cm->entries.emplace(std::move(path), CacheManifest::Entry{}).first->second;
			mr.str(sizeof(e.hash), e.hash);
			mr.str(sizeof(e.unk), e.unk);
		}
		mr.u32le(num_entries);
		cm->stripped_entries.reserve(num_entries);
		for (uint32_t i = 0; i != num_entries; ++i)
		{
			std::string path;
			mr.str_lp<u32le_t>(path);
			CacheManifest::Entry& e = cm->stripped_entries.emplace(std::move(path), CacheManifest::Entry{}).first->second;
			mr.str(sizeof(e.hash), e.hash);
			mr.str(sizeof(e.unk), e.unk);
		}
		return 1;
	});
	{ ObfusString name("owf_cachemanifest_new"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto cm = (CacheManifest*)lua_touserdata(L, 1);
		const auto path = pluto_checkstring(L, 2);
		if (auto e = cm->entries.find(path); e != cm->entries.end())
		{
			lua_pushlstring(L, e->second.hash, sizeof(e->second.hash));
			return 1;
		}
		if (auto e = cm->stripped_entries.find(path); e != cm->stripped_entries.end())
		{
			lua_pushlstring(L, e->second.hash, sizeof(e->second.hash));
			return 1;
		}
		return 0;
	});
	{ ObfusString name("owf_cachemanifest_get_hash"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto cm = (CacheManifest*)lua_touserdata(L, 1);
		const auto path = pluto_checkstring(L, 2);
		size_t size;
		const auto data = luaL_checklstring(L, 3, &size);
		if (size == 16)
		{
			if (auto e = cm->entries.find(path); e != cm->entries.end())
			{
				memcpy(e->second.hash, data, size);
			}
			else if (auto e = cm->stripped_entries.find(path); e != cm->stripped_entries.end())
			{
				memcpy(e->second.hash, data, size);
			}
		}
		return 0;
	});
	{ ObfusString name("owf_cachemanifest_set_hash"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto cm = (CacheManifest*)lua_touserdata(L, 1);
		StringWriter sw;
		uint32_t num_entries = cm->entries.size();
		sw.u32le(num_entries);
		for (auto& e : cm->entries)
		{
			sw.str_lp<u32le_t>(e.first);
			sw.str(sizeof(e.second.hash), e.second.hash);
			sw.str(sizeof(e.second.unk), e.second.unk);
		}
		num_entries = cm->stripped_entries.size();
		sw.u32le(num_entries);
		for (auto& e : cm->stripped_entries)
		{
			sw.str_lp<u32le_t>(e.first);
			sw.str(sizeof(e.second.hash), e.second.hash);
			sw.str(sizeof(e.second.unk), e.second.unk);
		}
		pluto_pushstring(L, sw.data);
		return 1;
	});
	{ ObfusString name("owf_cachemanifest_pack_entries"); lua_setglobal(L, name.c_str()); }

	// ffi.alloc & ffi.read will be added in Pluto 0.11.0, but for now...
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t compressed_len;
		const char* compressed = luaL_checklstring(L, 1, &compressed_len);
		const size_t decompressed_size = luaL_checkinteger(L, 2);

		SharedLibrary lib(ObfusString("Tools/Oodle/x64/final/oo2core_9_win64.dll"));
		using OodleLZ_Decompress_t = int(*)(const char* inputData, size_t inputLen, void* outputData, size_t outputLen, int a5, int a6, int a7, size_t a8, size_t a9, size_t a10, size_t a11, size_t a12, size_t a13, int a14);
		SOUP_IF_LIKELY (auto OodleLZ_Decompress = (OodleLZ_Decompress_t)lib.getAddress(ObfusString("OodleLZ_Decompress")))
		{
			auto decompressed = soup::malloc(decompressed_size);
			OodleLZ_Decompress(compressed, compressed_len, decompressed, decompressed_size, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3);
			lua_pushlstring(L, (const char*)decompressed, decompressed_size);
			soup::free(decompressed);
			return 1;
		}
		return 0;
	});
	{ ObfusString name("oodle_decompress"); lua_setglobal(L, name.c_str()); }

	// crypto.crc32c will be added in Pluto 0.11.0, but for now...
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t len;
		const auto text = luaL_checklstring(L, 1, &len);
		const auto hash = soup::crc32c::hash((const uint8_t*)text, len);
		lua_pushinteger(L, hash);
		return 1;
	});
	{ ObfusString name("crc32c"); lua_setglobal(L, name.c_str()); }

#if LABEL_REPLACEMENTS
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t tag_len;
		auto tag = luaL_checklstring(L, 1, &tag_len);
		size_t str_len;
		auto str = luaL_checklstring(L, 2, &str_len);

		const auto hash = lower_hash(tag, tag_len);
		const auto ps = fossilise_string(str, str_len);

		std::lock_guard lock(label_replacements_mtx);
		if (auto e = label_replacements.find(hash); e != label_replacements.end())
		{
			e->second = ps;
		}
		else
		{
			label_replacements.emplace(hash, ps);
		}

		return 0;
	});
	{ ObfusString name("owf_replace_label"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t tag_len;
		auto tag = luaL_checklstring(L, 1, &tag_len);

		const auto hash = lower_hash(tag, tag_len);

		std::lock_guard lock(label_replacements_mtx);
		label_replacements.erase(hash);

		return 0;
	});
	{ ObfusString name("owf_restore_label"); lua_setglobal(L, name.c_str()); }
#endif

	std::string runtime;
#if PRIVATE
	runtime = string::fromFile(R"(OpenWF/runtime.pluto)");
	if (runtime.empty())
#endif
	{
		std::lock_guard lock(g_archive_mtx);
		uint32_t size;
		auto data = g_archive.find(soup::joaat::compileTimeHash("OpenWF/runtime.pluto"), size);
		runtime = std::string(data, size);
	}
	if (luaL_loadbuffer(L, runtime.data(), runtime.size(), runtime_script_name.c_str()) != LUA_OK
		|| lua_pcall(L, 0, 1, 0) != LUA_OK
		)
	{
		owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error while loading runtime").str());
	}
}

bool owfScript::loadFile(std::string&& path)
{
	this->name = std::move(path);
	if (luaL_loadfile(main, this->name.c_str()) == LUA_OK)
	{
		coro = lua_newthread(main);
		luaL_ref(main, LUA_REGISTRYINDEX);
		lua_xmove(main, coro, 2);
		int nresults;
		if (lua_resume(coro, main, 1, &nresults) == LUA_YIELD)
		{
			return true;
		}
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error on load").str());
		coro = nullptr;
	}
	else
	{
		owfScript::logNl(lua_type(main, -1) == LUA_TSTRING ? pluto_checkstring(main, -1) : ObfusString("Non-string script error on load").str());
	}
	return false;
}

bool owfScript::loadString(std::string&& code)
{
	this->name = std::move(code);
	if (luaL_loadbuffer(main, this->name.data(), this->name.size(), this->name.c_str()) == LUA_OK)
	{
		coro = lua_newthread(main);
		luaL_ref(main, LUA_REGISTRYINDEX);
		lua_xmove(main, coro, 2);
		int nresults;
		if (lua_resume(coro, main, 1, &nresults) == LUA_YIELD)
		{
			return true;
		}
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error on load").str());
		coro = nullptr;
	}
	else
	{
		owfScript::logNl(lua_type(main, -1) == LUA_TSTRING ? pluto_checkstring(main, -1) : ObfusString("Non-string script error on load").str());
	}
	return false;
}

bool owfScript::tick()
{
	int nresults;
	int status = lua_resume(coro, main, 0, &nresults);
	if (status == LUA_YIELD)
	{
		return true;
	}
	if (status != LUA_OK)
	{
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error on tick").str());
	}
	return false;
}
