#include "owf_scripting.hpp"

#include <iostream>
#include <mutex>

#include <crc32c.hpp>
#include <filesystem.hpp>
#include <joaat.hpp>
#include <JsonObject.hpp>
#include <Key.hpp> // char_to_virtual_key
#include <MemoryRefReader.hpp>
#include <Module.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <SharedLibrary.hpp>
#include <StringWriter.hpp>

#include <lualib.h>
#include <lauxlib.h>
#include <lstate.h>
#include <lstring.h> // plutoS_prealloc, plutoS_commit

#include "owf_archive.hpp"
#include "owf_cache.hpp"
#include "owf_config.hpp"
#include "owf_console.hpp"
#include "owf_label_replacements.hpp"
#include "owf_luau.hpp"
#include "owf_structs.hpp"
#include "owf_tunables.hpp"

using namespace soup;

extern const char* g_bootstrapper_title;

extern bool owf_command(const std::string& in, JsonObject& out);

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
	OWF_SET_GLOBAL(L, "print");

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
	OWF_SET_GLOBAL(L, "write_to_console");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::cout << concat_arguments(L) << '\n';
		return 0;
	});
	OWF_SET_GLOBAL(L, "print_to_console");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owfConsole::setTitle(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_console_set_title");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::lock_guard lock(script_log_mtx);
		lua_pushinteger(L, script_log.size());
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_script_log_len");

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
	OWF_SET_GLOBAL(L, "owf_get_script_log_sub");

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
	OWF_SET_GLOBAL(L, "owf_archive_find");

	if (string_pool)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushstring(L, resolve_string_handle(luaL_checkinteger(L, 1)));
			return 1;
		});
		OWF_SET_GLOBAL(L, "resolve_string_handle");
	}

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushlstring(L, build_label, 16);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_build_version");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushlstring(L, build_hash, build_hash[0] ? 22 : 0);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_build_hash");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, server_host);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_server_host");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, http_port);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_http_port");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, https_port);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_https_port");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, auth_query);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_get_auth_query");

	lua_pushstring(L, g_bootstrapper_title);
	OWF_SET_GLOBAL(L, "OWF_CLIENT_TITLE"); // undocumented

	OWF_SET_GLOBAL_INT(L, "OWF_CLIENT_HTTP_PORT", client_http_port); // undocumented

#if PRIVATE
	lua_pushboolean(L, true);
	lua_setglobal(L, "OWF_PRIVATE_BUILD");
#endif
}

static owfScript* get_script_by_instance_id(size_t instance_id)
{
	std::lock_guard lock(running_scripts_mtx);
	for (const auto& scr : running_scripts)
	{
		if (scr->instance_id == instance_id)
		{
			return scr;
		}
	}
	return nullptr;
}

static size_t next_instance_id = 0;

