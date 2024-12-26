#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.6.2"

#define LOGGING false
#define PRIVATE false

#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true
#define PROVIDE_VERSION_INFO true

#include <cstdlib>
#include <iostream>

#include <DetourHook.hpp>
#include <HttpRequest.hpp>
#include <joaat.hpp>
#include <json.hpp>
#include <memGuard.hpp>
#include <Module.hpp>
#include <netConfig.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <pattern_macros.hpp>
#include <Process.hpp>
#include <Server.hpp>
#include <ServerWebService.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <structing.hpp>
#include <Thread.hpp>
#include <Uri.hpp>

#include "whirlpool.hpp"

using namespace soup;

static bool console_attached = false;
static bool disabled_xp_based_level_cap = false;
#if PROVIDE_VERSION_INFO
static const char* build_label = nullptr; // e.g. "2024.12.14.10.37 Retail Windows x64"
static const char* build_hash = nullptr;
#endif
static bool fallback_language_was_used = false;
static bool fallback_graphicsDriver_was_used = false;
static bool did_auto_login = false;
static std::string auth_query; // e.g. "accountId=6633b81e9dba0b714f28ff02&nonce=8300464181160923&ct=MSI"

static std::string server_host;
static uint16_t http_port;
static uint16_t https_port;
static std::string fallback_language;
static std::string fallback_graphicsDriver;
static std::string fallback_cluster;
static bool high_damage_numbers_patch;
static bool skip_mission_start_timer;
static float fov_override;
static bool enable_http_interface;
static bool disable_nrs_connection;
static bool autologin;
static std::string autologin_email;
static std::string autologin_password;

static HMODULE og_lib;
static FARPROC og_DwmGetCompositionTimingInfo;

extern "C" __declspec(dllexport) void DwmGetCompositionTimingInfo() { og_DwmGetCompositionTimingInfo(); }

/*struct ParsedUrl
{
	char pad[16];
	char host[256];
};

static DetourHook parse_url_hook;

static bool parse_url_detour(const char* in, ParsedUrl* out)
{
#if LOGGING
	std::cout << "parse_url " << in << std::endl;
#endif
	//in = "https://" SERVER "/origin/CAFEBABE"; // SpaceNinjaServer expects this kind of path prefix
	if (reinterpret_cast<decltype(&parse_url_detour)>(parse_url_hook.original)(in, out))
	{
		//strcpy(out->host, SERVER);
		return true;
	}
	return false;
}*/

union GameString
{
	struct
	{
		char data[15];
		uint8_t inv_len;
	} shrt;
	struct
	{
		char* ptr;
		uint64_t metadata;
	} lng;

	[[nodiscard]] bool isLong() const noexcept { return shrt.inv_len == 0xFF; }
	//[[nodiscard]] bool willFreeData() const noexcept { return isLong() && (lng.metadata & 0xFFFFFFF0000000ull) != 0xFFFFFFF0000000ull; }
	[[nodiscard]] char* getData() noexcept { return isLong() ? lng.ptr : shrt.data; }
	[[nodiscard]] size_t getSize() const noexcept { return isLong() ? (lng.metadata & 0xFFFFFFF) : (sizeof(shrt.data) - shrt.inv_len); }

	void setUnownedData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			lng.ptr = (char*)data;
			lng.metadata = 0xFF'FFFFFFF'0000000ull | (len & 0xFFFFFFF);
		}
		else
		{
			memcpy(shrt.data, data, len);
			shrt.data[len] = 0;
			shrt.inv_len = sizeof(shrt.data) - len;
		}
	}

	void setShortData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			len = sizeof(shrt.data);
		}
		memcpy(shrt.data, data, len);
		shrt.data[len] = 0;
		shrt.inv_len = sizeof(shrt.data) - len;
	}

	void setShortData(const std::string& str) noexcept
	{
		return setShortData(str.data(), str.size());
	}

	void clear() noexcept
	{
		shrt.data[0] = '\0';
		shrt.inv_len = sizeof(shrt.data);
	}
};
static_assert(sizeof(GameString) == 0x10);

struct Arguments
{
	PAD(0, 0x04) bool silent;
	PAD(0x05, 0x18) bool client;
	PAD(0x19, 0x140) bool got_debugSession;
	/* 0x148 */ GameString debugSession;
	/* 0x158 */ bool got_clientType;
	/* 0x160 */ GameString clientType;
	PAD(0x160 + sizeof(GameString), 0x189) bool got_graphicsDriver;
	/* 0x190 */ GameString graphicsDriver;
	PAD(0x190 + sizeof(GameString), 0x1AC) bool got_language;
	/* 0x1B0 */ GameString language;
	PAD(0x1B0 + sizeof(GameString), 0x1C0) bool got_cluster;
	/* 0x1C8 */ GameString cluster;
	/* 0x1D8 */ GameString relaunch;
};
static_assert(offsetof(Arguments, got_graphicsDriver) == 0x189);
static_assert(offsetof(Arguments, graphicsDriver) == 0x190);
static_assert(offsetof(Arguments, got_language) == 0x1AC);
static_assert(offsetof(Arguments, language) == 0x1B0);
static_assert(offsetof(Arguments, got_cluster) == 0x1C0);
static_assert(offsetof(Arguments, cluster) == 0x1C8);
static_assert(offsetof(Arguments, relaunch) == 0x1D8);


static DetourHook winhttp_connect_hook;

static void* winhttp_connect_detour(void* a1, void* a2, int a3, const char* host_1, uint16_t port, const char* host_2, const char* host_3)
{
#if LOGGING
	std::cout << "winhttp_connect for " << host_1 << ", port " << port << std::endl;
	if (host_2 && *host_2)
	{
		std::cout << "host_2 = " << host_2 << std::endl;
	}
	if (host_3 && *host_3)
	{
		std::cout << "host_3 = " << host_3 << std::endl;
	}
#endif

	if (port == 80)
	{
		port = http_port;
	}
	else
	{
		port = https_port;
	}

	return reinterpret_cast<decltype(&winhttp_connect_detour)>(winhttp_connect_hook.original)(a1, a2, a3, server_host.c_str(), port, nullptr, nullptr);
}


static DetourHook winhttp_new_request_hook;
static int num_cache_requests = 0;
static bool showed_graphicsDriver_error = false;
static bool showed_language_error = false;

