#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.7.3"

#define LOGGING false
#define PRIVATE false

#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true
#define PROVIDE_VERSION_INFO true

#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>

#include <CompactDetourHook.hpp>
#include <DetourHook.hpp>
#include <HttpRequest.hpp>
#include <joaat.hpp>
#include <json.hpp>
#include <memGuard.hpp>
#include <Module.hpp>
#include <Mutex.hpp>
#include <netConfig.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <pattern_macros.hpp>
#include <Process.hpp>
#include <ReplacementHook.hpp>
#include <Server.hpp>
#include <ServerWebService.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <structing.hpp>
#include <Thread.hpp>
#include <Uri.hpp>
#include <urlenc.hpp>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <lstate.h>

#include "whirlpool.hpp"

using namespace soup;

#include "owf_config.hpp"
#include "owf_console.hpp"
#include "owf_luau.hpp"
#include "owf_overlay.hpp"

static bool disabled_xp_based_level_cap = false;
#if PROVIDE_VERSION_INFO
static const char* build_label = nullptr; // e.g. "2024.12.14.10.37 Retail Windows x64"
static std::string build_hash;
#endif
static bool fallback_language_was_used = false;
static bool fallback_graphicsDriver_was_used = false;
static bool did_auto_login = false;
static std::string auth_query; // e.g. "accountId=6633b81e9dba0b714f28ff02&nonce=8300464181160923&ct=MSI"

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
	PAD(0x1A0, 0x1AC) bool got_language;
	/* 0x1B0 */ GameString language;
	/* 0x1C0 */ bool got_cluster;
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
		if (owfConsole::active)
		{
			owfConsole::deactivate();
		}
#endif
		owfOverlay::setPrelogin(false);
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
		if (build_label && !build_hash.empty())
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
		if (build_label && !build_hash.empty())
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


static ReplacementHook ssl_verify_internal_hook;

static int64_t ssl_verify_internal_detour(void* a1, void* a2)
{
	//std::cout << "ssl_verify_internal called" << std::endl;
	return 1; // "Verify success"
}


static ReplacementHook Curl_ossl_verifyhost_hook;

static int64_t Curl_ossl_verifyhost_detour(void* a1, void* a2)
{
	//auto ret = reinterpret_cast<decltype(&Curl_ossl_verifyhost_detour)>(Curl_ossl_verifyhost_hook.original)(a1, a2);
	//std::cout << "Curl_ossl_verifyhost returned " << ret << std::endl;
	return 0;
}


static ReplacementHook verify_worldstate_integrity_hook;

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
static bool prohibit_scripts = false;

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
		prohibit_scripts = jr && jr->isObj() && jr->reinterpretAsObj().contains(ObfusString("prohibit_scripts").str());

		if (prohibit_skip_mission_start_timer)
		{
			std::cout << ObfusString("Note: Skip Mission Start Timer is prohibited on this server.") << std::endl;
		}
		if (prohibit_fov_override)
		{
			std::cout << ObfusString("Note: FOV Override is prohibited on this server.") << std::endl;
		}
		if (prohibit_freecam)
		{
			std::cout << ObfusString("Note: Freecam is prohibited on this server.") << std::endl;
		}
		if (prohibit_teleport)
		{
			std::cout << ObfusString("Note: Teleport is prohibited on this server.") << std::endl;
		}
		if (prohibit_scripts)
		{
			std::cout << ObfusString("Note: Scripts are prohibited on this server.") << std::endl;
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


static ReplacementHook PostProcessInfo_getFov_hook;

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
	//std::cout << "get_dmg_to_display: " << dmg_int << " -> " << dmg_number << std::endl;
	if ((dmg_number - last_dmg) > 1.0f)
	{
		std::cout << "DAMAGE INCONGRUENCE: Converting " << dmg_int << " to " << dmg_number << ", last damage was " << last_dmg << std::endl;
	}
#endif
	return dmg_number;
}

static DetourHook get_total_damage_hook;

static float get_total_damage_detour(__int64 *a1, __int64 a2, float a3, unsigned __int8 a4, float *a5, float *a6)
{
	float ret = reinterpret_cast<decltype(&get_total_damage_detour)>(get_total_damage_hook.original)(a1, a2, a3, a4, a5, a6);
#if LOGGING
	//std::cout << "get_total_damage: " << ret << std::endl;
#endif
	if (ret != 0.0f)
	{
		last_dmg = ret;
	}
	//ret = FLT_MAX;
	return ret;
}


#if PROVIDE_VERSION_INFO && false
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


static DetourHook write_to_log_file_hook;
static ObfusString log_sep("]: ");
static std::string active_input_filter;

static void write_to_log_file_detour(void* a1, const char* data, size_t size)
{
	reinterpret_cast<decltype(&write_to_log_file_detour)>(write_to_log_file_hook.original)(a1, data, size);
	//std::cout << std::string(data, size);
	SOUP_IF_LIKELY (size > 15)
	{
		SOUP_IF_LIKELY (auto message = strstr(data + 15, log_sep.c_str()))
		{
			message += log_sep.size();
			size -= (message - data);

			if (size > 20)
			{
				switch (soup::joaat::hashRange(message, 20))
				{
#if PROVIDE_VERSION_INFO
				case soup::joaat::compileTimeHash("Cache manifest hash "):
					if (size == 43)
					{
						build_hash = std::string(message + 20, 22);
					}
					break;
#endif

				case soup::joaat::compileTimeHash("InitMapping for all "): // "InitMapping for all devices with bindings ... and filter ..."
					if (size > 42)
					{
						ObfusString sep(" and filter ");
						if (auto filter = strstr(message + 42, sep.c_str()))
						{
							filter += 12;
							size -= (filter - message);
							size -= 1; // '\n'
							active_input_filter = std::string(filter, size);
						}
					}
					break;
				}
			}
		}
	}
}


/*static DetourHook get_config_bool_hook;

static int get_config_bool_detour(luau_State* L)
{
	SOUP_IF_LIKELY (L->intop[1].type == LUAU_STRING)
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
		ObfusString str("Steam.AutoLogin");
		SOUP_IF_UNLIKELY (strcmp(name, str.c_str()) == 0)
		{
			if (!did_auto_login)
			{
#if LOGGING
				std::cout << "Reporting Steam.AutoLogin as true" << std::endl;
#endif
				return true;
			}
		}
	}

	return reinterpret_cast<decltype(&get_config_bool_vfunc_detour)>(get_config_bool_vfunc_hook.original)(a1, name, fallback);
}


static void* lua_SteamService_IsInitialized_og;

static int lua_SteamService_IsInitialized_detour(luau_State* L)
{
	if (!did_auto_login)
	{
#if LOGGING
		//std::cout << "Making lua_SteamService_IsInitialized return true" << std::endl;
#endif
		L->outtop->value.as_bool = true;
		L->outtop->type = LUAU_BOOL;
		L->outtop++;
		return 1;
	}
	return reinterpret_cast<decltype(&lua_SteamService_IsInitialized_detour)>(lua_SteamService_IsInitialized_og)(L);
}


struct ObjectType
{
	PAD(0, 0x2C) uint32_t unk_name_hash; // 1454702781 for LotusDangerRoomGameRules
};

struct Object
{
	/* 0x00 */ void* vftable;
	/* 0x08 */ ObjectType* type;
	/* 0x10 */ Object** self_pointer;
	PAD(0x18, 0x20);
};

struct WeaponEx : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x970) Object*(*GetActiveImpactBehavior)(WeaponEx*, void*);
	};

	INIT_PAD(Object, 0x8A0) void* unk_impact_behavior_data;

	Object* GetActiveImpactBehavior() { return reinterpret_cast<Vftable*>(vftable)->GetActiveImpactBehavior(this, unk_impact_behavior_data); }
};

struct LotusInventoryController : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x250) void(*RemoveItem)(LotusInventoryController*, uint8_t slot, bool); // from BaseInventoryController
		PAD(0x258, 0x2C8) Object*(*GetWeaponInHand)(LotusInventoryController*, uint32_t hand); // from BaseInventoryController
		PAD(0x2D0, 0x8E0) Object*(*GetActivePowerSuit)(LotusInventoryController*);
	};

	void RemoveItem(uint8_t slot, bool b) { return reinterpret_cast<Vftable*>(vftable)->RemoveItem(this, slot, b); }
	Object* GetWeaponInHand(uint32_t hand) { return reinterpret_cast<Vftable*>(vftable)->GetWeaponInHand(this, hand); }
	Object* GetActivePowerSuit() { return reinterpret_cast<Vftable*>(vftable)->GetActivePowerSuit(this); }
};