owfScript::owfScript()
	: instance_id(next_instance_id++)
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
	OWF_SET_GLOBAL(L, "owf_get_stop_requested");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto scr = reinterpret_cast<owfScript*>(L->l_G->user_data);
		SOUP_IF_UNLIKELY (scr->callback_context)
		{
			ObfusString err("Cannot yield in a callback context");
			luaL_error(L, err.c_str());
		}
		SOUP_IF_UNLIKELY (scr->stop_requested)
		{
			ObfusString err("Stop requested");
			luaL_error(L, err.c_str());
		}
		lua_yield(L, 0);
		return 0;
	});
	OWF_SET_GLOBAL(L, "yield");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		incnny(L);
		return 0;
	});
	OWF_SET_GLOBAL(L, "block_yield");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		decnny(L);
		return 0;
	});
	OWF_SET_GLOBAL(L, "unblock_yield");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, lang_code);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_lang_code");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (DWORD pid; GetWindowThreadProcessId(GetForegroundWindow(), &pid), pid == GetCurrentProcessId())
		{
			int vk = 0;
			if (lua_type(L, 1) == LUA_TSTRING)
			{
				size_t size;
				const char* data = luaL_checklstring(L, 1, &size);
				vk = soup::string_to_virtual_key(data, size);
			}
			if (!vk)
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
	OWF_SET_GLOBAL(L, "owf_is_key_down");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_regionmgr");

	/*lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? *regionmgr->game_rules : nullptr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_gamerules");*/

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, flashmgr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_flashmgr");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, gamedata);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_gamedata");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, profilemgr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_profilemgr");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, gClient);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_client");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, matchingservice);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_matchingservice");

	/*lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? regionmgr->GetLocalPlayer() : nullptr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_local_player");*/

	/*lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, regionmgr ? regionmgr->GetGameCamera() : nullptr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_game_camera");*/

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->getAvatar());
		return 1;
	});
	OWF_SET_GLOBAL(L, "player_get_avatar");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->controlling_camera);
		return 1;
	});
	OWF_SET_GLOBAL(L, "player_get_controlling_camera");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		reinterpret_cast<Player*>(luaL_checkinteger(L, 1))->controlling_camera = lua_toboolean(L, 2);
		return 0;
	});
	OWF_SET_GLOBAL(L, "player_set_controlling_camera");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto entity = reinterpret_cast<Entity*>(luaL_checkinteger(L, 1));
		lua_pushnumber(L, entity->pos_x);
		lua_pushnumber(L, entity->pos_y);
		lua_pushnumber(L, entity->pos_z);
		return 3;
	});
	OWF_SET_GLOBAL(L, "entity_get_pos");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<BaseAvatar*>(luaL_checkinteger(L, 1))->getDamageController());
		return 1;
	});
	OWF_SET_GLOBAL(L, "baseavatar_get_damage_controller");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<BaseAvatar*>(luaL_checkinteger(L, 1))->getInventoryController());
		return 1;
	});
	OWF_SET_GLOBAL(L, "baseavatar_get_inventory_controller");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera() = lua_toboolean(L, 2);
		return 0;
	});
	OWF_SET_GLOBAL(L, "avatar_set_followed_by_camera");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera());
		return 1;
	});
	OWF_SET_GLOBAL(L, "avatar_get_followed_by_camera");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<LotusInventoryController*>(luaL_checkinteger(L, 1))->GetWeaponInHand(luaL_checkinteger(L, 2)));
		return 1;
	});
	OWF_SET_GLOBAL(L, "inventory_get_weapon_in_hand");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<LotusInventoryController*>(luaL_checkinteger(L, 1))->GetActivePowerSuit());
		return 1;
	});
	OWF_SET_GLOBAL(L, "inventory_get_active_powersuit");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushpointer(L, reinterpret_cast<WeaponEx*>(luaL_checkinteger(L, 1))->GetActiveImpactBehavior());
		return 1;
	});
	OWF_SET_GLOBAL(L, "weaponex_get_active_impact_behavior");

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
	OWF_SET_GLOBAL(L, "mem_scan_exe");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int8_t*>(L, 1));
		return 1;
	});
	OWF_SET_GLOBAL(L, "mem_read_i8");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int16_t*>(L, 1));
		return 1;
	});
	OWF_SET_GLOBAL(L, "mem_read_i16");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int32_t*>(L, 1));
		return 1;
	});
	OWF_SET_GLOBAL(L, "mem_read_i32");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, *lua_checkpointer<int64_t*>(L, 1));
		return 1;
	});
	OWF_SET_GLOBAL(L, "mem_read_i64");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		*lua_checkpointer<int64_t*>(L, 1) = luaL_checkinteger(L, 2);
		return 0;
	});
	OWF_SET_GLOBAL(L, "mem_write_i64");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, *lua_checkpointer<float*>(L, 1));
		return 1;
	});
	OWF_SET_GLOBAL(L, "mem_read_f32");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		*lua_checkpointer<float*>(L, 1) = luaL_checknumber(L, 2);
		return 0;
	});
	OWF_SET_GLOBAL(L, "mem_write_f32");

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
	OWF_SET_GLOBAL(L, "ivkr_push_nil");

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
	OWF_SET_GLOBAL(L, "ivkr_push_bool");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (!luau_push_number(luau_L, static_cast<float>(luaL_checkinteger(L, 1))))
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_push_int");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (!luau_push_number(luau_L, static_cast<float>(luaL_checknumber(L, 1))))
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_push_float");

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
		OWF_SET_GLOBAL(L, "ivkr_push_string");
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
		OWF_SET_GLOBAL(L, "ivkr_push_pointer");
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
		OWF_SET_GLOBAL(L, "ivkr_push_object");
	}

	if (luau_pushcclosurek)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			const auto cid = luaL_checkinteger(L, 1);
			SOUP_IF_LIKELY (luau_push_lightuserdata(luau_L, reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<owfScript*>(L->l_G->user_data)->instance_id))))
			{
				SOUP_IF_LIKELY (luau_push_lightuserdata(luau_L, reinterpret_cast<void*>(static_cast<uintptr_t>(cid))))
				{
					luau_pushcclosurek(luau_L, [](luau_State* L) -> int
					{
						int npushed = 0;
						if (const auto scr = get_script_by_instance_id(L->ci->func->value.gc->cl.c.upvals[0].value.as_uintptr))
						{
							const auto cid = L->ci->func->value.gc->cl.c.upvals[1].value.as_uintptr;
							//std::cout << "Callback " << cid << " invoked with " << luau_gettop(L) << " arguments" << std::endl;
							/*L->global_state_error_longjump_data() = nullptr;
							L->global_state_panic_func() = [](luau_State* L, int)
							{
								std::cout << "LuaU is panicking" << std::endl;
								luau_error_msg = (--L->outtop)->getString();
								std::cout << luau_error_msg << std::endl;
								throw 0;
							};*/
							scr->callback_context = true;
							if (luau_L)
							{
								const auto og_L = luau_L;
								luau_L = L;
								ObfusString str("owf_internal_callback");
								lua_getglobal(scr->main, str.c_str());
								lua_pushinteger(scr->main, cid);
								lua_pushinteger(scr->main, luau_gettop(L));
								lua_call(scr->main, 2, 1);
								npushed = lua_tonumber(scr->main, -1);
								luau_L = og_L;
							}
							else
							{
								luau_L = L;
								lua_pushinteger(scr->coro, cid);
								lua_pushinteger(scr->coro, luau_gettop(L));
								const int nresults = scr->tick(2);
								//std::cout << "Runtime yielded " << nresults << " value(s)" << std::endl;
								if (nresults == 1)
								{
									npushed = lua_tonumber(scr->coro, -1);
								}
								luau_L = nullptr;
							}
							scr->callback_context = false;
							//std::cout << "Runtime indicates it has pushed " << npushed << " value(s) to LuaU" << std::endl;
						}
						else
						{
							//std::cout << "Callback invoked for a dead script" << std::endl;
						}
						return npushed;
					}, nullptr, 2, nullptr);
					return 0;
					//luau_L->outtop -= 1;
				}
				luau_L->outtop -= 1;
			}
			luaL_error(L, ObfusString("insufficient space"));
		});
		OWF_SET_GLOBAL(L, "ivkr_push_callback2");
	}

	if (luau_next)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushboolean(L, luau_next(luau_L, (int)luaL_checkinteger(L, 1)));
			return 1;
		});
		OWF_SET_GLOBAL(L, "ivkr_next");
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
	OWF_SET_GLOBAL(L, "ivkr_push_userdata");

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
	OWF_SET_GLOBAL(L, "ivkr_push_lightuserdata");

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
	OWF_SET_GLOBAL(L, "ivkr_push_value");

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
	OWF_SET_GLOBAL(L, "ivkr_call");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, luau_savestack(luau_L, luau_L->outtop) / sizeof(luau_TValue));
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_get_top");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		luau_L->outtop = luau_restorestack(luau_L, luaL_checkinteger(L, 1) * sizeof(luau_TValue));
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_set_top");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, luau_savestack(luau_L, luau_L->intop) / sizeof(luau_TValue));
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_get_base");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		luau_L->intop = luau_restorestack(luau_L, luaL_checkinteger(L, 1) * sizeof(luau_TValue));
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_set_base");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		luau_L->outtop -= luaL_optinteger(L, 1, 1);
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_BOOL)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushboolean(L, (--luau_L->outtop)->value.as_bool);
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop_bool");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_NUMBER)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushnumber(L, (--luau_L->outtop)->value.as_float);
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop_number");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_STRING)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushstring(L, (--luau_L->outtop)->getString());
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop_string");

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
	OWF_SET_GLOBAL(L, "ivkr_pop_userdata");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushinteger(L, luau_L->outtop[-1].value.as_uintptr);
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_get_userdata");

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
	OWF_SET_GLOBAL(L, "ivkr_pop_pointer");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_USERDATA)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushpointer(L, (--luau_L->outtop)->getObject());
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop_object");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_FUNCTION || !reinterpret_cast<luau_Closure*>(luau_L->outtop[-1].value.as_uintptr)->isC)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		lua_pushpointer(L, reinterpret_cast<void*>(reinterpret_cast<luau_Closure*>((--luau_L->outtop)->value.as_uintptr)->c.func));
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_pop_c_function");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto idx = (uint8_t)luaL_checkinteger(L, 1);
		SOUP_IF_UNLIKELY (luau_L->outtop[-1].type != LUAU_FUNCTION)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		const auto closure = reinterpret_cast<luau_Closure*>(luau_L->outtop[-1].value.as_uintptr);
		SOUP_IF_UNLIKELY (idx >= closure->nupvalues)
		{
			luaL_error(L, ObfusString("index out of range"));
		}
		luau_TValue* const arr = closure->isC ? closure->c.upvals : closure->l.uprefs;
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_TValue* tval = &arr[idx];
		if (tval->type == LUAU_TUPVAL)
		{
			tval = tval->value.gc->uv.v;
		}
		*luau_L->outtop = *tval;
		luau_L->outtop++;
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_get_upvalue");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto idx = (uint8_t)luaL_checkinteger(L, 1);
		SOUP_IF_UNLIKELY (luau_L->outtop[-2].type != LUAU_FUNCTION)
		{
			luaL_error(L, ObfusString("unexpected type"));
		}
		const auto closure = reinterpret_cast<luau_Closure*>(luau_L->outtop[-2].value.as_uintptr);
		SOUP_IF_UNLIKELY (idx >= closure->nupvalues)
		{
			luaL_error(L, ObfusString("index out of range"));
		}
		luau_TValue* const arr = closure->isC ? closure->c.upvals : closure->l.uprefs;
		SOUP_IF_UNLIKELY (luau_L->outtop == luau_L->stack_last)
		{
			luaL_error(L, ObfusString("insufficient space"));
		}
		luau_TValue* tval = &arr[idx];
		if (tval->type == LUAU_TUPVAL)
		{
			tval = tval->value.gc->uv.v;
		}
		*tval = *(--luau_L->outtop);
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_set_upvalue");

	OWF_SET_GLOBAL_INT(L, "IVKR_NIL", LUAU_NIL);
	OWF_SET_GLOBAL_INT(L, "IVKR_BOOL", LUAU_BOOL);
	OWF_SET_GLOBAL_INT(L, "IVKR_NUMBER", LUAU_NUMBER);
	OWF_SET_GLOBAL_INT(L, "IVKR_STRING", LUAU_STRING);
	OWF_SET_GLOBAL_INT(L, "IVKR_TABLE", LUAU_TABLE);
	OWF_SET_GLOBAL_INT(L, "IVKR_FUNCTION", LUAU_FUNCTION);

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, luau_L->getValue(luaL_checkinteger(L, 1))->type);
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_type");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto type = luau_L->getValue(luaL_checkinteger(L, 1))->type;
		lua_pushboolean(L, type == LUAU_LIGHTUSERDATA || type == LUAU_USERDATA);
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_isuserdata");

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
		OWF_SET_GLOBAL(L, "ivkr_gettable");
	}

	if (luau_createtable)
	{
		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_createtable(luau_L, 0, 0);
			return 0;
		});
		OWF_SET_GLOBAL(L, "ivkr_newtable");
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
		OWF_SET_GLOBAL(L, "ivkr_settable");
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
	OWF_SET_GLOBAL(L, "ivkr_get_types");

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
	OWF_SET_GLOBAL(L, "ivkr_get_methods");

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
	OWF_SET_GLOBAL(L, "ivkr_get_attributes");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
		{
			lua_pushstring(L, *e->second->parent_ptr_name);
			return 1;
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "ivkr_get_parent");

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
	OWF_SET_GLOBAL(L, "ivkr_get_enums");
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
	OWF_SET_GLOBAL(L, "ivkr_find_ctor");

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
	OWF_SET_GLOBAL(L, "ivkr_find_method");

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
	OWF_SET_GLOBAL(L, "ivkr_find_getter");

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
	OWF_SET_GLOBAL(L, "ivkr_find_setter");

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
	OWF_SET_GLOBAL(L, "ivkr_get_enum_value");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, owfOverlay::getWidth());
		lua_pushinteger(L, owfOverlay::getHeight());
		return 2;
	});
	OWF_SET_GLOBAL(L, "owf_overlay_get_size");

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
	OWF_SET_GLOBAL(L, "owf_overlay_add_rect");

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
	OWF_SET_GLOBAL(L, "owf_overlay_add_text");

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
	OWF_SET_GLOBAL(L, "owf_overlay_set_visibility");

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
	OWF_SET_GLOBAL(L, "owf_overlay_set_colour");

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
	OWF_SET_GLOBAL(L, "owf_overlay_set_text");

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
	OWF_SET_GLOBAL(L, "owf_overlay_remove");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		owfOverlay::redraw();
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_overlay_update");

	lua_pushcfunction(L, ([](lua_State* L) -> int
	{
		const auto font = luaL_checkinteger(L, 1) == 5 ? &RasterFont::simple5() : &RasterFont::simple8();
		auto [width, height] = font->measure(pluto_checkstring(L, 2));
		lua_pushinteger(L, width);
		lua_pushinteger(L, height);
		return 2;
	}));
	OWF_SET_GLOBAL(L, "owf_measure_text");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, fov_override);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_fov_override");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pause_always_stops_time = lua_toboolean(L, 1);
		return 0;
	});
	OWF_SET_GLOBAL(L, "set_pause_always_stops_time");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->subscribed_chat_prefixes.emplace(pluto_checkstring(L, 1), lua_toboolean(L, 2));
		return 0;
	});
	OWF_SET_GLOBAL(L, "chat_subscribe_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->subscribed_chat_prefixes.erase(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "chat_unsubscribe_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->subscribed_outgoing_chat_prefixes.emplace(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "chat_subscribe_outgoing_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->subscribed_outgoing_chat_prefixes.erase(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "chat_unsubscribe_outgoing_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->websocket_message_prefixes.emplace(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "register_websocket_message_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->websocket_message_prefixes.erase(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "unregister_websocket_message_prefix");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (ChatRedux_table
			&& luau_L->outtop != luau_L->stack_last
			)
		{
			luau_L->outtop->value.as_uintptr = ChatRedux_table;
			luau_L->outtop->type = LUAU_TABLE;
			luau_L->outtop++;
			lua_pushboolean(L, true);
		}
		else
		{
			lua_pushboolean(L, false);
		}
		return 1;
	});
	OWF_SET_GLOBAL(L, "ivkr_push_chat_redux");

	if (luauD_call)
	{
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
			SOUP_IF_UNLIKELY (!luau_error_msg.empty())
			{
				luaL_error(L, luau_error_msg.c_str());
			}
			return 0;
		});
		OWF_SET_GLOBAL(L, "ivkr_call2");
	}

	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_SUBMIT_CHAT_MESSAGE);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_OUTGOING_CHAT_MESSAGE);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_CUSTOM_ROUTE_SERVED);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_CALLBACK);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_SCRIPT_TRIGGERED);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_WEBSOCKET_MESSAGE);
	OWF_EXPOSE_INT_CONSTANT(L, OWF_EVT_SCRIPT_MESSAGE);

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
			case OWF_EVT_SUBMIT_CHAT_MESSAGE:
				pluto_pushstring(L, ObfusString("text").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				pluto_pushstring(L, ObfusString("blocked").str());
				lua_pushboolean(L, scr->events.front().intdata);
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
			case OWF_EVT_OUTGOING_CHAT_MESSAGE:
			case OWF_EVT_SCRIPT_MESSAGE:
				pluto_pushstring(L, ObfusString("data").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;

			case OWF_EVT_WEBSOCKET_MESSAGE:
				pluto_pushstring(L, ObfusString("sender").str());
				lua_pushinteger(L, scr->events.front().intdata);
				lua_settable(L, -3);
				pluto_pushstring(L, ObfusString("text").str());
				pluto_pushstring(L, scr->events.front().data);
				lua_settable(L, -3);
				break;
			}
			scr->events.pop_front();
			return 1;
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_internal_next_event");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, active_input_filter);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_active_input_filter");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushboolean(L, active_input_filter_allows_hotkeys);
		return 1;
	});
	OWF_SET_GLOBAL(L, "get_active_input_filter_allows_hotkeys");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		std::lock_guard lock(g_server_tunables_mtx);
		lua_pushboolean(L, g_server_tunables.getBool(soup::joaat::hash(luaL_checkstring(L, 1))));
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_tunables_has");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->custom_routes.emplace(soup::joaat::hash(luaL_checkstring(L, 1)), CustomRoute{ pluto_checkstring(L, 2), pluto_checkstring(L, 3) });
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_register_custom_route");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->custom_routes.erase(soup::joaat::hash(luaL_checkstring(L, 1)));
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_unregister_custom_route");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t tag_len;
		auto tag = luaL_checklstring(L, 1, &tag_len);

		std::string name = static_cast<owfScript*>(L->l_G->user_data)->name; // owf_script_get_path
		name.append(tag, tag_len);
		static_cast<owfScript*>(L->l_G->user_data)->callbacks.emplace(std::move(name));
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_register_callback");

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
	OWF_SET_GLOBAL(L, "owf_subscribe_to_script_trigger");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto script = luaL_checkstring(L, 1);
		const auto func = luaL_checkstring(L, 2);

		uint32_t hash = 0;
		hash = joaat::partialStr(script, hash);
		hash = joaat::partialStr(func, hash);
		joaat::finalise(hash);

		static_cast<owfScript*>(L->l_G->user_data)->subscribed_script_triggers.erase(hash);

		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_unsubscribe_from_script_trigger");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, static_cast<float>(luaL_checkinteger(L, 1)) + static_cast<float>(luaL_checkinteger(L, 2)));
		return 1;
	});
	OWF_SET_GLOBAL(L, "fltm_int_add");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, static_cast<float>(luaL_checkinteger(L, 1)) * static_cast<float>(luaL_checkinteger(L, 2)));
		return 1;
	});
	OWF_SET_GLOBAL(L, "fltm_int_mul");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, static_cast<float>(luaL_checknumber(L, 1)) + static_cast<float>(luaL_checknumber(L, 2)));
		return 1;
	});
	OWF_SET_GLOBAL(L, "fltm_float_add");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushnumber(L, static_cast<float>(luaL_checknumber(L, 1)) * static_cast<float>(luaL_checknumber(L, 2)));
		return 1;
	});
	OWF_SET_GLOBAL(L, "fltm_float_mul");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		/*if (pluto_checkstring(L, 1) == "[]")
		{
			luaL_error(L, ObfusString("u wot m8, no way you meant to broadcast []"));
		}*/
		owf_broadcast_message(pluto_checkstring(L, 1), luaL_optinteger(L, 2, 0));
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_broadcast_message");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		const auto cmd = pluto_checkstring(L, 1);
		try
		{
			if (JsonObject obj; owf_command(cmd, obj))
			{
				pluto_pushstring(L, obj.encode());
				return 1;
			}
		}
		catch (std::exception& e)
		{
			luaL_error(L, e.what());
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_command_raw");

	// Undocumented
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		pluto_pushstring(L, static_cast<owfScript*>(L->l_G->user_data)->name);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_script_get_path");

	// Undocumented
	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		lua_pushinteger(L, static_cast<owfScript*>(L->l_G->user_data)->instance_id);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_script_get_instance_id");

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
	OWF_SET_GLOBAL(L, "owf_cache_find");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		for (auto& e : open_cache_pairs)
		{
			delete e.second;
		}
		open_cache_pairs.clear();
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_cache_close");

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
		mr.u32_le(num_entries);
		cm->entries.reserve(num_entries);
		for (uint32_t i = 0; i != num_entries; ++i)
		{
			std::string path;
			mr.str_lp<u32_le_t>(path);
			CacheManifest::Entry& e = cm->entries.emplace(std::move(path), CacheManifest::Entry{}).first->second;
			mr.str(sizeof(e.hash), e.hash);
			mr.str(sizeof(e.unk), e.unk);
		}
		mr.u32_le(num_entries);
		cm->stripped_entries.reserve(num_entries);
		for (uint32_t i = 0; i != num_entries; ++i)
		{
			std::string path;
			mr.str_lp<u32_le_t>(path);
			CacheManifest::Entry& e = cm->stripped_entries.emplace(std::move(path), CacheManifest::Entry{}).first->second;
			mr.str(sizeof(e.hash), e.hash);
			mr.str(sizeof(e.unk), e.unk);
		}
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_cachemanifest_new");

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
	OWF_SET_GLOBAL(L, "owf_cachemanifest_get_hash");

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
	OWF_SET_GLOBAL(L, "owf_cachemanifest_set_hash");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto cm = (CacheManifest*)lua_touserdata(L, 1);
		StringWriter sw;
		uint32_t num_entries = cm->entries.size();
		sw.u32_le(num_entries);
		for (auto& e : cm->entries)
		{
			sw.str_lp<u32_le_t>(e.first);
			sw.str(sizeof(e.second.hash), e.second.hash);
			sw.str(sizeof(e.second.unk), e.second.unk);
		}
		num_entries = cm->stripped_entries.size();
		sw.u32_le(num_entries);
		for (auto& e : cm->stripped_entries)
		{
			sw.str_lp<u32_le_t>(e.first);
			sw.str(sizeof(e.second.hash), e.second.hash);
			sw.str(sizeof(e.second.unk), e.second.unk);
		}
		pluto_pushstring(L, sw.data);
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_cachemanifest_pack_entries");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t compressed_len;
		const char* compressed = luaL_checklstring(L, 1, &compressed_len);
		const size_t decompressed_size = luaL_checkinteger(L, 2);

		SharedLibrary lib(ObfusString("Tools/Oodle/x64/final/oo2core_9_win64.dll"));
		using OodleLZ_Decompress_t = int(*)(const char* inputData, size_t inputLen, void* outputData, size_t outputLen, int a5, int a6, int a7, size_t a8, size_t a9, size_t a10, size_t a11, size_t a12, size_t a13, int a14);
		SOUP_IF_LIKELY (auto OodleLZ_Decompress = (OodleLZ_Decompress_t)lib.getAddress(ObfusString("OodleLZ_Decompress")))
		{
			char shrtbuf[LUAI_MAXSHORTLEN];
			auto decompressed = plutoS_prealloc(L, shrtbuf, decompressed_size);
			OodleLZ_Decompress(compressed, compressed_len, decompressed, decompressed_size, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3);
			plutoS_commit(L, decompressed, decompressed_size);
			return 1;
		}
		return 0;
	});
	OWF_SET_GLOBAL(L, "oodle_decompress");

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
	OWF_SET_GLOBAL(L, "owf_replace_label");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t tag_len;
		auto tag = luaL_checklstring(L, 1, &tag_len);

		const auto hash = lower_hash(tag, tag_len);

		std::lock_guard lock(label_replacements_mtx);
		label_replacements.erase(hash);

		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_restore_label");