static void* winhttp_new_request_detour(void* a1, void* a2, void* a3, char* path, bool a5)
{
#if LOGGING
	std::cout << "winhttp_new_request: path = " << path << std::endl;
#endif
	char buff[1024];
	if (ObfusString cache_sub("/0/H.Cache.bin!D_---------------------w"); strstr(path, cache_sub.c_str()) != nullptr)
	{
		if (++num_cache_requests == 3)
		{
			std::cout << ObfusString("The game may fail to start as the server is unresponsive. Retrying.") << std::endl;
		}
#if PROVIDE_VERSION_INFO
		if (build_label)
		{
			auto i = strlen(path);
			memcpy(buff, path, i);
			path = buff;
			{
				ObfusString app("?version=");
				memcpy(&path[i], app.c_str(), app.size());
				i += app.size();
			}
			{
				memcpy(&path[i], build_label, 16);
				i += 16;
			}
			path[i] = '\0';
		}
#endif
	}
	else if (ObfusString lang_sub("/0/B.Cache.Windows_"); strstr(path, lang_sub.c_str()) != nullptr)
	{
		if (!showed_language_error)
		{
			showed_language_error = true;
			if (fallback_language_was_used)
			{
				ObfusString msg("The 'fallback_language' in your client_config.json does not seem to match your game files.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			else
			{
				ObfusString msg("The language that the game was supposed to launch with was not found in the game files.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
		}
	}
	else if (ObfusString dx_sub("/0/B.Cache.Dx"); strstr(path, dx_sub.c_str()) != nullptr)
	{
		if (!showed_graphicsDriver_error)
		{
			showed_graphicsDriver_error = true;
			if (fallback_graphicsDriver_was_used)
			{
				ObfusString msg("The 'fallback_graphicsDriver' in your client_config.json does not seem to match your game files.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			else
			{
				ObfusString msg("The graphicsDriver that the game was supposed to launch with was not found in the game files.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
		}
	}
	return reinterpret_cast<decltype(&winhttp_new_request_detour)>(winhttp_new_request_hook.original)(a1, a2, a3, path, a5);
}


static DetourHook game_http_request_hook;

struct GameHttpRequest
{
	/* 0x00 */ GameString url;
	PAD(0x10, 0x38) GameString body;
};
static_assert(offsetof(GameHttpRequest, body) == 0x38);

static void* game_http_request_detour(void* a1, GameHttpRequest* request, void* a3)
{
#if LOGGING
	std::cout << "game_http_request for " << (const char*)request->url.getData() << std::endl;
	/*if (request->body.getSize() != 0)
	{
		std::cout << request->body.getData() << std::endl;
	}*/
#endif

	std::string body_buf;

	Uri uri((const char*)request->url.getData());
	uri.host = server_host;
	if (uri.scheme.size() == 4) // "http"
	{
		if (http_port != 80)
		{
			uri.port = http_port;
		}
	}
	else
	{
		if (https_port != 443)
		{
			uri.port = https_port;
		}
	}
	if (uri.path == ObfusString("/api/inventory.php").str())
	{
#if DISABLE_XP_BASED_LEVEL_CAPPING
		if (disabled_xp_based_level_cap)
		{
			uri.query.append(ObfusString("&xpBasedLevelCapDisabled=1").str());
		}
#endif
	}
	else if (uri.path == ObfusString("/api/login.php").str())
	{
#if !LOGGING
		if (console_attached)
		{
			console_attached = false;
			const auto conWnd = GetConsoleWindow();
			FreeConsole();
			PostMessage(conWnd, WM_CLOSE, 0, 0);
		}
#endif
		if (autologin && !did_auto_login)
		{
			did_auto_login = true;
			if (auto jr = json::decode(request->body.getData()); jr && jr->isObj())
			{
				if (auto it = jr->reinterpretAsObj().findIt(ObfusString("email").str()); it != jr->reinterpretAsObj().end() && it->second->isStr())
				{
					it->second->reinterpretAsStr().value = autologin_email;
				}
				if (auto it = jr->reinterpretAsObj().findIt(ObfusString("password").str()); it != jr->reinterpretAsObj().end() && it->second->isStr())
				{
					it->second->reinterpretAsStr().value = autologin_password;
				}
				if (auto it = jr->reinterpretAsObj().findIt(ObfusString("kick").str()); it != jr->reinterpretAsObj().end())
				{
					// For some reason, ThemedMainMenu.lua sets the kick=true when dispatching login for "Client.AutoLogin".
					// However, as far as I can tell, this does not get persisted in any way, so I assume it's just something they do to ensure this is only used in their dev environment.
					// With "Steam.AutoLogin", we'd see kick=false as expected, but feels a bit more hacky.
					jr->reinterpretAsObj().erase(it);
				}
				body_buf = jr->encode();
				request->body.setUnownedData(body_buf.data(), body_buf.size());
			}
		}
#if PRIVATE
		if (strstr(request->body.getData(), "\"kick\"") != nullptr)
		{
			MessageBoxA(0, "ANTI-CHEAT TRIGGERED", "ANTI-CHEAT TRIGGERED", 0);
		}
#endif
#if PROVIDE_VERSION_INFO
		if (build_label && build_hash)
		{
			uri.query.append(ObfusString("&buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			uri.query.append(build_hash);
		}
#endif
	}
	else if (uri.path == ObfusString("/api/inbox.php").str())
	{
		auth_query = uri.query;
	}
	else if (uri.path.find(ObfusString("/dynamic/worldState.php").str()) != std::string::npos)
	{
#if PROVIDE_VERSION_INFO
		if (build_label && build_hash)
		{
			uri.query.append(ObfusString("buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			uri.query.append(build_hash);
		}
#endif
	}
#if PRIVATE
	/*else if (uri.path == "/api/heartbeat.php")
	{
		MessageBoxA(0, "ANTI-CHEAT TRIGGERED", "ANTI-CHEAT TRIGGERED", 0);
	}*/
#endif
	std::string url_buf = uri.toString();
	request->url.setUnownedData(url_buf.data(), url_buf.size());

	const auto ret = reinterpret_cast<decltype(&game_http_request_detour)>(game_http_request_hook.original)(a1, request, a3);

#if LOGGING
	// This now contains the response
	/*if (request->body.getSize() != 0)
	{
		std::cout << request->body.getData() << std::endl;
	}*/
#endif

	return ret;
}


static DetourHook Curl_resolv_hook;

static void* Curl_resolv_detour(void* a1, const char* hostname, int port, bool allowDOH, void* a5)
{
#if LOGGING
	std::cout << "Curl_resolv for " << hostname << ", port " << port << std::endl;
#endif

#if PRIVATE
	if (server_host != hostname)
	{
		MessageBoxA(0, "HOSTNAME MISMATCH", "HOSTNAME MISMATCH", 0);
	}
#endif

	return reinterpret_cast<decltype(&Curl_resolv_detour)>(Curl_resolv_hook.original)(a1, server_host.c_str(), port, allowDOH, a5);
}


static DetourHook ssl_verify_internal_hook;

static int64_t ssl_verify_internal_detour(void* a1, void* a2)
{
	//std::cout << "ssl_verify_internal called" << std::endl;
	return 1; // "Verify success"
}


static DetourHook Curl_ossl_verifyhost_hook;

static int64_t Curl_ossl_verifyhost_detour(void* a1, void* a2)
{
	//auto ret = reinterpret_cast<decltype(&Curl_ossl_verifyhost_detour)>(Curl_ossl_verifyhost_hook.original)(a1, a2);
	//std::cout << "Curl_ossl_verifyhost returned " << ret << std::endl;
	return 0;
}


static DetourHook verify_worldstate_integrity_hook;

static bool verify_worldstate_integrity_detour()
{
	return true;
}


/*static DetourHook int_rsa_verify_hook;

static int64_t int_rsa_verify_detour(void* a1, void* a2, void* a3, void* a4, size_t* a5, void* a6, void* a7, void* a8)
{
	//auto ret = reinterpret_cast<decltype(&int_rsa_verify_detour)>(int_rsa_verify_hook.original)(a1, a2, a3, a4, a5, a6, a7, a8);
	//std::cout << "int_rsa_verify returns " << ret << std::endl;
	if (a5)
	{
		*a5 = 1; // For > 0 return from pkey_rsa_verifyrecover
	}
	return 1;
}*/


static bool prohibit_skip_mission_start_timer = false;
static bool prohibit_fov_override = false;
static bool prohibit_freecam = false;
static bool prohibit_teleport = false;

static void on_got_server_host()
{
	std::cout << ObfusString("Redirecting requests to ") << server_host << std::endl;
	if (autologin && !did_auto_login)
	{
		std::cout << ObfusString("Will automatically log in") << std::endl;
	}

#if ASK_SERVER_FOR_TUNABLES
	Thread thrd([](Capture&&)
	{
		HttpRequest hr(server_host, ObfusString("/custom/tunables.json"));
		hr.port = https_port;
		hr.use_tls = true;
		netConfig::get().certchain_validator = &Socket::certchain_validator_none;

		UniquePtr<JsonNode> jr;
		if (auto res = hr.execute())
		{
			jr = json::decode(res->body);
		}

		prohibit_skip_mission_start_timer = jr && jr->isObj() && jr->reinterpretAsObj().contains(ObfusString("prohibit_skip_mission_start_timer").str());
		prohibit_fov_override = jr && jr->isObj() && jr->reinterpretAsObj().contains(ObfusString("prohibit_fov_override").str());
		prohibit_freecam = jr && jr->isObj() && jr->reinterpretAsObj().contains(ObfusString("prohibit_freecam").str());
		prohibit_teleport = jr && jr->isObj() && jr->reinterpretAsObj().contains(ObfusString("prohibit_teleport").str());

		if (prohibit_skip_mission_start_timer)
		{
			std::cout << ObfusString("Note: skip_mission_start_timer is prohibited on this server.") << std::endl;
		}
		if (prohibit_fov_override)
		{
			std::cout << ObfusString("Note: fov_override is prohibited on this server.") << std::endl;
		}
		if (prohibit_freecam)
		{
			std::cout << ObfusString("Note: freecam is prohibited on this server.") << std::endl;
		}
		if (prohibit_teleport)
		{
			std::cout << ObfusString("Note: teleport is prohibited on this server.") << std::endl;
		}
	});
	thrd.detach();
#endif
}

static void do_logout()
{
	if (!auth_query.empty())
	{
		HttpRequest hr(server_host, ObfusString("/api/logout.php?").str() + auth_query);
		hr.port = https_port;
		hr.use_tls = true;
		netConfig::get().certchain_validator = &Socket::certchain_validator_none;
		SOUP_UNUSED(hr.execute());
		auth_query.clear();
	}
}


static DetourHook parse_arguments_hook;
static bool processed_args = false;

static void parse_arguments_detour(Arguments* arguments, GameString* str, void* a3)
{
#if LOGGING
	std::cout << "parse_arguments: " << str->getData() << std::endl;
#endif

	reinterpret_cast<decltype(&parse_arguments_detour)>(parse_arguments_hook.original)(arguments, str, a3);

	if (!processed_args)
	{
		processed_args = true;
		for (const auto& arg : string::explode<std::string>(str->getData(), ' '))
		{
			if (arg.size() > 15 && arg.substr(0, 15) == ObfusString("-owfServerHost:").str())
			{
				server_host = arg.substr(15);
			}
		}
		on_got_server_host();
	}

	if (!arguments->got_language)
	{
		arguments->got_language = true;
		arguments->language.setShortData(fallback_language);
		fallback_language_was_used = true;
	}
	if (!arguments->got_graphicsDriver)
	{
		arguments->got_graphicsDriver = true;
		arguments->graphicsDriver.setShortData(fallback_graphicsDriver);
		fallback_graphicsDriver_was_used = true;
	}
	if (!arguments->got_cluster)
	{
		arguments->got_cluster = true;
		arguments->cluster.setShortData(fallback_cluster);
	}
}


static DetourHook SquadSetCountdownTimer_hook;

static __int64 SquadSetCountdownTimer_detour(void* a1, float seconds)
{
	//std::cout << "SquadSetCountdownTimer(" << seconds << ")" << std::endl;
	if (skip_mission_start_timer && !prohibit_skip_mission_start_timer && seconds == 5.9f)
	{
		seconds = 0.0f;
	}
	return reinterpret_cast<decltype(&SquadSetCountdownTimer_detour)>(SquadSetCountdownTimer_hook.original)(a1, seconds);
}


static DetourHook PostProcessInfo_getFov_hook;

static float PostProcessInfo_getFov_detour(uintptr_t a1)
{
	if (fov_override != 0.0f && !prohibit_fov_override)
	{
		// 0x888 seems to be cam rot pitch
		*reinterpret_cast<float*>(a1 + 0x898) = fov_override;
		return fov_override;
	}
	return *reinterpret_cast<float*>(a1 + 0x898);
}


static void* dmg_number_patch_addr;
static uint8_t dmg_number_trampoline[] = {
	0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)
	0x41, 0xff, 0xe2, // jmp r10
};
static uint8_t dmg_number_og_bytes[sizeof(dmg_number_trampoline)];

static void enable_dmg_number_patch()
{
	memGuard::setAllowedAccess(dmg_number_patch_addr, sizeof(dmg_number_trampoline), memGuard::ACC_RWX);
	memcpy(dmg_number_patch_addr, dmg_number_trampoline, sizeof(dmg_number_trampoline));
}

static void disable_dmg_number_patch()
{
	memcpy(dmg_number_patch_addr, dmg_number_og_bytes, sizeof(dmg_number_og_bytes));
}

static float last_dmg = 0.0f;

static float get_dmg_to_display(int dmg_int)
{
	float dmg_number = (dmg_int < 0 ? last_dmg : static_cast<float>(dmg_int));
#if LOGGING
	std::cout << "get_dmg_to_display: " << dmg_int << " -> " << dmg_number << std::endl;
#endif
	return dmg_number;
}

static DetourHook get_total_damage_hook;

static float get_total_damage_detour(__int64 *a1, __int64 a2, float a3, unsigned __int8 a4, float *a5, float *a6)
{
	float ret = reinterpret_cast<decltype(&get_total_damage_detour)>(get_total_damage_hook.original)(a1, a2, a3, a4, a5, a6);
#if LOGGING
	std::cout << "get_total_damage: " << ret << std::endl;
#endif
	if (ret != 0.0f)
	{
		last_dmg = ret;
	}
	//ret = FLT_MAX;
	return ret;
}


#if PROVIDE_VERSION_INFO
static DetourHook ReadCacheManifest_hook;

static bool ReadCacheManifest_detour(uintptr_t a1)
{
	bool ret = reinterpret_cast<decltype(&ReadCacheManifest_detour)>(ReadCacheManifest_hook.original)(a1);
	build_hash = reinterpret_cast<GameString*>(a1 + 0x1F0)->getData();
#if LOGGING
	std::cout << "cache manifest hash: " << build_hash << std::endl;
#endif
	return ret;
}
#endif


union lua_Value
{
	uintptr_t as_uintptr;
	bool as_bool;
};

enum lua_Type
{
	LUA_BOOL = 1,
	LUA_STRING = 5,
};

struct lua_TValue
{
	/* 0x00 */ lua_Value value;
	PAD(0x08, 0x0C) uint8_t type;

	[[nodiscard]] const char* getString() const noexcept
	{
		return reinterpret_cast<const char*>(value.as_uintptr + 0x18);
	}
};
static_assert(sizeof(lua_TValue) == 0x10);

struct lua_State
{
	PAD(0, 0x08) lua_TValue* outtop;
	/* 0x10 */ lua_TValue* intop;
};
static_assert(sizeof(lua_State) == 0x18);

/*static DetourHook get_config_bool_hook;

static int get_config_bool_detour(lua_State* L)
{
	SOUP_IF_LIKELY (L->intop[1].type == LUA_STRING)
	{
		ObfusString str("Steam.AutoLogin");
		if (strcmp(L->intop[1].getString(), str.c_str()) == 0)
		{
#if LOGGING
			std::cout << "Reporting Steam.AutoLogin as true" << std::endl;
#endif
			L->outtop[-1].value.as_bool = true;
			L->outtop[-1].type = LUA_BOOL;
			get_config_bool_hook.disable();
			return 1;
		}
	}

	return reinterpret_cast<decltype(&get_config_bool_detour)>(get_config_bool_hook.original)(L);
}*/


static DetourHook get_config_bool_vfunc_hook;

static bool get_config_bool_vfunc_detour(void* a1, const char* name, bool fallback)
{
	SOUP_IF_LIKELY (name)
	{
		ObfusString str("Client.AutoLogin");
		SOUP_IF_UNLIKELY (strcmp(name, str.c_str()) == 0)
		{
			if (!did_auto_login)
			{
#if LOGGING
				std::cout << "Reporting Client.AutoLogin as true" << std::endl;
#endif
				return true;
			}
		}
	}

	return reinterpret_cast<decltype(&get_config_bool_vfunc_detour)>(get_config_bool_vfunc_hook.original)(a1, name, fallback);
}


struct UnkControlsArg
{
};

struct Avatar
{
	struct Vftable
	{
		PAD(0, 0x610) void(*disableJumping)(Avatar*, UnkControlsArg*);
		/* 0x618 */ void(*enableJumping)(Avatar*, UnkControlsArg*);
	};

	/* 0x00 */ Vftable* vftable;
	PAD(0x08, 0x48) float mov_dir_x;
	/* 0x4C */ float mov_dir_y;
	/* 0x50 */ float mov_dir_z;
	PAD(0x54, 0x70) float pos_x; // Updating this position only takes effect while crouching.
	/* 0x74 */ float pos_y;
	/* 0x78 */ float pos_z;
	PAD(0x7C, 0xA0) float rot_x;
	/* 0xA4 */ float rot_y;
	/* 0xA8 */ float rot_z;
	PAD(0xAC, 0xD0) float body_pos_x;
	/* 0xD4 */ float body_pos_y;
	/* 0xD8 */ float body_pos_z;
	PAD(0x0DC, 0x0F0) float vel_x;
	/* 0xF4 */ float vel_y;
	/* 0xF8 */ float vel_z;
	PAD(0x0FC, 0x100) float pos2_x;
	/* 0x104 */ float pos2_y;
	/* 0x108 */ float pos2_z;
	PAD(0x10C, 0x110) float vis_x;
	/* 0x114 */ float vis_y;
	/* 0x118 */ float vis_z;
	PAD(0x11C, 0x500) float head_pos_x;
	/* 0x504 */ float head_pos_y;
	/* 0x508 */ float head_pos_z;
	PAD(0x50C, 0x511) bool followed_by_camera;
	PAD(0x512, 0x679) uint8_t movement_flags; // 2 = sprinting, 4 = crouching, 5 = sliding
	PAD(0x512, 0x6A0) bool render_above_everything;
};
static_assert(offsetof(Avatar, followed_by_camera) == 0x511);

struct Player
{
	PAD(0x000, 0x038) GameString name;
	PAD(0x048, 0x068) GameString name_with_platform_suffix;
	PAD(0x078, 0x090) GameString clan_name;
	PAD(0x0A0, 0x148) Avatar** avatar;
	PAD(0x150, 0x158) bool controlling_camera;
	PAD(0x159, 0x1A0) GameString mm_value;
	PAD(0x1B0, 0x1C0) GameString account_id;
	PAD(0x1D0, 0x13E0) UnkControlsArg unk_controls_arg;

	[[nodiscard]] Avatar* getAvatar() const noexcept { return *avatar; }
};
static_assert(offsetof(Player, controlling_camera) == 0x158);

static DetourHook calculate_spawn_position_hook;
static Player* local_player = nullptr;

static void* calculate_spawn_position_detour(void* a1, void* a2, void* a3, void* a4, void* a5, Player* player)
{
	if (player)
	{
		local_player = player;
#if LOGGING
		std::cout << "local_player = " << local_player << std::endl;
#endif
	}
	
	return reinterpret_cast<decltype(&calculate_spawn_position_detour)>(calculate_spawn_position_hook.original)(a1, a2, a3, a4, a5, player);
}


template <typename T>
struct LinkedList
{
	struct Entry
	{
		PAD(0, 0x10) void** unk;
		/* 0x18 */ Entry* _next;

		[[nodiscard]] T* getNext() noexcept
		{
			if (*_next->unk && _next != this)
			{
				return static_cast<T*>(_next);
			}
			return nullptr;
		}
	};
	static_assert(sizeof(Entry) == 0x20);

	uintptr_t _head;
	int unk1;
	int unk2;

	[[nodiscard]] T* getHead() noexcept
	{
		auto node = reinterpret_cast<Entry*>(_head - 0x10);
		if (*node->unk)
		{
			return static_cast<T*>(node);
		}
		return nullptr;
	}
};

enum MarkerType : uint8_t
{
	HUD_OBJECTIVE = 3, // (Diamond icon)
	HUD_TARGET1 = 9, // Exterminate
	HUD_LIFE_SUPPORT = 12,
	HUD_ELEVATOR = 14, // Typically only shows when nearby (without distance indicator)
	HUD_TARGET2 = 29, // Capture Target, Disruption Demolyst
	HUD_SPY_A = 40,
	HUD_SPY_B = 41,
	HUD_SPY_C = 42,
	HUD_WAYPOINT_1 = 49,
	HUD_FOCUS = 65,
	HUD_EXTRACT = 75,
	HUD_DISRUPTION = 79, // All keys & conduits seem to use this
};

struct Marker : public LinkedList<Marker>::Entry
{
	PAD(0x20, 0x4C) float world_x;
	/* 0x50 */ float world_y;
	/* 0x54 */ float world_z;
	PAD(0x58, 0x80) const char* label;
	PAD(0x88, 0xB0) MarkerType type;
	PAD(0xB1, 0xD0) int distance;
};

struct Hud
{
	PAD(0, 0xB80) LinkedList<Marker>** markers;
};

static DetourHook update_hud_hook;
static LinkedList<Marker>* markers = nullptr;

static bool update_hud_detour(Hud* hud, void* a2, void* a3, float a4)
{
	markers = *hud->markers;
	return reinterpret_cast<decltype(&update_hud_detour)>(update_hud_hook.original)(hud, a2, a3, a4);
}


static Thread tp_thrd;
static float tp_target_x;
static float tp_target_y;
static float tp_target_z;

static void teleport(float x, float y, float z)
{
	tp_target_x = x;
	tp_target_y = y;
	tp_target_z = z;
#if LOGGING
	std::cout << "[teleport] target set to " << tp_target_x << ", " << tp_target_y << ", " << tp_target_z << std::endl;
#endif
	if (!tp_thrd.isRunning())
	{
		tp_thrd.start([](Capture&&)
		{
#if LOGGING
			std::cout << "[teleport] thread started" << std::endl;
#endif
			size_t ticks_remaining = -1;
			while (--ticks_remaining != 0)
			{
				if (DWORD pid; GetWindowThreadProcessId(GetForegroundWindow(), &pid), pid == GetCurrentProcessId())
				{
					if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
					{
						if (ticks_remaining > 100)
						{
#if LOGGING
							std::cout << "[teleport] teleport to " << tp_target_x << ", " << tp_target_y << ", " << tp_target_z << " confirmed" << std::endl;
#endif
							ticks_remaining = 100;
						}
						auto avatar = local_player->getAvatar();
						local_player->controlling_camera = false;
						avatar->pos_x = tp_target_x;
						avatar->pos_y = tp_target_y;
						avatar->pos_z = tp_target_z;
					}
					else if (ticks_remaining > 100 && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
					{
#if LOGGING
						std::cout << "[teleport] teleport to " << tp_target_x << ", " << tp_target_y << ", " << tp_target_z << " canelled" << std::endl;
#endif
						break;
					}
				}
				Sleep(1);
			}
#if LOGGING
			std::cout << "[teleport] thread stopped" << std::endl;
#endif
		});
	}
}


static void save_config()
{
	JsonObject config;
	config.add(ObfusString("server_host"), server_host);
	config.add(ObfusString("http_port"), http_port);
	config.add(ObfusString("https_port"), https_port);
	config.add(ObfusString("fallback_language"), fallback_language);
	config.add(ObfusString("fallback_graphicsDriver"), fallback_graphicsDriver);
	config.add(ObfusString("fallback_cluster"), fallback_cluster);
	config.add(ObfusString("high_damage_numbers_patch"), high_damage_numbers_patch);
	config.add(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
	config.add(ObfusString("fov_override"), fov_override);
	config.add(ObfusString("enable_http_interface"), enable_http_interface);
	config.add(ObfusString("disable_nrs_connection"), disable_nrs_connection);
	config.add(ObfusString("autologin"), autologin);
	config.add(ObfusString("autologin_email"), autologin_email);
	config.add(ObfusString("autologin_password"), autologin_password);
	string::toFile(ObfusString("client_config.json").str(), config.encodePretty());
}

static void attach_console()
{
	AllocConsole();
	SetConsoleTitleA(BOOTSTRAPPER_TITLE);
	{
		FILE* f;
		freopen_s(&f, ObfusString("CONIN$"), ObfusString("r"), stdin);
		freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stderr);
		freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stdout);
	}
	console_attached = true;
}

#define CONFIG_LOADED_ONLY_ONCE true

[[nodiscard]] static bool is_valid_whirlpool_hex_digest(const std::string& str) noexcept
{
	if (str.size() != 128)
	{
		return false;
	}
	for (const auto c : str)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}
	return true;
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, PVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hmod);

		if (auto proc = soup::Process::current(); proc->name != "Warframe.x64.exe")
		{
			MessageBoxA(0, "Please only put the dwmapi.dll in your Warframe installation folder.", BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			return FALSE;
		}

		attach_console();

		{
			std::wstring path(_wgetenv(L"windir"));
			path.append(LR"(\System32\dwmapi.dll)");
			og_lib = LoadLibraryW(path.c_str());
#if LOGGING
			std::cout << "og_lib = " << (void*)og_lib << std::endl;
#endif
			og_DwmGetCompositionTimingInfo = GetProcAddress(og_lib, ObfusString("DwmGetCompositionTimingInfo"));
		}

		{
			UniquePtr<JsonNode> config = json::decode(string::fromFile(ObfusString("client_config.json").str()));
			if (!config || !config->isObj())
			{
				config = soup::make_unique<JsonObject>();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("server_host")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				server_host = it->second->reinterpretAsStr().value;
			}
			else
			{
				server_host = ObfusString("localhost").str();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("http_port")); it != config->reinterpretAsObj().end() && it->second->isInt())
			{
				http_port = it->second->reinterpretAsInt().value;
			}
			else
			{
				http_port = 80;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("https_port")); it != config->reinterpretAsObj().end() && it->second->isInt())
			{
				https_port = it->second->reinterpretAsInt().value;
			}
			else
			{
				https_port = 443;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_language")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				fallback_language = it->second->reinterpretAsStr().value;
			}
			else
			{
				fallback_language = ObfusString("en").str();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_graphicsDriver")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				fallback_graphicsDriver = it->second->reinterpretAsStr().value;
			}
			else
			{
				fallback_graphicsDriver = ObfusString("dx11").str();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_cluster")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				fallback_cluster = it->second->reinterpretAsStr().value;
			}
			else
			{
				fallback_cluster = ObfusString("public").str();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("high_damage_numbers_patch")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				high_damage_numbers_patch = it->second->reinterpretAsBool().value;
			}
			else
			{
				high_damage_numbers_patch = true;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("skip_mission_start_timer")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				skip_mission_start_timer = it->second->reinterpretAsBool().value;
			}
			else
			{
				skip_mission_start_timer = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fov_override")); it != config->reinterpretAsObj().end())
			{
				if (it->second->isFloat())
				{
					fov_override = it->second->reinterpretAsFloat().value;
				}
				else if (it->second->isInt())
				{
					fov_override = it->second->reinterpretAsInt().value;
				}
				else
				{
					fov_override = 0.0f;
				}
			}
			else
			{
				fov_override = 0.0f;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("enable_http_interface")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				enable_http_interface = it->second->reinterpretAsBool().value;
			}
			else
			{
				enable_http_interface = true;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("disable_nrs_connection")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				disable_nrs_connection = it->second->reinterpretAsBool().value;
			}
			else
			{
				disable_nrs_connection = true;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("autologin")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				autologin = it->second->reinterpretAsBool().value;
			}
			else
			{
				autologin = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("autologin_email")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				autologin_email = it->second->reinterpretAsStr().value;
			}
			else
			{
#if !CONFIG_LOADED_ONLY_ONCE
				autologin_email.clear();
#endif
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("autologin_password")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				autologin_password = it->second->reinterpretAsStr().value;
				if (!autologin_password.empty() && !is_valid_whirlpool_hex_digest(autologin_password))
				{
					whirlpool_ctx ctx;
					unsigned char result[64];
					rhash_whirlpool_init(&ctx);
					rhash_whirlpool_update(&ctx, (const unsigned char*)autologin_password.data(), autologin_password.size());
					rhash_whirlpool_final(&ctx, result);
					autologin_password = string::bin2hexLower((const char*)result, 64);
				}
			}
			else
			{
#if !CONFIG_LOADED_ONLY_ONCE
				autologin_password.clear();
#endif
			}
		}
		save_config();

		/*{
			SIG_INST("48 89 5C 24 18 55 56 57 48 8D AC 24 30 F6 FF FF 48 81 EC D0 0A 00 00");
			auto parse_url = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "parse_url = " << parse_url << std::endl;
#endif
			parse_url_hook.detour = reinterpret_cast<void*>(&parse_url_detour);
			parse_url_hook.target = parse_url;
			parse_url_hook.create();
			parse_url_hook.enable();
		}*/

		{
			SIG_INST("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 68 0C 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 0C 00 00");
			auto winhttp_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "winhttp_connect = " << winhttp_connect << std::endl;
#endif
			if (!winhttp_connect)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			winhttp_connect_hook.detour = reinterpret_cast<void*>(&winhttp_connect_detour);
			winhttp_connect_hook.target = winhttp_connect;
			winhttp_connect_hook.create();
			winhttp_connect_hook.enable();
		}

		{
			SIG_INST("40 55 56 57 41 56 41 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 ? ? ? ? ? 33 ED");
			auto winhttp_new_request = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "winhttp_new_request = " << winhttp_new_request << std::endl;
#endif
			if (winhttp_new_request)
			{
				winhttp_new_request_hook.detour = reinterpret_cast<void*>(&winhttp_new_request_detour);
				winhttp_new_request_hook.target = winhttp_new_request;
				winhttp_new_request_hook.create();
				winhttp_new_request_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

#if PROVIDE_VERSION_INFO
		{
			SIG_INST("4C 8D 05 ? ? ? ? 4C 8B CB 48 8D 0D");
			auto pBuildLabel = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "pBuildLabel = " << pBuildLabel.as<void*>() << std::endl;
#endif
			if (pBuildLabel)
			{
				build_label = pBuildLabel.add(13).rip().as<const char*>();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

		{
			SIG_INST("48 8D 53 18 E8 ? ? ? ? 48 8D 8B");
			auto game_http_request_caller = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "game_http_request_caller = " << game_http_request_caller.as<void*>() << std::endl;
#endif
			if (!game_http_request_caller)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			auto game_http_request = game_http_request_caller.add(5).rip().as<void*>();
			game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour);
			game_http_request_hook.target = game_http_request;
			game_http_request_hook.create();
			game_http_request_hook.enable();
		}

		{
			//SIG_INST("48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 40 48 8B 39");
			SIG_INST("40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC A0 00 00 00 48 8B 05");
			auto Curl_resolv = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "Curl_resolv = " << Curl_resolv << std::endl;
#endif
			if (!Curl_resolv)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			Curl_resolv_hook.detour = reinterpret_cast<void*>(&Curl_resolv_detour);
			Curl_resolv_hook.target = Curl_resolv;
			Curl_resolv_hook.create();
			Curl_resolv_hook.enable();
		}

		{
			SIG_INST("49 8B D4 48 8B CB E8 ? ? ? ? 85 C0 7F");
			auto ssl_verify_internal_caller = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "ssl_verify_internal_caller = " << ssl_verify_internal_caller.as<void*>() << std::endl;
#endif
			if (!ssl_verify_internal_caller)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			auto ssl_verify_internal = ssl_verify_internal_caller.add(7).rip().as<void*>();
			ssl_verify_internal_hook.detour = reinterpret_cast<void*>(&ssl_verify_internal_detour);
			ssl_verify_internal_hook.target = ssl_verify_internal;
			ssl_verify_internal_hook.create();
			ssl_verify_internal_hook.enable();
		}

		{
			//SIG_INST("40 53 55 56 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 78 4C 8B 31");
			SIG_INST("40 53 55 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 8B 05");
			auto Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "Curl_ossl_verifyhost = " << Curl_ossl_verifyhost << std::endl;
#endif
			if (!Curl_ossl_verifyhost)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			Curl_ossl_verifyhost_hook.detour = reinterpret_cast<void*>(&Curl_ossl_verifyhost_detour);
			Curl_ossl_verifyhost_hook.target = Curl_ossl_verifyhost;
			Curl_ossl_verifyhost_hook.create();
			Curl_ossl_verifyhost_hook.enable();
		}

		// This hook allows WorldSeed to be absent or just any value.
		{
			SIG_INST("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 83 EC 70 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 F0 48 8B D9");
			auto verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "verify_worldstate_integrity = " << verify_worldstate_integrity << std::endl;
#endif
			if (!verify_worldstate_integrity)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			verify_worldstate_integrity_hook.detour = reinterpret_cast<void*>(&verify_worldstate_integrity_detour);
			verify_worldstate_integrity_hook.target = verify_worldstate_integrity;
			verify_worldstate_integrity_hook.create();
			verify_worldstate_integrity_hook.enable();
		}

		// This hook allows any WorldSeed be considered valid.
		/*{
			SIG_INST("48 89 5C 24 10 48 89 6C 24 18 56 41 54 41 55 41 56 41 57 48 83 EC 40 48 8B AC 24 A8 00 00 00");
			auto int_rsa_verify = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "int_rsa_verify = " << int_rsa_verify << std::endl;
#endif
			int_rsa_verify_hook.detour = reinterpret_cast<void*>(&int_rsa_verify_detour);
			int_rsa_verify_hook.target = int_rsa_verify;
			int_rsa_verify_hook.create();
			int_rsa_verify_hook.enable();
		}*/

		{
			SIG_INST("4C 8B DC 55 41 57 49 8D 6B A1 48 81 EC ? 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 49 89 5B 20");
			auto parse_arguments = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "parse_arguments = " << parse_arguments << std::endl;
#endif
			if (parse_arguments)
			{
				parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour);
				parse_arguments_hook.target = parse_arguments;
				parse_arguments_hook.create();
				parse_arguments_hook.enable();
			}
			else
			{
				on_got_server_host();
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

#if false
		{
			SIG_INST("48 03 0D ? ? ? ? 48 89 8F");
			auto worldstate_update_interval_insn = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "worldstate_update_interval_insn = " << worldstate_update_interval_insn.as<void*>() << std::endl;
#endif
			if (worldstate_update_interval_insn)
			{
				*worldstate_update_interval_insn.add(3).rip().as<uint64_t*>() = 0; // default: 300
			}
		}
#endif

#if DISABLE_XP_BASED_LEVEL_CAPPING
		{
			SIG_INST("73 43 B2 05");
			auto xp_based_level_jnb = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "xp_based_level_jnb = " << xp_based_level_jnb.as<void*>() << std::endl;
#endif
			if (xp_based_level_jnb)
			{
				memGuard::setAllowedAccess(xp_based_level_jnb.as<void*>(), 1, memGuard::ACC_RWX);
				*xp_based_level_jnb.as<uint8_t*>() = 0xEB; // jnb -> jmp
				disabled_xp_based_level_cap = true;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

		{
			//SIG_INST("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 50 0F 29 74 24 40 0F 57 C0 0F 28 F1");
			SIG_INST("48 89 5C 24 10 57 48 83 EC 50 0F 29 74 24 40 0F 57 C0 0F 28 F1");
			auto SquadSetCountdownTimer = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "SquadSetCountdownTimer = " << SquadSetCountdownTimer << std::endl;
#endif
			if (SquadSetCountdownTimer)
			{
				SquadSetCountdownTimer_hook.detour = reinterpret_cast<void*>(&SquadSetCountdownTimer_detour);
				SquadSetCountdownTimer_hook.target = SquadSetCountdownTimer;
				SquadSetCountdownTimer_hook.create();
				SquadSetCountdownTimer_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? FE FF FF 48 81 EC ? 02 00 00 0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF 44 0F 29 98 68 FF FF FF 44 0F 29 A0 58 FF FF FF 44 0F 29 A8 48 FF FF FF 44 0F 29 B0 38 FF FF FF 44 0F 29 B8 28 FF FF FF");
			auto get_total_damage = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
		std::cout << "get_total_damage = " << get_total_damage << std::endl;
#endif
			if (get_total_damage)
			{
				get_total_damage_hook.detour = reinterpret_cast<void*>(&get_total_damage_detour);
				get_total_damage_hook.target = get_total_damage;
				get_total_damage_hook.create();
				get_total_damage_hook.enable();
			}
		}

		{
			SIG_INST("66 41 0F 6E F4 0F 5B F6 0F 84");
			auto addr = Module(nullptr).range.scan(sig_inst);
			dmg_number_patch_addr = addr.as<void*>();
#if LOGGING
			std::cout << "dmg_number_patch_addr = " << dmg_number_patch_addr << std::endl;
#endif
			if (get_total_damage_hook.target && addr)
			{
				uint8_t detour_bytes[] = {
					// prepare call
					/*  0 */ 0x44, 0x89, 0xE1, // mov ecx, r12d
					/*  3 */ 0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)

					/* 13 */ 0x74, (34 - 15), // if compact numbers are off, jump to the appropriate branch

					// compact numbers on
					/* 15 */ 0x41, 0xFF, 0xD2, // call r10
					/* 18 */ 0x0F, 0x28, 0xF0, // movaps xmm6, xmm0
					/* 21 */ 0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)
					/* 31 */ 0x41, 0xFF, 0xE2, // jmp r10

					// compact numbers off
					/* 34 */ 0x41, 0xFF, 0xD2, // call r10
					/* 37 */ 0x0F, 0x28, 0xF0, // movaps xmm6, xmm0
					/* 40 */ 0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)
					/* 50 */ 0x41, 0xFF, 0xE2, // jmp r10
				};
				static_assert(sizeof(detour_bytes) == 50 + 3);
				*(void**)(detour_bytes + 3 + 2) = reinterpret_cast<void*>(&get_dmg_to_display);
				*(void**)(detour_bytes + 21 + 2) = addr.add(17).as<void*>(); // no jump at jz = compact numbers on -> go to `call log10f`
				*(void**)(detour_bytes + 40 + 2) = addr.add(10).rip().as<void*>(); // jumped at jz = compact numbers off -> go to branch

				void* detour = memGuard::alloc(sizeof(detour_bytes), memGuard::ACC_RWX);
				memcpy(detour, detour_bytes, sizeof(detour_bytes));

				*(void**)(dmg_number_trampoline + 2) = detour;

				memcpy(dmg_number_og_bytes, dmg_number_patch_addr, sizeof(dmg_number_trampoline));

				if (high_damage_numbers_patch)
				{
					enable_dmg_number_patch();
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		// Emulate a non-stripped build so that no H.Cache is needed (breaks dialogue)
		/*{
			SIG_INST("0F B6 44 24 70 40 0F B6 CF 88 05");
			auto insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
			memGuard::setAllowedAccess(insn, 5, memGuard::ACC_RWX);
			insn[0] = 0x31;
			insn[1] = 0xc0;
			insn[2] = 0x90;
			insn[3] = 0x90;
			insn[4] = 0x90;
		}*/

		{
			// Search for string "PostProcessInfo", vftable is below that, function is at offset 0x260
			SIG_INST("F3 0F 10 81 98 08 00 00 C3");
			auto PostProcessInfo_getFov = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "PostProcessInfo_getFov = " << PostProcessInfo_getFov << std::endl;
#endif
			if (PostProcessInfo_getFov)
			{
				PostProcessInfo_getFov_hook.detour = reinterpret_cast<void*>(&PostProcessInfo_getFov_detour);
				PostProcessInfo_getFov_hook.target = PostProcessInfo_getFov;
				PostProcessInfo_getFov_hook.create();
				PostProcessInfo_getFov_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		if (disable_nrs_connection)
		{
			//SIG_INST("0F 85 4A 20 00 00");
			SIG_INST("0F 85 ? ? ? ? 48 89 9C 24 ? ? ? ? 4C 89 BC 24 ? ? ? ? E8");
			auto nrs_jnz = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "nrs_jnz = " << nrs_jnz.as<void*>() << std::endl;
#endif
			if (nrs_jnz)
			{
				memGuard::setAllowedAccess(nrs_jnz.as<void*>(), 2, memGuard::ACC_RWX);
				nrs_jnz.as<uint8_t*>()[0] = 0x90;
				nrs_jnz.as<uint8_t*>()[1] = 0xE9;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

#if PROVIDE_VERSION_INFO
		{
			SIG_INST("4C 8B DC 55 53 41 56 49 8D 6B A8 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4");
			auto ReadCacheManifest = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "ReadCacheManifest = " << ReadCacheManifest << std::endl;
#endif
			if (ReadCacheManifest)
			{
				ReadCacheManifest_hook.detour = reinterpret_cast<void*>(&ReadCacheManifest_detour);
				ReadCacheManifest_hook.target = ReadCacheManifest;
				ReadCacheManifest_hook.create();
				ReadCacheManifest_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

		/*{
			SIG_INST("4C 8B 89 10 03 00 00 48 8B CE 41 FF D1");
			auto get_config_bool = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "get_config_bool = " << get_config_bool.as<void*>() << std::endl;
#endif
			if (get_config_bool)
			{
				if (autologin)
				{
					get_config_bool = get_config_bool.sub(0x0000000140F40B8C - 0x0000000140F40B20);

					get_config_bool_hook.detour = reinterpret_cast<void*>(&get_config_bool_detour);
					get_config_bool_hook.target = get_config_bool.as<void*>();
					get_config_bool_hook.create();
					get_config_bool_hook.enable();
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}*/

		{
			SIG_INST("40 55 56 41 56 48 83 EC 30 41 0F B6 E8");
			auto get_config_bool_vfunc = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "get_config_bool_vfunc = " << get_config_bool_vfunc << std::endl;
#endif
			if (get_config_bool_vfunc)
			{
				if (autologin)
				{
					get_config_bool_vfunc_hook.detour = reinterpret_cast<void*>(&get_config_bool_vfunc_detour);
					get_config_bool_vfunc_hook.target = get_config_bool_vfunc;
					get_config_bool_vfunc_hook.create();
					get_config_bool_vfunc_hook.enable();
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

#if false
		// Enable Steam Login by making the Lua scripts think Steam is initialised
		{
			SIG_INST("FC C6 D4 49");
			auto lua_SteamService_IsInitialized_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_SteamService_IsInitialized_hash = " << lua_SteamService_IsInitialized_hash.as<void*>() << std::endl;
#endif
			if (lua_SteamService_IsInitialized_hash)
			{
				auto lua_SteamService_IsInitialized = *lua_SteamService_IsInitialized_hash.add(8).as<void**>();
#if LOGGING
				std::cout << "lua_SteamService_IsInitialized = " << lua_SteamService_IsInitialized << std::endl;
#endif
				auto SteamService_IsInitialized_call = Pointer(lua_SteamService_IsInitialized).add(0x00000001404F582E - 0x00000001404F5820).as<uint8_t*>();

				if (autologin)
				{
					const uint8_t patch[5] = {
						0xb0, 0x01, // mov al, 1
						0x90, // nop
						0x90, // nop
						0x90, // nop
					};
					memGuard::setAllowedAccess(SteamService_IsInitialized_call, sizeof(patch), memGuard::ACC_RWX);
					memcpy(SteamService_IsInitialized_call, patch, sizeof(patch));
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

		{
			SIG_INST("40 55 53 56 57 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 0F 29 BC 24");
			auto calculate_spawn_position = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "calculate_spawn_position = " << calculate_spawn_position << std::endl;
#endif
			if (calculate_spawn_position)
			{
				calculate_spawn_position_hook.detour = reinterpret_cast<void*>(&calculate_spawn_position_detour);
				calculate_spawn_position_hook.target = calculate_spawn_position;
				calculate_spawn_position_hook.create();
				calculate_spawn_position_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("40 55 53 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? 48 81 EC E0 0E 00 00");
			auto update_hud = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "update_hud = " << update_hud << std::endl;
#endif
			if (update_hud)
			{
				update_hud_hook.detour = reinterpret_cast<void*>(&update_hud_detour);
				update_hud_hook.target = update_hud;
				update_hud_hook.create();
				update_hud_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		if (enable_http_interface)
		{
			Thread thrd([](Capture&&)
			{
				Server serv;
				ServerWebService srv([](soup::Socket& s, soup::HttpRequest&& req, soup::ServerWebService&)
				{
					auto arr = string::explode(req.path, '?');
					switch (soup::joaat::hash(arr.at(0)))
					{
					default:
						ServerWebService::send404(s);
						break;

					case soup::joaat::compileTimeHash("/"):
						{
							std::string html;
#if PRIVATE
							html = string::fromFile("index.html");
							if (html.empty())
#endif
							{
								html = ObfusString(R"EOC(<body style="background:#000;color:#fff;">
	<div id="disconnected" style="display:none">
		<p>Connection to DLL lost. Attempting to reestablish...</p>
		<hr>
	</div>
	<p><label for="server_host">Server Host:</label> <input id="server_host" type="text" /> <button id="server_host_submit">Change</button> <button id="logout">Logout</button></p>
	<p><label for="high_damage_numbers_patch">High Damage Numbers Patch:</label> <input id="high_damage_numbers_patch" type="checkbox" /></p>
	<p><label for="skip_mission_start_timer">Skip Mission Start Timer:</label> <input id="skip_mission_start_timer" type="checkbox" /></p>
	<p><label for="fov_override">FOV Override (0 = disabled):</label> <input id="fov_override" type="range" min="0" value="0" max="2260000" step="10000"></p>
	<button id="save_config">Save changes to client_config.json</button>
	<hr>
	<p><label for="camtype">Camera Type:</label> <select id="camtype"><option value="gamecam">Normal</option><option value="freecam">Freecam</option><option value="lockcam">Locked In Place</option></select></p>
	<p><label for="pos">Position:</label> <input id="pos" type="text" style="width:230px" onclick="this.select()" readonly /></p>
	<p><button id="tp-submit">Teleport To</button> <select id="tp-target"><option>Custom</option></select> <input id="tp-pos" type="text" style="width:230px" onclick="this.select()" /> <span id="tp-status"></span></p>
	<script>
		fetch("/server_host").then(res => res.text()).then(res => {
			document.getElementById("server_host").value = res;
		});
		document.getElementById("server_host_submit").onclick = function() {
			fetch("/server_host?" + document.getElementById("server_host").value);
		};
		document.getElementById("logout").onclick = function() {
			fetch("/logout");
		};

		fetch("/high_damage_numbers_patch").then(res => res.text()).then(res => {
			document.getElementById("high_damage_numbers_patch").checked = (res == "1");
		});
		document.getElementById("high_damage_numbers_patch").onchange = function() {
			fetch("/high_damage_numbers_patch?" + this.checked);
		};

		fetch("/skip_mission_start_timer").then(res => res.text()).then(res => {
			document.getElementById("skip_mission_start_timer").checked = (res == "1");
		});
		document.getElementById("skip_mission_start_timer").onchange = function() {
			fetch("/skip_mission_start_timer?" + this.checked);
		};

		fetch("/fov_override").then(res => res.text()).then(res => {
			document.getElementById("fov_override").value = parseFloat(res) * 10000;
		});
		document.getElementById("fov_override").oninput = function() {
			fetch("/fov_override?" + this.value);
		};

		document.getElementById("save_config").onclick = function() {
			fetch("/save_config");
		};

		document.getElementById("camtype").onchange = function() {
			fetch("/" + this.value);
		};

		const marker_types = {
			"3": "Objective",
			"9": "Target",
			"12": "Life Support",
			"29": "Target",
			"40": "A",
			"41": "B",
			"42": "C",
			"49": "Waypoint",
			"65": "Focus",
			"75": "Extraction",
		};

		function onMarkersChange() {
			document.getElementById("tp-pos").style.display = document.getElementById("tp-target").value == "Custom" ? "" : "none";
		}

		function pollStatus() {
			fetch("/status").then(res => res.json()).then(res => {
				document.getElementById("disconnected").style.display = "none";
				if (res.camtype) {
					document.getElementById("camtype").value = res.camtype;
				}
				document.getElementById("pos").value = res.pos ?? "";
				document.getElementById("tp-status").textContent = res.tping ? "Teleport initated. In-game: Press [Ctrl] to confirm or [Shift] to cancel." : "";

				const marker_set = {};
				for (const marker of res.markers) {
					if (marker.type != 14) {
						const name = (marker.type in marker_types ? marker_types[marker.type] : "Marker") + " in " + marker.dist + "m";
						const pos = marker.x + "," + marker.y + "," + marker.z;
						const slug = marker.type == 49 ? "wp" : pos;
						marker_set[slug] = true;
						let option = document.querySelector("#tp-target [data-slug='"+slug+"']");
						if (!option) {
							option = document.getElementById("tp-target").appendChild(document.createElement("option"));
							option.setAttribute("data-slug", slug);
						}
						if (option.value != pos) {
							option.value = pos;
						}
						if (option.textContent != name) {
							option.textContent = name;
						}
					}
				}
				for (const child of document.getElementById("tp-target").children) {
					if (child.value != "Custom" && !(child.getAttribute("data-slug") in marker_set)) {
						document.getElementById("tp-target").removeChild(child);
						onMarkersChange();
					}
				}
			}).catch((e) => {
				console.error(e);
				document.getElementById("disconnected").style.display = "";
			}).finally(pollStatus);
		}
		pollStatus();

		document.getElementById("tp-target").onchange = onMarkersChange;

		document.getElementById("tp-submit").onclick = function() {
			let pos = document.getElementById("tp-target").value;
			if (pos == "Custom") {
				pos = document.getElementById("tp-pos").value
			}
			fetch("/teleport?" + pos);
		};
	</script>
</body>)EOC").str();
							}
							ServerWebService::sendHtml(s, html);
						}
						break;

					case soup::joaat::compileTimeHash("/ping"):
						ServerWebService::sendText(s, ObfusString("pong"));
						break;

					case soup::joaat::compileTimeHash("/save_config"):
						save_config();
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/skip_mission_start_timer"):
						if (arr.size() > 1)
						{
							skip_mission_start_timer = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(skip_mission_start_timer));
						break;

					case soup::joaat::compileTimeHash("/fov_override"):
						if (arr.size() > 1)
						{
							fov_override = static_cast<float>(string::toIntOpt<int64_t>(arr[1]).value()) / 10000.0f;
						}
						ServerWebService::sendText(s, std::to_string(fov_override));
						break;

					case soup::joaat::compileTimeHash("/high_damage_numbers_patch"):
						if (arr.size() > 1)
						{
							high_damage_numbers_patch = (arr[1].size() == 4);
							if (high_damage_numbers_patch)
							{
								enable_dmg_number_patch();
							}
							else
							{
								disable_dmg_number_patch();
							}
						}
						ServerWebService::sendText(s, std::to_string(high_damage_numbers_patch));
						break;

					case soup::joaat::compileTimeHash("/logout"):
						do_logout();
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/server_host"):
						if (arr.size() > 1
							&& server_host != arr[1]
							)
						{
							do_logout();
							server_host = arr[1];
							if (!console_attached)
							{
								attach_console();
							}
							on_got_server_host();
						}
						ServerWebService::sendText(s, server_host);
						break;

					case soup::joaat::compileTimeHash("/freecam"):
						if (local_player && !prohibit_freecam)
						{
							local_player->controlling_camera = true;
							local_player->getAvatar()->followed_by_camera = false;
						}
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/lockcam"):
						if (local_player && !prohibit_freecam)
						{
							local_player->controlling_camera = false;
							local_player->getAvatar()->followed_by_camera = false;
						}
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/gamecam"):
						if (local_player && !prohibit_freecam)
						{
							local_player->controlling_camera = false;
							local_player->getAvatar()->followed_by_camera = true;
						}
						ServerWebService::send204(s);
						break;

						// Vania Mall: Closet behind Arthur: -15,-6.5,13
						// Vania Mall: Cutscene Room: -19,-6.5,14
					case soup::joaat::compileTimeHash("/teleport"):
						if (local_player && !prohibit_teleport)
						{
							std::vector<std::string> pos_arr;
							if (arr.size() > 1)
							{
								pos_arr = string::explode(arr[1], ',');
							}
							if (pos_arr.size() == 3)
							{
								teleport(
									strtof(pos_arr[0].c_str(), nullptr),
									strtof(pos_arr[1].c_str(), nullptr),
									strtof(pos_arr[2].c_str(), nullptr)
								);
								ServerWebService::send204(s);
							}
							else
							{
								ServerWebService::send400(s);
							}
						}
						else
						{
							ServerWebService::send500(s);
						}
						break;

					case soup::joaat::compileTimeHash("/status"):
						{
							JsonObject obj;
							if (local_player)
							{
								std::string camtype = ObfusString("gamecam").str();
								std::string pos_str;
								__try
								{
									if (auto avatar = local_player->getAvatar())
									{
										if (!avatar->followed_by_camera)
										{
											camtype = local_player->controlling_camera ? ObfusString("freecam").str() : ObfusString("lockcam").str();
										}
										pos_str = std::to_string(avatar->pos_x);
										pos_str.push_back(',');
										pos_str.append(std::to_string(avatar->pos_y));
										pos_str.push_back(',');
										pos_str.append(std::to_string(avatar->pos_z));
									}
								}
								__except (EXCEPTION_EXECUTE_HANDLER)
								{
#if LOGGING
									std::cout << "Exception while reading from player or avatar" << std::endl;
									local_player = nullptr; // Could maybe do better by zeroing this when it says "Clearing gRegion [...]"
#endif
								}
								obj.add(ObfusString("camtype"), std::move(camtype));
								obj.add(ObfusString("pos"), std::move(pos_str));
							}
							obj.add(ObfusString("tping"), tp_thrd.isRunning());
							auto arr = soup::make_unique<JsonArray>();
							if (markers)
							{
								Marker* marker = nullptr;
								__try
								{
									marker = markers->getHead();
								}
								__except (EXCEPTION_EXECUTE_HANDLER)
								{
#if LOGGING
									std::cout << "Exception while fetching marker head" << std::endl;
#endif
									markers = nullptr;
								}
								while (marker != nullptr)
								{
									auto marker_obj = soup::make_unique<JsonObject>();
									__try
									{
										marker_obj->add(ObfusString("type").str(), (int)marker->type);
										marker_obj->add(ObfusString("x").str(), marker->world_x);
										marker_obj->add(ObfusString("y").str(), marker->world_y);
										marker_obj->add(ObfusString("z").str(), marker->world_z);
										marker_obj->add(ObfusString("dist").str(), marker->distance);
										marker = marker->getNext();
									}
									__except (EXCEPTION_EXECUTE_HANDLER)
									{
#if LOGGING
										std::cout << "Exception while reading marker" << std::endl;
#endif
										markers = nullptr;
									}
									arr->children.emplace_back(std::move(marker_obj));
								}
							}
							obj.add(ObfusString("markers"), std::move(arr));
							ServerWebService::sendText(s, obj.encodePretty());
						}
						break;
					}
				});
				if (serv.bind(61558, &srv))
				{
					serv.run();
				}
			});
			thrd.detach();
		}
	}
	return TRUE;
}