struct BaseEntity : public Object
{
};

struct Entity : public BaseEntity
{
	INIT_PAD(BaseEntity, 0x48) float mov_dir_x;
	/* 0x4C */ float mov_dir_y;
	/* 0x50 */ float mov_dir_z;
	PAD(0x54, 0x70) float pos_x;
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
};

using Entity_SetPosition_t = void(*)(Entity*, float[3]);
static Entity_SetPosition_t Entity_SetPosition = nullptr;

struct UnkControlsArg
{
};

struct BaseAvatar : public Entity
{
	struct Vftable
	{
		PAD(0, 0x610) void(*disableJumping)(BaseAvatar*, UnkControlsArg*);
		/* 0x618 */ void(*enableJumping)(BaseAvatar*, UnkControlsArg*);
		PAD(0x620, 0x8C8) Object*(*getDamageController)(BaseAvatar*);
		PAD(0x8D0, 0x8E8) Object*(*getInputController)(BaseAvatar*);
		PAD(0x8F0, 0x8F8) LotusInventoryController*(*getInventoryController)(BaseAvatar*);
		PAD(0x900, 0xC40) void(*Suicide)(BaseAvatar*);
	};
	static_assert(sizeof(Vftable) == 0xC40 + 8);

	Object* getDamageController() { return reinterpret_cast<Vftable*>(vftable)->getDamageController(this); }
	LotusInventoryController* getInventoryController() { return reinterpret_cast<Vftable*>(vftable)->getInventoryController(this); }
};

struct Avatar : public BaseAvatar
{
	INIT_PAD(BaseAvatar, 0x500) float head_pos_x;
	/* 0x504 */ float head_pos_y;
	/* 0x508 */ float head_pos_z;
	PAD(0x50C, 0x511) bool followed_by_camera;
	PAD(0x512, 0x679) uint8_t movement_flags; // 2 = sprinting, 4 = crouching, 5 = sliding
	PAD(0x67A, 0x6A0) bool render_above_everything;
};
static_assert(offsetof(Avatar, followed_by_camera) == 0x511);

struct LotusAvatar : public Avatar
{
	INIT_PAD(Avatar, 0x6D8) bool relationship_group; // if equal between two avatars, they are friendlies (IsAvatarFriendly; ee0bc178)
};

struct Player : public Object
{
	PAD(0x020, 0x038) GameString name;
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

struct Camera : public Entity
{
};

struct LotusGameRules : public Object
{
};

// Most of what we access here is actually on RegionMgrImpl
struct RegionMgr : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x3B0) Camera*(*GetGameCamera)(RegionMgr*);
		PAD(0x3B8, 0x3F8) Player*(*GetLocalPlayer)(RegionMgr*);
		/* 0x400 */ Avatar*(*GetLocalPlayerAvatar)(RegionMgr*);
	};

	Camera* GetGameCamera() { return reinterpret_cast<Vftable*>(vftable)->GetGameCamera(this); }
	Player* GetLocalPlayer() { return reinterpret_cast<Vftable*>(vftable)->GetLocalPlayer(this); }
	Avatar* GetLocalPlayerAvatar() { return reinterpret_cast<Vftable*>(vftable)->GetLocalPlayerAvatar(this); }

	INIT_PAD(Object, 0x208) Player*** local_player;
	PAD(0x210, 0x218) LotusGameRules** game_rules;
	PAD(0x220, 0x2C8) Camera** game_camera;
};
static_assert(sizeof(RegionMgr) == 0x2C8 + 8);

static DetourHook set_lua_global_hook;
static RegionMgr* regionmgr = nullptr;
//static LotusGameRules* gamerules;
static Object* flashmgr = nullptr;

static void* set_lua_global_detour(void* a1, Object*** a2, const char* name)
{
	if (a2 && *a2)
	{
#if LOGGING
		std::cout << "set_lua_global: " << name << " = " << **a2 << std::endl;
#endif

		switch (soup::joaat::hash(name))
		{
		case soup::joaat::compileTimeHash("gRegion"):
			regionmgr = static_cast<RegionMgr*>(**a2);
			if (!owfOverlay::isInited())
			{
				owfOverlay::init();
			}
			break;

		/*case soup::joaat::compileTimeHash("gGameRules"):
			gamerules = static_cast<LotusGameRules*>(**a2);
			break;*/

		case soup::joaat::compileTimeHash("gFlashMgr"):
			flashmgr = **a2;
			break;
		}
	}
	return reinterpret_cast<decltype(&set_lua_global_detour)>(set_lua_global_hook.original)(a1, a2, name);
}


/*using luau_newstate_t = luau_State*(*)(luau_Alloc f, void* ud, char);
static luau_newstate_t luau_newstate = nullptr;*/

using luau_pushstring_t = const char*(*)(luau_State*, const char*);
static luau_pushstring_t luau_pushstring = nullptr;

using luau_pushpointer_t = void*(*)(luau_State*, void*);
static luau_pushpointer_t luau_pushpointer = nullptr;

using luau_pushobject_t = Object*(*)(luau_State*, Object*);
static luau_pushobject_t luau_pushobject = nullptr;

using luau_gettable_t = int(*)(luau_State*, int idx);
static luau_gettable_t luau_gettable = nullptr;

using luauD_call_t = int(*)(luau_State* L, luau_TValue* func, int nresults);
static luauD_call_t luauD_call = nullptr;

static luau_State* luau_L = nullptr;
//static Object*** luau_obj_buf[4];
static std::string luau_error_msg;

struct SwigMethod
{
	uint32_t hash;
	luau_CFunction func;
};
static_assert(sizeof(SwigMethod) == 0x10);

struct SwigAttribute
{
	uint32_t hash;
	luau_CFunction getter;
	luau_CFunction setter;
};
static_assert(sizeof(SwigAttribute) == 0x18);

struct SwigTypeDesc
{
	/* 0x00 */ const char* name; // e.g. "Object"
	PAD(0x08, 0x10) luau_CFunction ctor;
	PAD(0x18, 0x20) SwigMethod* methods;
	/* 0x28 */ SwigAttribute* attributes;
	PAD(0x30, 0x38) const char** parent_ptr_name; // e.g. "Object *"

	luau_CFunction findMethod(uint32_t hash)
	{
		for (auto method = this->methods; method->hash != 0; ++method)
		{
			if (method->hash == hash)
			{
				return method->func;
			}
		}
		return nullptr;
	}

	luau_CFunction findGetter(uint32_t hash)
	{
		for (auto attr = this->attributes; attr->hash != 0; ++attr)
		{
			if (attr->hash == hash)
			{
				return attr->getter;
			}
		}
		return nullptr;
	}

	luau_CFunction findSetter(uint32_t hash)
	{
		for (auto attr = this->attributes; attr->hash != 0; ++attr)
		{
			if (attr->hash == hash)
			{
				return attr->setter;
			}
		}
		return nullptr;
	}
};
static_assert(sizeof(SwigTypeDesc) == 0x40);

struct SwigTypeField
{
	/* 0x00 */ const char* field_name; // e.g. "_p_Object"
	/* 0x08 */ const char* type_name; // e.g. "Object *"
	PAD(0x10, 0x18) SwigTypeDesc* type_desc;
};
static_assert(sizeof(SwigTypeField) == 0x20);

static std::unordered_map<uint32_t, SwigTypeDesc*> swig_types;

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

static Mutex script_log_mtx;
static std::string script_log;

static uintptr_t ChatRedux_table = 0;
static uintptr_t ChatRedux_SystemMessage_method = 0;

struct owfScript
{
	std::string name;
	lua_State* main;
	lua_State* coro = nullptr;
	bool stop_requested = false;

	std::unordered_set<std::string> blocked_chat_prefixes;
	std::deque<std::string> blocked_chat_messages;

	static void logNl(const std::string& msg)
	{
		std::cout << msg << std::endl;
		std::lock_guard lock(script_log_mtx);
		script_log.append(msg).push_back('\n');
	}