#endif

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		static_cast<owfScript*>(L->l_G->user_data)->channels.emplace(pluto_checkstring(L, 1));
		return 0;
	});
	OWF_SET_GLOBAL(L, "owf_script_register_channel");

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		auto channel = pluto_checkstring(L, 1);
		auto text = pluto_checkstring(L, 2);

		owfScript* target = nullptr;
		for (auto& scr : running_scripts)
		{
			if (scr->channels.contains(channel))
			{
				target = scr;
				break;
			}
		}
		if (!target && bgscript && bgscript->channels.contains(channel))
		{
			target = bgscript;
		}

		if (target)
		{
			JsonObject obj;
			obj.add(ObfusString("channel"), std::move(channel));
			obj.add(ObfusString("text"), std::move(text));
			target->events.emplace_back(OWF_EVT_SCRIPT_MESSAGE, obj.encode());
			lua_pushboolean(L, true);
		}
		else
		{
			lua_pushboolean(L, false);
		}
		return 1;
	});
	OWF_SET_GLOBAL(L, "owf_script_send_message");

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
		owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error").str());
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
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error").str());
		coro = nullptr;
	}
	else
	{
		owfScript::logNl(lua_type(main, -1) == LUA_TSTRING ? pluto_checkstring(main, -1) : ObfusString("Non-string script error").str());
	}
	return false;
}

bool owfScript::loadString(const std::string& name, const std::string& code)
{
	this->name = name;
	if (luaL_loadbuffer(main, code.data(), code.size(), this->name.c_str()) == LUA_OK)
	{
		coro = lua_newthread(main);
		luaL_ref(main, LUA_REGISTRYINDEX);
		lua_xmove(main, coro, 2);
		int nresults;
		if (lua_resume(coro, main, 1, &nresults) == LUA_YIELD)
		{
			return true;
		}
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error").str());
		coro = nullptr;
	}
	else
	{
		owfScript::logNl(lua_type(main, -1) == LUA_TSTRING ? pluto_checkstring(main, -1) : ObfusString("Non-string script error").str());
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
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error").str());
	}
	return false;
}

int owfScript::tick(int nargs)
{
	int nresults = 0;
	int status = lua_resume(coro, main, nargs, &nresults);
	SOUP_IF_UNLIKELY (status != LUA_YIELD && status != LUA_OK)
	{
		owfScript::logNl(lua_type(coro, -1) == LUA_TSTRING ? pluto_checkstring(coro, -1) : ObfusString("Non-string script error").str());
		return 0;
	}
	return nresults;
}