	static void log(const std::string& msg)
	{
		std::cout << msg;
		std::lock_guard lock(script_log_mtx);
		script_log.append(msg);
	}

	owfScript()
	{
		auto L = luaL_newstate();
		this->main = L;
		L->l_G->user_data = this;
		luaL_openlibs(L);

		lua_pushcfunction(L, [](lua_State* L) -> int
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
			owfScript::logNl(msg);
			return 0;
		});
		{ ObfusString name("print"); lua_setglobal(L, name.c_str()); }

		{ ObfusString name("io"); lua_getglobal(L, name.c_str()); }
		{ ObfusString name("write"); lua_pushlstring(L, name.data(), name.size()); }
		lua_pushcfunction(L, [](lua_State* L) -> int
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
			owfScript::log(msg);
			return 0;
		});
		lua_settable(L, -3);

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

		if (Entity_SetPosition)
		{
			lua_pushcfunction(L, [](lua_State* L) -> int
			{
				auto entity = reinterpret_cast<Entity*>(luaL_checkinteger(L, 1));
				float pos[3];
				pos[0] = static_cast<float>(luaL_checknumber(L, 2));
				pos[1] = static_cast<float>(luaL_checknumber(L, 3));
				pos[2] = static_cast<float>(luaL_checknumber(L, 4));
				Entity_SetPosition(entity, pos);
				return 3;
			});
			{ ObfusString name("entity_set_pos"); lua_setglobal(L, name.c_str()); }
		}

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
			reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera = lua_toboolean(L, 2);
			return 0;
		});
		{ ObfusString name("avatar_set_followed_by_camera"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushboolean(L, reinterpret_cast<Avatar*>(luaL_checkinteger(L, 1))->followed_by_camera);
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
			lua_pushinteger(L, Module(nullptr).range.scan(Pattern(str, len)).as<uintptr_t>());
			return 1;
		});
		{ ObfusString name("mem_scan_exe"); lua_setglobal(L, name.c_str()); }

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
			luau_L->intop = luau_L->outtop;
			return 0;
		});
		{ ObfusString name("luau_stktrk_begin"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushinteger(L, luau_L->outtop - luau_L->intop);
			return 1;
		});
		{ ObfusString name("luau_stktrk_end"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->type = LUAU_NIL;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_nil"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->value.as_bool = lua_toboolean(L, 1);
			luau_L->outtop->type = LUAU_BOOL;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_bool"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->value.as_float = static_cast<float>(luaL_checkinteger(L, 1));
			luau_L->outtop->type = LUAU_NUMBER;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_int"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->value.as_float = static_cast<float>(luaL_checknumber(L, 1));
			luau_L->outtop->type = LUAU_NUMBER;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_float"); lua_setglobal(L, name.c_str()); }

		if (luau_pushstring)
		{
			lua_pushcfunction(L, [](lua_State* L) -> int
			{
				const char* str = luaL_checkstring(L, 1);
				luau_pushstring(luau_L, str);
				return 0;
			});
			{ ObfusString name("luau_push_string"); lua_setglobal(L, name.c_str()); }
		}

		if (luau_pushpointer)
		{
			lua_pushcfunction(L, [](lua_State* L) -> int
			{
				luau_pushpointer(luau_L, reinterpret_cast<void*>(luaL_checkinteger(L, 1)));
				return 0;
			});
			{ ObfusString name("luau_push_pointer"); lua_setglobal(L, name.c_str()); }
		}

		if (luau_pushobject)
		{
			lua_pushcfunction(L, [](lua_State* L) -> int
			{
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
			{ ObfusString name("luau_push_object"); lua_setglobal(L, name.c_str()); }
		}

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->value.as_uintptr = luaL_checkinteger(L, 1);
			luau_L->outtop->type = LUAU_USERDATA;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_userdata"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop->value.as_uintptr = luaL_checkinteger(L, 1);
			luau_L->outtop->type = LUAU_LIGHTUSERDATA;
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_lightuserdata"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			*luau_L->outtop = *luau_L->getValue(luaL_checkinteger(L, 1));
			luau_L->outtop++;
			return 0;
		});
		{ ObfusString name("luau_push_value"); lua_setglobal(L, name.c_str()); }

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
		{ ObfusString name("luau_call"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			luau_L->outtop -= luaL_optinteger(L, 1, 1);
			return 0;
		});
		{ ObfusString name("luau_pop"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_BOOL)
			{
				lua_pushboolean(L, (--luau_L->outtop)->value.as_bool);
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_bool"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_NUMBER)
			{
				lua_pushnumber(L, (--luau_L->outtop)->value.as_float);
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_number"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_STRING)
			{
				lua_pushstring(L, (--luau_L->outtop)->getString());
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_string"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_USERDATA)
			{
				lua_pushinteger(L, (--luau_L->outtop)->value.as_uintptr);
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_userdata"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_USERDATA)
			{
				lua_pushinteger(L, luau_L->outtop[-1].value.as_uintptr);
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_get_userdata"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_USERDATA)
			{
				lua_pushpointer(L, *(void**)((--luau_L->outtop)->value.as_uintptr + 0x18));
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_pointer"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			if (luau_L->outtop[-1].type == LUAU_USERDATA)
			{
				lua_pushpointer(L, ***(void****)((--luau_L->outtop)->value.as_uintptr + 0x18));
				return 1;
			}
			return 0;
		});
		{ ObfusString name("luau_pop_object"); lua_setglobal(L, name.c_str()); }

		if (luau_gettable)
		{
			lua_pushcfunction(L, [](lua_State* L) -> int
			{
				luau_gettable(luau_L, luaL_checkinteger(L, 1));
				return 0;
			});
			{ ObfusString name("luau_gettable"); lua_setglobal(L, name.c_str()); }
		}

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
		{ ObfusString name("luau_find_ctor"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			void* res = nullptr;
			if (auto e = swig_types.find(soup::joaat::hash(luaL_checkstring(L, 1))); e != swig_types.end())
			{
				res = reinterpret_cast<void*>(e->second->findMethod(wf_fnv_32(luaL_checkstring(L, 2))));
			}
			lua_pushpointer(L, res);
			return 1;
		});
		{ ObfusString name("luau_find_method"); lua_setglobal(L, name.c_str()); }

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
		{ ObfusString name("luau_find_getter"); lua_setglobal(L, name.c_str()); }

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
		{ ObfusString name("luau_find_setter"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushinteger(L, owfOverlay::addRect(
				luaL_checkinteger(L, 1),
				luaL_checkinteger(L, 2),
				luaL_checkinteger(L, 3),
				luaL_checkinteger(L, 4),
				luaL_checkinteger(L, 5),
				luaL_checkinteger(L, 6),
				luaL_checkinteger(L, 7)
			));
			return 1;
		});
		{ ObfusString name("owf_overlay_add_rect"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			lua_pushinteger(L, owfOverlay::addText(
				luaL_checkinteger(L, 1),
				luaL_checkinteger(L, 2),
				pluto_checkstring(L, 3),
				luaL_checkinteger(L, 4) == 5 ? &RasterFont::simple5() : &RasterFont::simple8(),
				luaL_checkinteger(L, 5),
				luaL_checkinteger(L, 6),
				luaL_checkinteger(L, 7),
				luaL_optinteger(L, 8, 1)
			));
			return 1;
		});
		{ ObfusString name("owf_overlay_add_text"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			owfOverlay::remove(luaL_checkinteger(L, 1));
			return 0;
		});
		{ ObfusString name("owf_overlay_remove"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			owfOverlay::redraw();
			return 0;
		});
		{ ObfusString name("owf_overlay_update"); lua_setglobal(L, name.c_str()); }

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
		}

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			auto scr = static_cast<owfScript*>(L->l_G->user_data);
			if (!scr->blocked_chat_messages.empty())
			{
				lua_newtable(L);
				{
					pluto_pushstring(L, ObfusString("type").str());
					lua_pushinteger(L, 1); // OWF_EVT_BLOCKED_CHAT_MESSAGE
					lua_settable(L, -3);
				}
				{
					pluto_pushstring(L, ObfusString("text").str());
					pluto_pushstring(L, scr->blocked_chat_messages.front());
					lua_settable(L, -3);
				}
				scr->blocked_chat_messages.pop_front();
				return 1;
			}
			return 0;
		});
		{ ObfusString name("owf_next_event"); lua_setglobal(L, name.c_str()); }

		lua_pushcfunction(L, [](lua_State* L) -> int
		{
			pluto_pushstring(L, active_input_filter);
			return 1;
		});
		{ ObfusString name("get_active_input_filter"); lua_setglobal(L, name.c_str()); }

		std::string runtime;
#if PRIVATE
		runtime = string::fromFile(R"(C:\Users\Sainan\Desktop\Repos\warframe-dll\runtime.pluto)");
		if (runtime.empty())
#endif
		{
			using namespace soup::literals;
			int dummy;
			runtime = (
				#include "runtime.pluto"
			).str();
		}
		if (luaL_loadbuffer(L, runtime.data(), runtime.size(), runtime_script_name.c_str()) != LUA_OK
			|| lua_pcall(L, 0, 1, 0) != LUA_OK
			)
		{
			owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error while loading runtime").str());
		}
	}

	bool loadFile(std::string&& path)
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

	bool loadString(std::string&& code)
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

	bool tick()
	{
		if (!prohibit_scripts)
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
		}
		return false;
	}

	~owfScript()
	{
		lua_close(main);
	}
};

static Mutex running_scripts_mtx;
static std::vector<UniquePtr<owfScript>> running_scripts;

static void start_script_from_file(std::string&& path)
{
	auto scr = soup::make_unique<owfScript>();
	if (scr->loadFile(std::move(path)))
	{
		std::lock_guard lock(running_scripts_mtx);
		running_scripts.emplace_back(std::move(scr));
	}
}

static void start_script_from_string(std::string&& code)
{
	auto scr = soup::make_unique<owfScript>();
	if (scr->loadString(std::move(code)))
	{
		std::lock_guard lock(running_scripts_mtx);
		running_scripts.emplace_back(std::move(scr));
	}
}

static owfScript* get_script_by_name(const std::string& name)
{
	for (const auto& scr : running_scripts)
	{
		if (scr->name == name)
		{
			return scr.get();
		}
	}
	return nullptr;
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
	HUD_LIFE_SUPPORT_CAPSULE = 12,
	HUD_ELEVATOR = 14, // Typically only shows when nearby (without distance indicator)
	HUD_TARGET2 = 29, // Capture Target, Disruption Demolyst
	HUD_LIFE_SUPPORT_PICKUP = 31,
	HUD_SPY_A = 40,
	HUD_SPY_B = 41,
	HUD_SPY_C = 42,
	HUD_WAYPOINT_1 = 49,
	HUD_FOCUS = 65,
	HUD_EXTRACT = 75,
	HUD_DISRUPTION = 79, // All keys & conduits seem to use this
	HUD_REINFORCEMENT_BEACON = 86, // Orb Vallis
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

struct LotusHudStatus : public Object
{
	INIT_PAD(Object, 0xB80) LinkedList<Marker>** markers;
};

static DetourHook lua_update_hud_hook;
static int lua_update_hud_detour(luau_State* L)
{
	const auto og_outtop = L->outtop;
	const auto og_intop = L->intop;
	const auto og_lngjmp = L->global_state->error_longjump_data;
	const auto og_panic = L->global_state->panic_func;

	luau_L = L;
	L->global_state->error_longjump_data = nullptr;
	L->global_state->panic_func = [](luau_State* L, int)
	{
#if LOGGING
		std::cout << "LuaU is panicking" << std::endl;
#endif
		luau_error_msg = (--L->outtop)->getString();
		throw 0;
	};
	{
		std::lock_guard mtx(running_scripts_mtx);
		for (auto i = running_scripts.begin(); i != running_scripts.end(); )
		{
			if ((*i)->tick())
			{
				++i;
			}
			else
			{
				i = running_scripts.erase(i);
			}
		}
	}

#if LOGGING
	if (L->outtop != og_outtop)
	{
		std::cout << "Not all values were popped from LuaU stack" << std::endl;
	}
#endif
	L->outtop = og_outtop;
	L->intop = og_intop;
	L->global_state->error_longjump_data = og_lngjmp;
	L->global_state->panic_func = og_panic;

	return reinterpret_cast<decltype(&lua_update_hud_detour)>(lua_update_hud_hook.original)(L);
}

static DetourHook update_hud_hook;
static LinkedList<Marker>* markers = nullptr;

static bool update_hud_detour(LotusHudStatus* hud, void* a2, void* a3, float a4)
{
	markers = *hud->markers;
	return reinterpret_cast<decltype(&update_hud_detour)>(update_hud_hook.original)(hud, a2, a3, a4);
}


static ReplacementHook get_profile_dir_hook;

static GameString* get_profile_dir_detour(uintptr_t a1)
{
	auto str = reinterpret_cast<GameString*>(a1 + 0x2A0);
	str->setUnownedData(forced_profile_dir.data(), forced_profile_dir.size());
	return str;
}


static CompactDetourHook lua_AvatarEntry_excludedFromSimulacrum_get_hook;

static int lua_AvatarEntry_excludedFromSimulacrum_get_detour(luau_State* L)
{
	reinterpret_cast<luau_CFunction>(lua_AvatarEntry_excludedFromSimulacrum_get_hook.original)(L);
	//std::cout << "lua_AvatarEntry_excludedFromSimulacrum_get: " << L->outtop[-1].value.as_bool << std::endl;
	L->outtop[-1].value.as_bool = L->outtop[-1].value.as_bool ? !simulacrum_blacklisted : !simulacrum_whitelisted;
	return 1;
}


static DetourHook is_pause_allowed_hook;

static bool is_pause_allowed_detour(void* gamerules)
{
	return pause_always_stops_time
		|| reinterpret_cast<decltype(&is_pause_allowed_detour)>(is_pause_allowed_hook.original)(gamerules)
		;
}


static owfScript* find_message_blocking_script(const std::string& msg)
{
	std::lock_guard lock(running_scripts_mtx);
	for (auto& scr : running_scripts)
	{
		for (const auto& prefix : scr->blocked_chat_prefixes)
		{
			if (msg.starts_with(prefix))
			{
				return scr.get();
			}
		}
	}
	return nullptr;
}

static luau_CFunction lua_FlashInstance_GetStringVariable_og;

static int lua_FlashInstance_GetStringVariable_detour(luau_State* L)
{
	auto ret = lua_FlashInstance_GetStringVariable_og(L);
	//std::cout << "lua_FlashInstance_GetStringVariable: " << L->intop[1].getString() << " -> " << L->outtop[-1].getString() << std::endl;
	if (soup::joaat::hash(L->intop[1].getString()) == soup::joaat::compileTimeHash("Window.SendMessageBar.MessageBox"))
	{
		std::string current_draft = L->outtop[-1].getString();
		auto blocking_script = find_message_blocking_script(current_draft);

		if (L->intop[-3].type == LUAU_NIL) // Heuristic to determine if the message was just submitted
		{
			if (blocking_script != nullptr)
			{
				blocking_script->blocked_chat_messages.emplace_back(std::move(current_draft));
			}
		}

		if (blocking_script != nullptr && luau_pushstring)
		{
			// Stop the game from processing this
			L->outtop--;
			luau_pushstring(L, " ");
		}

		if (luau_gettable)
		{
			int i = 0;
			while (--i > -20)
			{
				if (L->outtop[i].type == LUAU_TABLE)
				{
					ObfusString SystemMessage("SystemMessage");
					luau_pushstring(L, SystemMessage.c_str());
					if (luau_gettable(L, i - 1) == LUAU_FUNCTION
						//&& *(uint8_t*)(L->outtop[-1].value.as_uintptr + 3) == 0 // Closure::isC
						)
					{
						ChatRedux_table = L->outtop[i - 1].value.as_uintptr;
						ChatRedux_SystemMessage_method = L->outtop[-1].value.as_uintptr;
						L->outtop--;
						break;
					}
					L->outtop--;
				}
			}
		}
	}
	return ret;
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
	config.add(ObfusString("simulacrum_blacklisted"), simulacrum_blacklisted);
	config.add(ObfusString("simulacrum_whitelisted"), simulacrum_whitelisted);
	config.add(ObfusString("pause_always_stops_time"), pause_always_stops_time);
	config.add(ObfusString("enable_http_interface"), enable_http_interface);
	config.add(ObfusString("disable_nrs_connection"), disable_nrs_connection);
	config.add(ObfusString("autologin"), autologin);
	config.add(ObfusString("autologin_email"), autologin_email);
	config.add(ObfusString("autologin_password"), autologin_password);
	{
		auto arr = soup::make_unique<JsonArray>();
		for (const auto& path : auto_start_scripts)
		{
			arr->children.emplace_back(soup::make_unique<JsonString>(path));
		}
		config.add(ObfusString("auto_start_scripts"), std::move(arr));
	}
	config.add(ObfusString("forced_profile_dir"), forced_profile_dir);
	string::toFile(ObfusString("OpenWF/client_config.json").str(), config.encodePretty());
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

static bool check_ec(const std::error_code& ec)
{
	if (ec)
	{
		ObfusString msg("Filesystem error. It's likely your anti-virus is interfering; please ensure the game folder excluded from it.");
		MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
		return false;
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

		owfConsole::activate();

		{
			std::wstring path(_wgetenv(L"windir"));
			path.append(LR"(\System32\dwmapi.dll)");
			og_lib = LoadLibraryW(path.c_str());
#if LOGGING
			std::cout << "og_lib = " << (void*)og_lib << std::endl;
#endif
			og_DwmGetCompositionTimingInfo = GetProcAddress(og_lib, ObfusString("DwmGetCompositionTimingInfo"));
		}

		std::error_code ec{};
		std::filesystem::create_directory(ObfusString("OpenWF").str(), ec);
		SOUP_RETHROW_FALSE(check_ec(ec));
		if (std::filesystem::exists(ObfusString("client_config.json").str())
			&& !std::filesystem::exists(ObfusString("OpenWF/client_config.json").str())
			)
		{
			std::filesystem::rename(ObfusString("client_config.json").str(), ObfusString("OpenWF/client_config.json").str(), ec);
			SOUP_RETHROW_FALSE(check_ec(ec));
		}
		{
			UniquePtr<JsonNode> config = json::decode(string::fromFile(ObfusString("OpenWF/client_config.json").str()));
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("simulacrum_blacklisted")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				simulacrum_blacklisted = it->second->reinterpretAsBool().value;
			}
			else
			{
				simulacrum_blacklisted = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("simulacrum_whitelisted")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				simulacrum_whitelisted = it->second->reinterpretAsBool().value;
			}
			else
			{
				simulacrum_whitelisted = true;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("pause_always_stops_time")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				pause_always_stops_time = it->second->reinterpretAsBool().value;
			}
			else
			{
				pause_always_stops_time = false;
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("auto_start_scripts")); it != config->reinterpretAsObj().end() && it->second->isArr())
			{
				for (const auto& node : it->second->reinterpretAsArr().children)
				{
					if (node->isStr())
					{
						auto_start_scripts.emplace_back(node->reinterpretAsStr());
					}
				}
			}
			else
			{
#if !CONFIG_LOADED_ONLY_ONCE
				auto_start_scripts.clear();
#endif
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("forced_profile_dir")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				forced_profile_dir = it->second->reinterpretAsStr().value;
				if (!forced_profile_dir.empty())
				{
					const auto path = std::filesystem::absolute(forced_profile_dir);
					std::filesystem::create_directories(path);
					forced_profile_dir = string::fixType(path.u8string());
				}
			}
			else
			{
#if !CONFIG_LOADED_ONLY_ONCE
				forced_profile_dir.clear();
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
			//ssl_verify_internal_hook.create();
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
			//Curl_ossl_verifyhost_hook.create();
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
			//verify_worldstate_integrity_hook.create();
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
				//PostProcessInfo_getFov_hook.create();
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

#if PROVIDE_VERSION_INFO && false
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

		{
			SIG_INST("4C 89 44 24 18 48 89 54 24 10 53 48 83 EC 50 48 8D 59 48");
			auto write_to_log_file = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "write_to_log_file = " << write_to_log_file << std::endl;
#endif
			if (write_to_log_file)
			{
				write_to_log_file_hook.detour = reinterpret_cast<void*>(&write_to_log_file_detour);
				write_to_log_file_hook.target = write_to_log_file;
				write_to_log_file_hook.create();
				write_to_log_file_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

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

		{
			SIG_INST("FC C6 D4 49");
			auto lua_SteamService_IsInitialized_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_SteamService_IsInitialized_hash = " << lua_SteamService_IsInitialized_hash.as<void*>() << std::endl;
#endif
			if (lua_SteamService_IsInitialized_hash)
			{
				auto lua_SteamService_IsInitialized_fp = lua_SteamService_IsInitialized_hash.add(8).as<void**>();
#if LOGGING
				std::cout << "lua_SteamService_IsInitialized = " << *lua_SteamService_IsInitialized_fp << std::endl;
#endif
				if (autologin)
				{
					memGuard::setAllowedAccess(lua_SteamService_IsInitialized_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
					lua_SteamService_IsInitialized_og = *lua_SteamService_IsInitialized_fp;
					*lua_SteamService_IsInitialized_fp = reinterpret_cast<void*>(&lua_SteamService_IsInitialized_detour);
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("40 53 56 57 48 83 EC 20 48 83 79 20 00 49 8B F8");
			auto set_lua_global = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "set_lua_global = " << set_lua_global << std::endl;
#endif
			if (set_lua_global)
			{
				set_lua_global_hook.detour = reinterpret_cast<void*>(&set_lua_global_detour);
				set_lua_global_hook.target = set_lua_global;
				set_lua_global_hook.create();
				set_lua_global_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("40 53 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 70 48 8B 81 E0 01 00 00");
			Entity_SetPosition = Module(nullptr).range.scan(sig_inst).as<Entity_SetPosition_t>();
#if LOGGING
			std::cout << "Entity_SetPosition = " << (void*)Entity_SetPosition << std::endl;
#endif
			if (!Entity_SetPosition)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("0F 28 D8 4C 8B C3 48 8B D7 48 8B CE E8");
			auto lua_update_hud = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_update_hud = " << lua_update_hud.as<void*>() << std::endl;
#endif
			if (lua_update_hud)
			{
				auto update_hud = lua_update_hud.add(13).rip().as<void*>();
				lua_update_hud = lua_update_hud.sub(0x0000000141054F72 - 0x0000000141054F10);

				lua_update_hud_hook.detour = reinterpret_cast<void*>(&lua_update_hud_detour);
				lua_update_hud_hook.target = lua_update_hud.as<void*>();
				lua_update_hud_hook.create();
				lua_update_hud_hook.enable();

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

		/*{
			SIG_INST("48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC ? ? ? ? 48 8B FA 41 8B E8 48 8B F1 41 B9");
			luau_newstate = Module(nullptr).range.scan(sig_inst).as<luau_newstate_t>();
#if LOGGING
			std::cout << "luau_newstate = " << (void*)luau_newstate << std::endl;
#endif
			if (!luau_newstate)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}*/

		{
			SIG_INST("48 89 6C 24 18 56 48 83 EC 20 48 8B EA 48 8B F1 48 85 D2");
			luau_pushstring = Module(nullptr).range.scan(sig_inst).as<luau_pushstring_t>();
#if LOGGING
			std::cout << "luau_pushstring = " << (void*)luau_pushstring << std::endl;
#endif
			if (!luau_pushstring)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 75 0F");
			luau_pushpointer = Module(nullptr).range.scan(sig_inst).as<luau_pushpointer_t>();
#if LOGGING
			std::cout << "luau_pushpointer = " << (void*)luau_pushpointer << std::endl;
#endif
			if (!luau_pushpointer)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 75 0F 48 8B 74");
			luau_pushobject = Module(nullptr).range.scan(sig_inst).as<luau_pushobject_t>();
#if LOGGING
			std::cout << "luau_pushobject = " << (void*)luau_pushobject << std::endl;
#endif
			if (!luau_pushobject)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("BA 03 00 00 00 48 8B CF E8 ? ? ? ? BA FF FF FF FF");
			auto luau_gettable_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "luau_gettable_callsite = " << luau_gettable_callsite.as<void*>() << std::endl;
#endif
			if (luau_gettable_callsite)
			{
				luau_gettable = luau_gettable_callsite.add(9).rip().as<luau_gettable_t>();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 89 5C 24 18 57 48 83 EC 20 0F B7 41 50 48 8B D9 66 FF C0");
			luauD_call = Module(nullptr).range.scan(sig_inst).as<luauD_call_t>();
#if LOGGING
			std::cout << "luauD_call = " << (void*)luauD_call << std::endl;
#endif
			if (!luauD_call)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 8D 05 ? ? ? ? 48 89 35 ? ? ? ? 48 89 05 ? ? ? ? BF 01 00 00 00 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? EB 02 33 FF E8 ? ? ? ? 48 8B C8");
			Pointer res[20];
			int nres = Module(nullptr).range.scanWithMultipleResults(sig_inst, res);
			for (int i = 0; i != nres; ++i)
			{
				auto type_arr = res[i].add(3).rip().as<SwigTypeField**>();
				auto type_arr_end = res[i].add(29).rip().as<SwigTypeField**>();
#if LOGGING
				//std::cout << i << std::endl;
				//std::cout << "type_arr = " << (void*)type_arr << std::endl;
				//std::cout << "type_arr_end = " << (void*)type_arr_end << std::endl;
				//std::cout << "type_arr_size = " << (type_arr_end - type_arr) << std::endl;
#endif
				for (auto entry = type_arr; entry != type_arr_end && *entry; ++entry)
				{
					if ((*entry)->type_desc)
					{
#if LOGGING
						//std::cout << "\t- " << (*entry)->type_desc->name << std::endl;
#endif
						swig_types.emplace(soup::joaat::hash((*entry)->type_desc->name), (*entry)->type_desc);
					}
				}
			}
			if (nres == 0)
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("40 55 53 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 0F B6 81 AF 02 00 00");
			auto get_profile_dir = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "get_profile_dir = " << get_profile_dir << std::endl;
#endif
			if (get_profile_dir)
			{
				if (!forced_profile_dir.empty())
				{
					get_profile_dir_hook.detour = reinterpret_cast<void*>(&get_profile_dir_detour);
					get_profile_dir_hook.target = get_profile_dir;
					//get_profile_dir_hook.create();
					get_profile_dir_hook.enable();
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("1E 90 F4 FC 00 00 00 00");
			auto excludedFromSimulacrum_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "excludedFromSimulacrum_hash = " << excludedFromSimulacrum_hash.as<void*>() << std::endl;
#endif
			if (excludedFromSimulacrum_hash)
			{
				auto lua_AvatarEntry_excludedFromSimulacrum_get = *excludedFromSimulacrum_hash.add(8).as<void**>();
#if LOGGING
				std::cout << "lua_AvatarEntry_excludedFromSimulacrum_get = " << lua_AvatarEntry_excludedFromSimulacrum_get << std::endl;
#endif
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.detour = reinterpret_cast<void*>(&lua_AvatarEntry_excludedFromSimulacrum_get_detour);
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.target = lua_AvatarEntry_excludedFromSimulacrum_get;
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.code_cave = Module(nullptr).range.scan(Pattern("CC CC CC CC CC CC CC CC CC CC CC CC CC")).as<void*>();
#if LOGGING
				std::cout << "lua_AvatarEntry_excludedFromSimulacrum_get_hook.code_cave = " << lua_AvatarEntry_excludedFromSimulacrum_get_hook.code_cave << std::endl;
#endif
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.create();
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 78 48 8B D9 E8");
			auto is_pause_allowed = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "is_pause_allowed = " << is_pause_allowed << std::endl;
#endif
			if (is_pause_allowed)
			{
				is_pause_allowed_hook.detour = reinterpret_cast<void*>(&is_pause_allowed_detour);
				is_pause_allowed_hook.target = is_pause_allowed;
				is_pause_allowed_hook.create();
				is_pause_allowed_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("6F 5D A9 54 00 00 00 00");
			auto lua_FlashInstance_GetStringVariable_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_FlashInstance_GetStringVariable_hash = " << lua_FlashInstance_GetStringVariable_hash.as<void*>() << std::endl;
#endif
			if (lua_FlashInstance_GetStringVariable_hash)
			{
				auto lua_FlashInstance_GetStringVariable_fp = lua_FlashInstance_GetStringVariable_hash.add(8).as<luau_CFunction*>();
				lua_FlashInstance_GetStringVariable_og = *lua_FlashInstance_GetStringVariable_fp;
				memGuard::setAllowedAccess(lua_FlashInstance_GetStringVariable_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_FlashInstance_GetStringVariable_fp = lua_FlashInstance_GetStringVariable_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

soup::string::toFile(ObfusString("OpenWF/Download Latest DLL.ps1").str(), ObfusString(R"EOC(Write-Host "Fetching latest DLL version..."
$version = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/latest.txt" -Method Get
Write-Host "Downloading OpenWF Bootstrapper v$version..."
Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$version/dwmapi.dll" -OutFile "../dwmapi.dll")EOC").str());

		{
			std::string reference;
			{
				using namespace soup::literals;
				int dummy;
				reference = (
					#include "OpenWF/Script API Reference.pluto"
				).str();
			}
			soup::string::toFile(ObfusString("OpenWF/Script API Reference.pluto").str(), std::move(reference));
		}

		std::filesystem::create_directory(ObfusString("OpenWF/scripts").str(), ec);
		std::filesystem::create_directory(ObfusString("OpenWF/scripts/samples").str(), ec);
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Auto Teleport to Waypoint.pluto").str(), ObfusString(R"EOC(repeat
	for gRegion:GetLocalPlayer():GetHudStatus():GetFlashMarkers() as marker do
		if marker.markerType == 49 and not marker.garbage then
			gRegion:GetLocalPlayerAvatar():SetPosition(marker.pos)
		end
	end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Become The Stalker.pluto").str(), ObfusString(R"EOC(gRegion:GetLocalPlayerAvatar():InventoryControl():RemoveItem(Engine.SLOT_4, true)
gRegion:GetLocalPlayerAvatar():GiveItem(Type("/Lotus/Types/Enemies/Stalker/StalkerSuit"), true)
gRegion:GetLocalPlayerAvatar():InventoryControl():GetActivePowerSuit():SetXP(1600000))EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Chat Commands.pluto").str(), ObfusString(R"EOC(local commands = {}
commands["/god"] = function()
	if gRegion:GetLocalPlayerAvatar():DamageControl():HasTemporaryImmunity() then
		gRegion:GetLocalPlayerAvatar():DamageControl():RemoveTemporaryImmunity()
		chat_system_reply("Removed immunity.")
	else
		gRegion:GetLocalPlayerAvatar():DamageControl():GiveTemporaryImmunity(500000, 500000)
		chat_system_reply("Granted immunity.")
	end
end
commands["/suicide"] = function()
	if gGameRules instanceof LotusGameRules then
		gRegion:GetLocalPlayerAvatar():Suicide()
	else
		chat_system_reply("That's not a good idea.")
	end
end
commands["/killall"] = function()
	local player = gRegion:GetLocalPlayerAvatar()
	for gRegion:GetAvatars() as avatar do
		if not avatar:IsAvatarFriendly(player) then
			avatar:Suicide()
		end
	end
end
commands["/kdrive"] = function()
	gRegion:CreateEntity(Type("/Lotus/Types/Enemies/Corpus/Venus/Hoverboard/CrpHoverboardUnmannedAvatar"))
end
commands["/simulacrum"] = function()
	local args = Engine.OpenLevelArgs()
	args:SetLevel("/Lotus/Levels/Tenno/SimulacrumEnemySpawnerC.level")
	args:SetGameRules("/Lotus/Types/GameRules/LotusDangerRoomGameRules")
	Engine.OpenLevel(args)
end
commands["/level"] = function(text)
	local level = text:sub(8)
	chat_system_reply("Loading level "..level)
	local args = Engine.OpenLevelArgs()
	args:SetLevel(level)
	Engine.OpenLevel(args)
end
commands["/captura"] = function(text)
	local level = text:sub(10)
	chat_system_reply("Opening Captura in "..level)
	local args = Engine.OpenLevelArgs()
	args:SetLevel(level)
	args:SetGameRules("/Lotus/Types/GameRules/LotusPhotoBoothGameRules")
	Engine.OpenLevel(args)
end
commands["/energy"] = function()
	gRegion:GetLocalPlayerAvatar():InventoryControl():GetActivePowerSuit():SetMaxEnergy(1000000)
	gRegion:GetLocalPlayerAvatar():InventoryControl():GetActivePowerSuit():SetEnergy(1000000)
end
commands["/quit"] = function()
	gFlashMgr:ExecuteToolMenuCommand(Resource("/EE/Editor/ToolMenus/Commands/CmdQuit"))
end
for prefix in commands do
	chat_block_prefix(prefix)
end
repeat
	while evt := owf_next_event() do
		if evt.type == OWF_EVT_BLOCKED_CHAT_MESSAGE then
			for prefix, f in commands do
				if evt.text:sub(1, #prefix) == prefix then
					f(evt.text)
					break
				end
			end
		end
	end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Complete Wave or Mission.pluto").str(), ObfusString(R"EOC(if gGameRules instanceof LotusGameRules then
	gGameRules:OpenMissionContinueDialog(nil)
else
	print("Not available in the current mission")
end)EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Cycle Camera Hotkey (K).pluto").str(), ObfusString(R"EOC(local was_down = false
repeat
	if owf_is_key_down('K')
		and get_active_input_filter() ~= "/EE/Types/Input/MenuInputFilter"
		and get_active_input_filter() ~= "/Lotus/Types/Input/LoadoutReduxInputFilter"
	then
		if not was_down then
			was_down = true
			if gRegion:GetLocalPlayerAvatar():isFollowedByCamera() then
				print("Normal -> Freecam")
				gRegion:GetLocalPlayer():setControllingCamera(true)
				gRegion:GetLocalPlayerAvatar():ControlCamera(false)
			elseif gRegion:GetLocalPlayer():isControllingCamera() then
				print("Freecam -> Locked In Place")
				gRegion:GetLocalPlayer():setControllingCamera(false)
			else
				print("Locked In Place -> Normal")
				gRegion:GetLocalPlayer():setControllingCamera(false)
				gRegion:GetLocalPlayerAvatar():ControlCamera(true)
			end
		end
	else
		was_down = false
	end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Enter Simulacrum.pluto").str(), ObfusString(R"EOC(local args = Engine.OpenLevelArgs()
args:SetLevel("/Lotus/Levels/Tenno/SimulacrumEnemySpawnerC.level")
args:SetGameRules("/Lotus/Types/GameRules/LotusDangerRoomGameRules")
Engine.OpenLevel(args))EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Freecam Teleport on Disable.pluto").str(), ObfusString(R"EOC(local was_in_freecam = false
local last_pos
repeat
    if avatar := gRegion:GetLocalPlayerAvatar() then
        if avatar:isFollowedByCamera() then
            if was_in_freecam then
                was_in_freecam = false
                avatar:SetPosition(last_pos)
            end
        else
            was_in_freecam = gRegion:GetLocalPlayer():isControllingCamera()
            if was_in_freecam then
                last_pos = gRegion:GetGameCamera():GetPosition()
            end
        end
    end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Freecam Up Down.pluto").str(), ObfusString(R"EOC($define VK_CONTROL = 0x11
$define VK_SPACE = 0x20

local Y_STEP <const> = Vector3(0, 0.01, 0)

local t = os.millis()
repeat
	local delta = os.millis() - t
	if gRegion:GetLocalPlayer():isControllingCamera() then
		if owf_is_key_down(VK_SPACE) then
			gRegion:GetGameCamera():SetPosition(gRegion:GetGameCamera():GetPosition() + Y_STEP * delta)
		end
		if owf_is_key_down(VK_CONTROL) then
			gRegion:GetGameCamera():SetPosition(gRegion:GetGameCamera():GetPosition() - Y_STEP * delta)
		end
	end
	t = os.millis()
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Godmode.pluto").str(), ObfusString(R"EOC(repeat
    if avatar := gRegion:GetLocalPlayerAvatar() then
        avatar:DamageControl():GiveTemporaryImmunity(500000, 500000)
    end
until not pcall(yield)

gRegion:GetLocalPlayerAvatar():DamageControl():RemoveTemporaryImmunity())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Increase Damage.pluto").str(), ObfusString(R"EOC(if weapon := gRegion:GetLocalPlayerAvatar():InventoryControl():GetWeaponInHand(0) then
	local impactBehavior = weapon:GetActiveImpactBehavior()
	impactBehavior.criticalHitChance = 10000
	impactBehavior.criticalHitDamageMultiplier = 10000
	print("Your weapon damage has been increased!")
else
	print("You don't seem to have a weapon in hand.")
end)EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Kill All Enemies.pluto").str(), ObfusString(R"EOC(repeat
	local player = gRegion:GetLocalPlayerAvatar()
	for gRegion:GetAvatars() as avatar do
		if not avatar:IsAvatarFriendly(player) then
			avatar:Suicide()
		end
	end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Loot Party.pluto").str(), ObfusString(R"EOC(repeat
	for gRegion:GetAvatars() as avatar do
		if inventory := avatar:InventoryControl() then
			inventory:DoItemDrop()
		end
	end
until yield())EOC").str());
		soup::string::toFile(ObfusString("OpenWF/scripts/samples/Watermark.pluto").str(), ObfusString(R"EOC(local shadow = owf_overlay_add_text(12, 12, "OpenWF", OWF_FONT_SIMPLE8, 0, 0, 0, 2)
local text = owf_overlay_add_text(10, 10, "OpenWF", OWF_FONT_SIMPLE8, 90, 253, 123, 2)
owf_overlay_update()

while pcall(yield) do end

owf_overlay_remove(shadow)
owf_overlay_remove(text)
owf_overlay_update())EOC").str());

		if (!auto_start_scripts.empty())
		{
			ObfusString base_path("OpenWF/scripts/");
			for (const auto& path : auto_start_scripts)
			{
				start_script_from_file(base_path.str() + path);
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
								html = ObfusString(R"EOC(<style>body{font-family:sans-serif;background:#000;filter:invert(1)}</style>
<body>
	<p><label for="server_host">Server Host:</label> <input id="server_host" type="text" /> <button id="server_host_submit">Change</button> <button id="logout">Logout</button></p>
	<p><label for="high_damage_numbers_patch">High Damage Numbers Patch:</label> <input id="high_damage_numbers_patch" type="checkbox" /></p>
	<p><label for="skip_mission_start_timer">Skip Mission Start Timer:</label> <input id="skip_mission_start_timer" type="checkbox" /></p>
	<p><label for="simulacrum_blacklisted">Blacklisted Enemies in Simulacrum:</label> <input id="simulacrum_blacklisted" type="checkbox" /></p>
	<p><label for="simulacrum_whitelisted">Whitelisted Enemies in Simulacrum:</label> <input id="simulacrum_whitelisted" type="checkbox" /></p>
	<p><label for="pause_always_stops_time">Pause Always Stops Time:</label> <input id="pause_always_stops_time" type="checkbox" /></p>
	<p><label for="fov_override">FOV Override (0 = disabled):</label> <input id="fov_override" type="range" min="0" value="0" max="2260000" step="10000"></p>
	<button id="save_config">Save changes to client_config.json</button>
	<hr>
	<p><label for="camtype">Camera Type:</label> <select id="camtype"><option value="gamecam">Normal</option><option value="freecam">Freecam</option><option value="lockcam">Locked In Place</option></select></p>
	<p><label for="pos">Position:</label> <input id="pos" type="text" style="width:230px" onclick="this.select()" readonly /></p>
	<p><button id="tp-submit">Teleport To</button> <select id="tp-target"><option>Custom</option></select> <input id="tp-pos" type="text" style="width:230px" onclick="this.select()" /></p>
	<hr>
	<div id="scripts-container"></div>
	<textarea id="script_log" style="width:100%;height:150px" readonly></textarea>
	<p><label for="console">Console:</label> <input id="console" type="checkbox" /></p>
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

		fetch("/simulacrum_blacklisted").then(res => res.text()).then(res => {
			document.getElementById("simulacrum_blacklisted").checked = (res == "1");
		});
		document.getElementById("simulacrum_blacklisted").onchange = function() {
			fetch("/simulacrum_blacklisted?" + this.checked);
		};

		fetch("/simulacrum_whitelisted").then(res => res.text()).then(res => {
			document.getElementById("simulacrum_whitelisted").checked = (res == "1");
		});
		document.getElementById("simulacrum_whitelisted").onchange = function() {
			fetch("/simulacrum_whitelisted?" + this.checked);
		};

		fetch("/pause_always_stops_time").then(res => res.text()).then(res => {
			document.getElementById("pause_always_stops_time").checked = (res == "1");
		});
		document.getElementById("pause_always_stops_time").onchange = function() {
			if (this.checked) {
				fetch("/start_script_inline?" + encodeURIComponent(`set_pause_always_stops_time(true)`));
			}
			else {
				fetch("/start_script_inline?" + encodeURIComponent(`gGameRules:RequestUnpause() set_pause_always_stops_time(false)`));
			}
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
			"12": "Life Support Capsule",
			"29": "Target",
			"31": "Life Support Pickup",
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

		let script_log_len = 0;
		function pollStatus() {
			fetch("/status?" + script_log_len).then(res => res.json()).then(res => {
				document.getElementById("console").checked = res.console;
				if (res.camtype) {
					document.getElementById("camtype").value = res.camtype;
				}
				document.getElementById("pos").value = res.pos ?? "";

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

				for (const script of document.getElementById("scripts-container").children) {
					const path = script.children[1].getAttribute("data-path");
					script.children[1].checked = res.running_scripts.find(x => x == path);
				}

				if (res.script_log_sub) {
					const log = document.getElementById("script_log");
					log.textContent += res.script_log_sub;
					log.scrollTop = log.scrollHeight;
					script_log_len = res.script_log_len;
				}

				pollStatus();
			}).catch((e) => {
				console.error(e);
				document.body.innerHTML = `<p>Connection to DLL lost. <a href="/">Attempt to reconnect.</a></p>`;
			});
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

		document.getElementById("console").onchange = function() {
			fetch("/toggle_console");
		};

		fetch("/scripts").then(res => res.json()).then(res => {
			res.forEach(script => {
				script = script.split("\\").join("/");
				const p = document.createElement("p");
				const label = document.createElement("label");
				label.setAttribute("for", script);
				label.textContent = script + ": ";
				p.appendChild(label);
				const input = document.createElement("input");
				input.id = script;
				input.type = "checkbox";
				input.setAttribute("data-path", "OpenWF/scripts/" + script);
				input.onchange = function() {
					fetch((this.checked ? "/start_script?" : "/stop_script?") + this.getAttribute("data-path"));
				};
				p.appendChild(input);
				document.getElementById("scripts-container").appendChild(p);
			});
		});
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

					case soup::joaat::compileTimeHash("/simulacrum_whitelisted"):
						if (arr.size() > 1)
						{
							simulacrum_whitelisted = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(simulacrum_whitelisted));
						break;

					case soup::joaat::compileTimeHash("/simulacrum_blacklisted"):
						if (arr.size() > 1)
						{
							simulacrum_blacklisted = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(simulacrum_blacklisted));
						break;

					case soup::joaat::compileTimeHash("/pause_always_stops_time"):
						ServerWebService::sendText(s, std::to_string(pause_always_stops_time));
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
							if (!owfConsole::active)
							{
								owfConsole::activate();
							}
							owfOverlay::setPrelogin(false);
							on_got_server_host();
						}
						ServerWebService::sendText(s, server_host);
						break;

					case soup::joaat::compileTimeHash("/freecam"):
						if (regionmgr && !prohibit_freecam)
						{
							if (auto local_player = regionmgr->GetLocalPlayer())
							{
								local_player->controlling_camera = true;
								local_player->getAvatar()->followed_by_camera = false;
							}
						}
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/lockcam"):
						if (regionmgr && !prohibit_freecam)
						{
							if (auto local_player = regionmgr->GetLocalPlayer())
							{
								local_player->controlling_camera = false;
								local_player->getAvatar()->followed_by_camera = false;
							}
						}
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/gamecam"):
						if (regionmgr && !prohibit_freecam)
						{
							if (auto local_player = regionmgr->GetLocalPlayer())
							{
								local_player->controlling_camera = false;
								local_player->getAvatar()->followed_by_camera = true;
							}
						}
						ServerWebService::send204(s);
						break;

						// Vania Mall: Closet behind Arthur: -15,-6.5,13
						// Vania Mall: Cutscene Room: -19,-6.5,14
					case soup::joaat::compileTimeHash("/teleport"):
						if (Entity_SetPosition && regionmgr && !prohibit_teleport)
						{
							std::vector<std::string> pos_arr;
							if (arr.size() > 1)
							{
								pos_arr = string::explode(arr[1], ',');
							}
							if (pos_arr.size() == 3)
							{
								float pos[3] = {
									strtof(pos_arr[0].c_str(), nullptr),
									strtof(pos_arr[1].c_str(), nullptr),
									strtof(pos_arr[2].c_str(), nullptr)
								};
								Entity_SetPosition(regionmgr->GetLocalPlayerAvatar(), pos);
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
							obj.add(ObfusString("console"), owfConsole::active);
							if (regionmgr)
							{
								if (auto local_player = regionmgr->GetLocalPlayer())
								{
									if (auto avatar = local_player->getAvatar())
									{
										std::string camtype = ObfusString("gamecam").str();
										if (!avatar->followed_by_camera)
										{
											camtype = local_player->controlling_camera ? ObfusString("freecam").str() : ObfusString("lockcam").str();
										}
										obj.add(ObfusString("camtype"), std::move(camtype));

										std::string pos_str;
										pos_str = std::to_string(avatar->pos_x);
										pos_str.push_back(',');
										pos_str.append(std::to_string(avatar->pos_y));
										pos_str.push_back(',');
										pos_str.append(std::to_string(avatar->pos_z));
										obj.add(ObfusString("pos"), std::move(pos_str));
									}
								}
							}
							{
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
							}
							{
								std::lock_guard lock(running_scripts_mtx);
								auto arr = soup::make_unique<JsonArray>();
								for (const auto& scr : running_scripts)
								{
									arr->children.emplace_back(soup::make_unique<JsonString>(std::string(scr->name)));
								}
								obj.add(ObfusString("running_scripts"), std::move(arr));
							}
							if (arr.size() > 1)
							{
								const size_t i = strtoull(arr[1].c_str(), nullptr, 0);
								std::lock_guard lock(script_log_mtx);
								obj.add(ObfusString("script_log_sub"), script_log.substr(i));
								obj.add(ObfusString("script_log_len"), static_cast<int64_t>(script_log.size()));
							}
							ServerWebService::sendText(s, obj.encodePretty());
						}
						break;

					case soup::joaat::compileTimeHash("/toggle_console"):
						if (owfConsole::active)
						{
							owfConsole::deactivate();
						}
						else
						{
							owfConsole::activate();
						}
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/scripts"):
						{
							JsonArray arr;
							for (auto& file : std::filesystem::recursive_directory_iterator(ObfusString("OpenWF/scripts").str()))
							{
								if (std::filesystem::is_regular_file(file))
								{
									arr.children.emplace_back(soup::make_unique<JsonString>(string::fixType(file.path().u8string()).substr(15)));
								}
							}
							ServerWebService::sendText(s, arr.encodePretty());
						}
						break;

					case soup::joaat::compileTimeHash("/start_script"):
						if (!prohibit_scripts)
						{
							start_script_from_file(urlenc::decode(arr[1]));
							ServerWebService::send204(s);
						}
						break;

					case soup::joaat::compileTimeHash("/start_script_inline"):
						if (!prohibit_scripts)
						{
							start_script_from_string(urlenc::decode(arr[1]));
							ServerWebService::send204(s);
						}
						break;

					case soup::joaat::compileTimeHash("/stop_script"):
						{
							std::lock_guard lock(running_scripts_mtx);
							if (auto scr = get_script_by_name(urlenc::decode(arr[1])))
							{
								scr->stop_requested = true;
							}
							ServerWebService::send204(s);
						}
						break;

					case soup::joaat::compileTimeHash("/clear_script_log"):
						script_log.clear();
						break;
					}
				});
				if (serv.bind(61558, &srv))
				{
					serv.run();
				}
				else
				{
					std::cout << ObfusString("Failed to bind TCP/61558. HTTP interface will be unavailable.").str() << std::endl;
				}
			});
			thrd.detach();
		}
	}
	return TRUE;
}
