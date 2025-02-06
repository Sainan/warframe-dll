#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.9"

#define SELF_HOST_CACHE_MANIFEST true
#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true
#define PROVIDE_VERSION_INFO true
#define LABEL_REPLACEMENTS true

// LOGGING should be true when using this
#define VERBOSE_RNG false
#define VERBOSE_CRC32C false

#include <iostream>
#include <mutex>

#include <alloc.hpp>
#include <cat.hpp>
#include <CompactDetourHook.hpp>
#include <DetourHook.hpp>
#include <FileReader.hpp>
#include <filesystem.hpp>
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
#include <WebSocketMessage.hpp>

//#include <wininet.h>
//#pragma comment(lib, "wininet")

#include "whirlpool.hpp"

using namespace soup;

#include "owf_archive.hpp"
#include "owf_config.hpp"
#include "owf_console.hpp"
#include "owf_luau.hpp"
#include "owf_overlay.hpp"
#include "owf_scripting.hpp"
#include "owf_structs.hpp"
#include "owf_tunables.hpp"

static bool disabled_xp_based_level_cap = false;
#if PROVIDE_VERSION_INFO
static char build_label[16] = { 0 }; // e.g. "2024.12.14.10.37"
#endif
static std::string build_hash;
static bool did_auto_login = false;
static std::string auth_query; // e.g. "accountId=6633b81e9dba0b714f28ff02&nonce=8300464181160923&ct=MSI"

static HMODULE og_dwmapi;
static FARPROC og_DwmGetCompositionTimingInfo;
extern "C" __declspec(dllexport) void DwmGetCompositionTimingInfo() { og_DwmGetCompositionTimingInfo(); }

static HMODULE og_wtsapi32;
static FARPROC og_WTSRegisterSessionNotification;
static FARPROC og_WTSUnRegisterSessionNotification;
extern "C" __declspec(dllexport) void WTSRegisterSessionNotification() { og_WTSRegisterSessionNotification(); }
extern "C" __declspec(dllexport) void WTSUnRegisterSessionNotification() { og_WTSUnRegisterSessionNotification(); }


// Cache tunables for faster access
static bool prohibit_skip_mission_start_timer = false;
static bool prohibit_freecam = false;
static bool prohibit_scripts = false;


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


/*struct LegacyParsedUrl
{
	PAD(0, 0x30) const wchar_t* hostname;
};

static DetourHook legacy_parse_url_hook;

static bool legacy_parse_url_detour(LegacyParsedUrl* out, LegacyGameString* in)
{
	std::cout << "legacy_parse_url: " << in->getData() << std::endl;

	LegacyGameString buf;
	strcpy(buf.data, "http://localhost");
	in = &buf;

	auto ret = reinterpret_cast<decltype(&legacy_parse_url_detour)>(legacy_parse_url_hook.original)(out, in);
	if (out->hostname)
	{
		//std::cout << "hostname = " << unicode::utf16_to_utf8(std::wstring(out->hostname)) << std::endl;
	}
	return ret;
}*/


/*static CompactDetourHook internet_connect_hook;

static void internet_connect_detour(uintptr_t a1)
{
	ObfusString localhost("localhost");
	*reinterpret_cast<HINTERNET*>(a1 + 104) = InternetConnectA(
		*reinterpret_cast<HINTERNET*>(a1 + 96),
		localhost.c_str(),
		61558,
		"",
		"",
		INTERNET_SERVICE_HTTP,
		0,
		0
	);
}*/


static DetourHook resolve_addr_hook;

static bool resolve_addr_detour(sockaddr* sa, void* a2, void* a3)
{
	if (sa->sa_family == AF_INET)
	{
#if LOGGING
		std::cout << "resolve_addr called with IPv4, port " << Endianness::toNative(network_u16_t(reinterpret_cast<sockaddr_in*>(sa)->sin_port)) << std::endl;
#endif
		reinterpret_cast<sockaddr_in*>(sa)->sin_addr.s_addr = SOUP_IPV4_NWE(127, 0, 0, 1);
		return reinterpret_cast<decltype(&resolve_addr_detour)>(resolve_addr_hook.original)(sa, a2, a3);
	}
	else if (sa->sa_family == AF_INET6)
	{
#if LOGGING
		std::cout << "resolve_addr called with IPv6, port " << Endianness::toNative(network_u16_t(reinterpret_cast<sockaddr_in6*>(sa)->sin6_port)) << std::endl;
#endif
	}
	else
	{
#if LOGGING
		std::cout << "resolve_addr called with unknown address family" << std::endl;
#endif
	}
	return false;
}


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

#if SELF_HOST_CACHE_MANIFEST
	ObfusString localhost("localhost");
	if (!owfOverlay::isInited())
	{
		host_1 = localhost.c_str();
		port = 61558;
	}
	else
#endif
	{
		host_1 = server_host.c_str();
		if (port == 80)
		{
			port = http_port;
		}
		else
		{
			port = https_port;
		}
	}

	return reinterpret_cast<decltype(&winhttp_connect_detour)>(winhttp_connect_hook.original)(a1, a2, a3, host_1, port, nullptr, nullptr);
}


static DetourHook game_http_request_hook;

struct GameHttpRequest
{
	/* 0x00 */ GameString url;
	PAD(0x10, 0x38) GameString body;
};
static_assert(offsetof(GameHttpRequest, body) == 0x38);

static void on_got_server_host();
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
	if (uri.path == ObfusString("/api/inventory.php").str() || uri.path == ObfusString("/api/missionInventoryUpdate.php").str())
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
		if (auto jr = json::decode(request->body.getData()); jr && jr->isObj())
		{
			if (autologin && !did_auto_login)
			{
				did_auto_login = true;
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
			else
			{
				if (auto it = jr->reinterpretAsObj().findIt(ObfusString("email").str()); it != jr->reinterpretAsObj().end() && it->second->isStr())
				{
					if (it->second->reinterpretAsStr().value.c_str()[0] == '@')
					{
						server_host = it->second->reinterpretAsStr().value.substr(1);
						owfOverlay::redraw();
						on_got_server_host();
						return nullptr;
					}
				}
			}
		}
#if PROVIDE_VERSION_INFO
		if (build_label[0] && !build_hash.empty())
		{
			uri.query.append(ObfusString("&buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			uri.query.append(build_hash);
		}
		uri.query.append(ObfusString("&clientMod=").str());
		uri.query.append(urlenc::encode(ObfusString(BOOTSTRAPPER_TITLE).str()));
#endif
	}
	else if (uri.path == ObfusString("/api/inbox.php").str())
	{
		auth_query = uri.query;
	}
	else if (uri.path.find(ObfusString("/dynamic/worldState.php").str()) != std::string::npos)
	{
#if PROVIDE_VERSION_INFO
		if (build_label[0] && !build_hash.empty())
		{
			uri.query.append(ObfusString("buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			uri.query.append(build_hash);
		}
#endif
	}
	else if (uri.path == ObfusString("/api/logout.php").str())
	{
		owfOverlay::setPrelogin(true);
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


static void on_got_server_host()
{
	string::lower(server_host);
	if (server_host.find(ObfusString(".warframe.com").str()) != std::string::npos)
	{
		server_host = ObfusString("localhost").str();
	}

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

		if (jr && jr->isObj())
		{
			std::lock_guard lock(owfTunables::mtx);

			owfTunables::set.clear();
			for (const auto& e : jr->reinterpretAsObj().children)
			{
				if (e.first->isStr())
				{
					owfTunables::set.emplace_back(joaat::hash(e.first->reinterpretAsStr().value));
				}
			}

			prohibit_skip_mission_start_timer = owfTunables::hasLocked(joaat::compileTimeHash("prohibit_skip_mission_start_timer"));
			prohibit_freecam = owfTunables::hasLocked(joaat::compileTimeHash("prohibit_freecam"));
			prohibit_scripts = owfTunables::hasLocked(joaat::compileTimeHash("prohibit_scripts"));
		}

		owfOverlay::redraw();
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

		owfOverlay::setPrelogin(true);
	}
}


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
	}
	if (!arguments->got_graphicsDriver)
	{
		arguments->got_graphicsDriver = true;
		arguments->graphicsDriver.setShortData(fallback_graphicsDriver);
	}
	if (!arguments->got_cluster)
	{
		arguments->got_cluster = true;
		arguments->cluster.setShortData(fallback_cluster);
	}

	lang_code = std::string(arguments->language.getData(), arguments->language.getSize());
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


#if SELF_HOST_CACHE_MANIFEST
static DetourHook ReadCacheManifest_hook;

static bool ReadCacheManifest_detour(uintptr_t a1)
{
	build_hash = ObfusString("owf_no_manifest").str();
	bool ret = reinterpret_cast<decltype(&ReadCacheManifest_detour)>(ReadCacheManifest_hook.original)(a1);
	reinterpret_cast<GameString*>(a1 + 0x1F0)->setShortData(build_hash);
	return ret;
}
#endif


static DetourHook write_to_log_file_hook;
static ObfusString log_sep("]: ");

static void write_to_log_file_detour(void* const a1, char* const data, size_t _size)
{
	SOUP_IF_LIKELY (_size > 15)
	{
		SOUP_IF_LIKELY (auto message = strstr(data + 15, log_sep.c_str()))
		{
			message += log_sep.size();
			size_t size = _size - (message - data);

			if (size > 10)
			{
				switch (soup::joaat::hashRange(message, 10))
				{
				case soup::joaat::compileTimeHash("Logged in "):
					owfOverlay::setPrelogin(false);
					break;

#if PROVIDE_VERSION_INFO
				case soup::joaat::compileTimeHash("Build Labe"):
					if (size >= 29)
					{
						memcpy(build_label, message + 13, 16);
					}
					break;
#endif

				case soup::joaat::compileTimeHash("Cache mani"): // "Cache manifest hash "
					if (size == 43)
					{
#if !SELF_HOST_CACHE_MANIFEST
						build_hash = std::string(message + 20, 22);
#else
						auto ptr = data + (_size - size) + 20;
						memcpy(ptr, build_hash.data(), build_hash.size()); ptr += build_hash.size();
						*ptr++ = '\n';
						_size = (_size - size) + 20 + build_hash.size() + 1;
#endif
					}
					break;

				case soup::joaat::compileTimeHash("InitMappin"): // "InitMapping for all devices with bindings ... and filter ..."
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

				case soup::joaat::compileTimeHash("Failed to "): // "Failed to created child context for function OWF_..., script: /Lotus/Interface/PostCameraUpdateHud.lua"
					if (size > 100 && soup::joaat::hashRange(message + 10, 39) == soup::joaat::compileTimeHash("created child context for function OWF_"))
					{
						const auto name = std::string(message + 49, size - 100);
#if LOGGING
						std::cout << "OWF callback called: " << name << std::endl;
#endif
						std::lock_guard lock(running_scripts_mtx);
						for (auto& scr : running_scripts)
						{
							if (scr->callbacks.contains(name))
							{
								scr->callbacks.erase(name);
								scr->events.emplace_back(OWF_EVT_CALLBACK, std::move(name));
								break;
							}
						}
						return; // Don't log this
					}
					break;
				}
			}
		}
	}
	if (ee_log_in_console)
	{
		std::cout << std::string(data, _size);
	}
	reinterpret_cast<decltype(&write_to_log_file_detour)>(write_to_log_file_hook.original)(a1, data, _size);
}


static luau_CFunction lua_FlashMgr_GetConfigBool_og;

static int lua_FlashMgr_GetConfigBool_detour(luau_State* L)
{
	if (!did_auto_login)
	{
		SOUP_IF_LIKELY (L->intop[1].type == LUAU_STRING)
		{
			ObfusString str("Client.AutoLogin");
			if (strcmp(L->intop[1].getString(), str.c_str()) == 0)
			{
#if LOGGING
				std::cout << "Reporting Client.AutoLogin as true" << std::endl;
#endif
				L->outtop[-1].value.as_bool = true;
				L->outtop[-1].type = LUAU_BOOL;
				return 1;
			}
		}
	}

	return lua_FlashMgr_GetConfigBool_og(L);
}


static DetourHook lua_set_global_hook;

static void lua_set_global_detour(luau_State* L, const char* name)
{
	switch (soup::joaat::hash(name))
	{
	case soup::joaat::compileTimeHash("gRegion"):
		regionmgr = L->outtop[-1].type == LUAU_USERDATA ? ***(RegionMgr****)(L->outtop[-1].value.as_uintptr + 0x18) : nullptr;
#if LOGGING
		std::cout << "lua_set_global: gRegion = " << regionmgr << std::endl;
#endif
		if (!owfOverlay::isInited())
		{
			owfOverlay::init();
		}
		break;

	case soup::joaat::compileTimeHash("gFlashMgr"):
		flashmgr = L->outtop[-1].type == LUAU_USERDATA ? ***(Object****)(L->outtop[-1].value.as_uintptr + 0x18) : nullptr;
#if LOGGING
		std::cout << "lua_set_global: gFlashMgr = " << flashmgr << std::endl;
#endif
		break;

	case soup::joaat::compileTimeHash("gGameData"):
		gamedata = L->outtop[-1].type == LUAU_USERDATA ? ***(Object****)(L->outtop[-1].value.as_uintptr + 0x18) : nullptr;
#if LOGGING
		std::cout << "lua_set_global: gGameData = " << gamedata << std::endl;
#endif
		break;

	case soup::joaat::compileTimeHash("gMatchingService"):
		matchingservice = L->outtop[-1].type == LUAU_USERDATA ? *(void**)(L->outtop[-1].value.as_uintptr + 0x18) : nullptr;
#if LOGGING
		std::cout << "lua_set_global: gMatchingService = " << matchingservice << std::endl;
#endif
		break;
	}
	return reinterpret_cast<decltype(&lua_set_global_detour)>(lua_set_global_hook.original)(L, name);
}


static void populate_running_scripts_locked(JsonObject& obj)
{
	auto arr = soup::make_unique<JsonArray>();
	for (const auto& scr : running_scripts)
	{
		arr->children.emplace_back(soup::make_unique<JsonString>(std::string(scr->name)));
	}
	obj.add(ObfusString("running_scripts"), std::move(arr));
}

static void populate_running_scripts(JsonObject& obj)
{
	std::lock_guard lock(running_scripts_mtx);
	return populate_running_scripts_locked(obj);
}

static void broadcast_running_scripts_locked()
{
	JsonObject obj;
	populate_running_scripts_locked(obj);
	owf_broadcast_message(obj.encode());
}

static void start_script_from_file(std::string&& path)
{
	auto scr = soup::make_unique<owfScript>();
	if (scr->loadFile(std::move(path)))
	{
		std::lock_guard lock(running_scripts_mtx);
		running_scripts.emplace_back(std::move(scr));
		broadcast_running_scripts_locked();
	}
}

static void start_script_from_string(std::string&& code)
{
	auto scr = soup::make_unique<owfScript>();
	if (scr->loadString(std::move(code)))
	{
		std::lock_guard lock(running_scripts_mtx);
		running_scripts.emplace_back(std::move(scr));
		broadcast_running_scripts_locked();
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

static luau_CFunction lua_LotusHudStatus_UpdateFlashMarkers_og;
static int lua_LotusHudStatus_UpdateFlashMarkers_detour(luau_State* L)
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
#if LOGGING
		std::cout << luau_error_msg << std::endl;
#endif
		throw 0;
	};
	{
		std::lock_guard mtx(running_scripts_mtx);
		if (bgscript != nullptr)
		{
			SOUP_IF_UNLIKELY (!bgscript->tick())
			{
				delete bgscript;
				bgscript = nullptr;
			}
		}
		bool any_killed = false;
		for (auto i = running_scripts.begin(); i != running_scripts.end(); )
		{
			if (!prohibit_scripts && (*i)->tick())
			{
				++i;
			}
			else
			{
				i = running_scripts.erase(i);
				any_killed = true;
			}
		}
		SOUP_IF_UNLIKELY (any_killed)
		{
			broadcast_running_scripts_locked();
		}
	}

#if PRIVATE
	if (L->outtop != og_outtop)
	{
		owfScript::logNl("Not all values were popped from LuaU stack");
	}
#endif
	L->outtop = og_outtop;
	L->intop = og_intop;
	L->global_state->error_longjump_data = og_lngjmp;
	L->global_state->panic_func = og_panic;

	return lua_LotusHudStatus_UpdateFlashMarkers_og(L);
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

static luau_CFunction lua_FlashInstance_GetStringVariable_og;

static int lua_FlashInstance_GetStringVariable_detour(luau_State* L)
{
	auto ret = lua_FlashInstance_GetStringVariable_og(L);
	//std::cout << "lua_FlashInstance_GetStringVariable: " << L->intop[1].getString() << " -> " << L->outtop[-1].getString() << std::endl;
	if (soup::joaat::hash(L->intop[1].getString()) == soup::joaat::compileTimeHash("Window.SendMessageBar.MessageBox"))
	{
		owfScript* blocking_script = nullptr;
		{
			std::string current_draft = L->outtop[-1].getString();

			std::lock_guard lock(running_scripts_mtx);
			for (auto& scr : running_scripts)
			{
				if (scr->isBlockingMessage(current_draft))
				{
					blocking_script = scr.get();
					break;
				}
			}
			/*if (!blocking_script && bgscript && bgscript->isBlockingMessage)
			{
				blocking_script = bgscript;
			}*/

			if (L->intop[-3].type == LUAU_NIL) // Heuristic to determine if the message was just submitted
			{
				if (blocking_script != nullptr)
				{
					blocking_script->events.emplace_back(OWF_EVT_BLOCKED_CHAT_MESSAGE, std::move(current_draft));
				}
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


#if LABEL_REPLACEMENTS
static CompactDetourHook check_string_substitutions_hook;

struct PermanentString
{
	size_t size;
	char data[1];
};
static std::unordered_map<uint32_t, PermanentString*> permanent_strings;
static PermanentString* fossilise_string(const char* data, size_t size)
{
	const auto hash = soup::joaat::hashRange(data, size);
	if (auto e = permanent_strings.find(hash); e != permanent_strings.end())
	{
		return e->second;
	}
	auto ps = (PermanentString*)soup::malloc(offsetof(PermanentString, data) + size + 1);
	ps->size = size;
	memcpy(ps->data, data, size);
	ps->data[size] = 0;
	permanent_strings.emplace(hash, ps);
	return ps;
}

static uint8_t to_lower_table[256] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 };
static uint32_t lower_hash(const char* data, size_t size)
{
	uint32_t val = 0;
	while (size-- != 0)
	{
		val += to_lower_table[*(uint8_t*)(data++)];
		val += (val << 10);
		val ^= (val >> 6);
	}
	return (0x8001 * (((uint32_t)(9 * val) >> 11) ^ (9 * val)));
}

static Mutex label_replacements_mtx;
static std::unordered_map<uint32_t, PermanentString*> label_replacements;
static void load_label_replacements()
{
	std::lock_guard lock(label_replacements_mtx);
	label_replacements.clear();
	FileReader fr(ObfusString("OpenWF/Label Replacements.cat.txt"));
	if (auto root = soup::cat::parse(fr))
	{
		for (const auto& e : root->children)
		{
			const auto hash = lower_hash(e->name.data(), e->name.size());
			const auto ps = fossilise_string(e->value.data(), e->value.size());
			label_replacements.emplace(hash, ps);
		}
	}
}

static void check_string_substitutions_detour(GameString* str, void* substitutions, GameString* loctag, bool dont_log)
{
	if (dont_resolve_labels)
	{
		std::swap(*str, *loctag);
		return;
	}
	{
		const auto hash = lower_hash(loctag->getData(), loctag->getSize());
		std::lock_guard lock(label_replacements_mtx);
		if (auto e = label_replacements.find(hash); e != label_replacements.end())
		{
			str->setUnownedData(e->second->data, e->second->size);
		}
	}
	return reinterpret_cast<decltype(&check_string_substitutions_detour)>(check_string_substitutions_hook.original)(str, substitutions, loctag, dont_log);
}
#endif


#if VERBOSE_RNG
static int64_t* lua_seed;
static luau_CFunction lua_SetSeed_og;
static luau_CFunction lua_ChurnSeed_og;
static luau_CFunction lua_SRandom_og;
static luau_CFunction lua_SRandomInt_og;
static luau_CFunction lua_HashCrc32_og;

static int lua_SetSeed_detour(luau_State* L)
{
	lua_SetSeed_og(L);
	std::cout << "lua_SetSeed: lua_seed is now " << (lua_seed ? std::to_string(*lua_seed) : "[unknown]") << std::endl;
	return 0;
}

static int lua_ChurnSeed_detour(luau_State* L)
{
	lua_ChurnSeed_og(L);
	std::cout << "lua_ChurnSeed: " << L->intop[1].value.as_float << " iterations; lua_seed is now " << (lua_seed ? std::to_string(*lua_seed) : "[unknown]") << std::endl;
	return 0;
}

static int lua_SRandom_detour(luau_State* L)
{
	lua_SRandom_og(L);
	std::cout << "lua_SRandom(" << L->intop[0].value.as_float << ", " << L->intop[1].value.as_float << "): generated " << L->outtop[-1].value.as_float << "; lua_seed is now " << (lua_seed ? std::to_string(*lua_seed) : "[unknown]") << std::endl;
	return 1;
}

static int lua_SRandomInt_detour(luau_State* L)
{
	lua_SRandomInt_og(L);
	std::cout << "lua_SRandomInt(" << L->intop[0].value.as_float << ", " << L->intop[1].value.as_float << "): generated " << L->outtop[-1].value.as_float << "; lua_seed is now " << (lua_seed ? std::to_string(*lua_seed) : "[unknown]") << std::endl;
	return 1;
}

static int lua_HashCrc32_detour(luau_State* L)
{
	lua_HashCrc32_og(L);
	std::cout << "lua_HashCrc32(" << L->intop[0].getString() << "): returned " << L->outtop[-1].value.as_float << std::endl;
	return 1;
}
#endif


#if VERBOSE_CRC32C
static DetourHook crc32c_impl_hook;

static uint32_t crc32c_impl_detour(uint32_t initial, const char* data, size_t size)
{
	auto res = reinterpret_cast<decltype(&crc32c_impl_detour)>(crc32c_impl_hook.original)(initial, data, size);
	std::cout << "CRC32C: initial = " << initial << ", data = " << string::bin2hex(std::string(data, size)) << ", res = " << res << ", caller offset = " << Pointer(_ReturnAddress()).sub(Module(nullptr).range.base.as<uintptr_t>()).as<void*>() << std::endl;
	return res;
}
#endif


static void save_config()
{
	JsonObject config;

	config.add(ObfusString("fallback_language"), fallback_language);
	config.add(ObfusString("fallback_graphicsDriver"), fallback_graphicsDriver);
	config.add(ObfusString("fallback_cluster"), fallback_cluster);

	config.add(ObfusString("server_host"), server_host);
	config.add(ObfusString("http_port"), http_port);
	config.add(ObfusString("https_port"), https_port);
	config.add(ObfusString("autologin"), autologin);
	config.add(ObfusString("autologin_email"), autologin_email);
	config.add(ObfusString("autologin_password"), autologin_password);

	config.add(ObfusString("high_damage_numbers_patch"), high_damage_numbers_patch);
	config.add(ObfusString("simulacrum_blacklisted"), simulacrum_blacklisted);
	config.add(ObfusString("simulacrum_whitelisted"), simulacrum_whitelisted);
	config.add(ObfusString("pause_always_stops_time"), pause_always_stops_time);
	config.add(ObfusString("disable_nrs_connection"), disable_nrs_connection);

	config.add(ObfusString("ee_log_in_console"), ee_log_in_console);
	config.add(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
	config.add(ObfusString("fov_override"), fov_override);
	config.add(ObfusString("forced_profile_dir"), forced_profile_dir);
	{
		auto arr = soup::make_unique<JsonArray>();
		for (const auto& path : auto_start_scripts)
		{
			arr->children.emplace_back(soup::make_unique<JsonString>(path));
		}
		config.add(ObfusString("auto_start_scripts"), std::move(arr));
	}
	config.add(ObfusString("dont_resolve_labels"), dont_resolve_labels);

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

static Server serv;

struct owfWebsocketTag {};

static void start_bgscript()
{
	std::string code;
#if PRIVATE
	code = string::fromFile("OpenWF/bgscript.pluto");
	if (code.empty())
#endif
	{
		uint32_t size;
		auto data = g_archive.find(soup::joaat::compileTimeHash("OpenWF/bgscript.pluto"), size);
		code = std::string(data, size);
	}

	std::lock_guard lock(running_scripts_mtx);
	bgscript = new owfScript();
	bgscript->loadString(std::move(code));
	bgscript->tick();
}

static void restart_bgscript()
{
	if (bgscript)
	{
		bgscript->stop_requested = true;
		while (bgscript)
		{
			Sleep(10);
		}
	}

	start_bgscript();
}

static void populate_initial_status(JsonObject& obj)
{
	obj.add(ObfusString("console"), owfConsole::active);
	obj.add(ObfusString("bgscript_status_string"), bgscript_status_string);
}

static void populate_full_script_log(JsonObject& obj)
{
	std::lock_guard lock(script_log_mtx);
	obj.add(ObfusString("script_log"), script_log);
	obj.add(ObfusString("script_log_len"), static_cast<int64_t>(script_log.size()));
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

		owfConsole::activate(BOOTSTRAPPER_TITLE);

		{
			std::wstring path(_wgetenv(L"windir"));
			path.append(LR"(\System32\dwmapi.dll)");
			og_dwmapi = LoadLibraryW(path.c_str());
#if LOGGING
			std::cout << "og_dwmapi = " << (void*)og_dwmapi << std::endl;
#endif
			og_DwmGetCompositionTimingInfo = GetProcAddress(og_dwmapi, "DwmGetCompositionTimingInfo");
		}

		{
			std::wstring path(_wgetenv(L"windir"));
			path.append(LR"(\System32\wtsapi32.dll)");
			og_wtsapi32 = LoadLibraryW(path.c_str());
#if LOGGING
			std::cout << "og_wtsapi32 = " << (void*)og_wtsapi32 << std::endl;
#endif
			og_WTSRegisterSessionNotification = GetProcAddress(og_wtsapi32, "WTSRegisterSessionNotification");
			og_WTSUnRegisterSessionNotification = GetProcAddress(og_wtsapi32, "WTSUnRegisterSessionNotification");
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("ee_log_in_console")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				ee_log_in_console = it->second->reinterpretAsBool().value;
			}
			else
			{
				ee_log_in_console = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("dont_resolve_labels")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				dont_resolve_labels = it->second->reinterpretAsBool().value;
			}
			else
			{
				dont_resolve_labels = false;
			}
		}
		save_config();

		bool is_legacy = false;

		// 2018.02.22.14.34 (M:8004325165498360760)
		/*{
			SIG_INST("48 89 5C 24 18 55 56 57 48 8D AC 24 00 FA FF FF 48 81 EC 00 07 00 00 48 8B 05 ? ? ? ? 48 33 C4");
			auto legacy_parse_url = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "legacy_parse_url = " << legacy_parse_url << std::endl;
#endif
			if (legacy_parse_url)
			{
				legacy_parse_url_hook.detour = reinterpret_cast<void*>(&legacy_parse_url_detour);
				legacy_parse_url_hook.target = legacy_parse_url;
				legacy_parse_url_hook.create();
				legacy_parse_url_hook.enable();

				is_legacy = true;
			}
		}*/

		// 2018.02.22.14.34 (M:8004325165498360760)
		/*{
			SIG_INST("40 53 48 81 EC 60 02 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 02 00 00 4C 8B 49 08");
			auto internet_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "internet_connect = " << internet_connect << std::endl;
#endif
			if (internet_connect)
			{
				internet_connect_hook.detour = reinterpret_cast<void*>(&internet_connect_detour);
				internet_connect_hook.target = internet_connect;
				internet_connect_hook.code_cave = Module(nullptr).range.scan(CompactDetourHook::getCodeCavePattern()).as<void*>();
				internet_connect_hook.create();
				internet_connect_hook.enable();

				is_legacy = true;
			}
		}*/

		// 2018.02.22.14.34 (M:8004325165498360760)
		{
			SIG_INST("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B7 01");
			auto resolve_addr = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "resolve_addr = " << resolve_addr << std::endl;
#endif
			if (resolve_addr)
			{
				resolve_addr_hook.detour = reinterpret_cast<void*>(&resolve_addr_detour);
				resolve_addr_hook.target = resolve_addr;
				resolve_addr_hook.create();
				resolve_addr_hook.enable();

				is_legacy = true;
			}
		}

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

		if (!is_legacy)
		{
			SIG_INST("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 68 0C 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 0C 00 00");
			auto winhttp_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "winhttp_connect = " << winhttp_connect << std::endl;
#endif
			if (winhttp_connect)
			{
				winhttp_connect_hook.detour = reinterpret_cast<void*>(&winhttp_connect_detour);
				winhttp_connect_hook.target = winhttp_connect;
				winhttp_connect_hook.create();
				winhttp_connect_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An important pattern scan has failed. The game will likely fail to start.") << std::endl;
			}
		}

#if SELF_HOST_CACHE_MANIFEST
		if (!is_legacy)
		{
			SIG_INST("75 0E 48 8B 45 ? ? 3B 47 08 0F");
			auto cache_hash_checks = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
#if LOGGING
			std::cout << "cache_hash_checks = " << (void*)cache_hash_checks << std::endl;
#endif
			if (!cache_hash_checks)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			}
			memGuard::setAllowedAccess(cache_hash_checks, 12, memGuard::ACC_RWX);
			cache_hash_checks[0] = 0x90;
			cache_hash_checks[1] = 0x90;
			cache_hash_checks[10] = 0x90;
			cache_hash_checks[11] = 0xe9;
		}
#endif

		if (!is_legacy)
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

		if (!is_legacy)
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
			SIG_INST("49 8B D4 48 8B ? E8 ? ? ? ? 85 C0 7F");
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

		if (!is_legacy)
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
		if (!is_legacy)
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

		// Same idea for 2018.02.22.14.34 (M:8004325165498360760), but can't see any immediate issues with it.
		if (is_legacy)
		{
			SIG_INST("E8 ? ? ? ? 0F B6 84 24 ? ? ? ? 88 05 ? ? ? ? 0F B6 84");
			auto insn = Module(nullptr).range.scan(sig_inst).add(5).as<uint8_t*>();
#if LOGGING
			std::cout << "is_stripped_insn = " << (void*)insn << std::endl;
#endif
			if ((uintptr_t)insn != 5)
			{
				memGuard::setAllowedAccess(insn, 8, memGuard::ACC_RWX);
				insn[0] = 0x31;
				insn[1] = 0xc0;
				insn[2] = 0x90;
				insn[3] = 0x90;
				insn[4] = 0x90;
				insn[5] = 0x90;
				insn[6] = 0x90;
				insn[7] = 0x90;
			}
			else
			{
				std::cout << ObfusString("An important pattern scan has failed. The game will likely fail to start.") << std::endl;
			}
		}

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

#if SELF_HOST_CACHE_MANIFEST
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

		{
			SIG_INST("FC 94 94 BF 00 00 00 00 ? ? ? ? ? ? ? ? C0 99 E8 D0 00 00 00 00");
			auto lua_FlashMgr_GetConfigBool_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_FlashMgr_GetConfigBool_hash = " << lua_FlashMgr_GetConfigBool_hash.as<void*>() << std::endl;
#endif
			if (lua_FlashMgr_GetConfigBool_hash)
			{
				if (autologin)
				{
					auto lua_FlashMgr_GetConfigBool_fp = lua_FlashMgr_GetConfigBool_hash.add(8).as<luau_CFunction*>();
					lua_FlashMgr_GetConfigBool_og = *lua_FlashMgr_GetConfigBool_fp;
					memGuard::setAllowedAccess(lua_FlashMgr_GetConfigBool_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
					*lua_FlashMgr_GetConfigBool_fp = lua_FlashMgr_GetConfigBool_detour;
				}
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 F6 41 01 04 48 8B FA");
			auto lua_set_global = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "lua_set_global = " << lua_set_global << std::endl;
#endif
			if (lua_set_global)
			{
				lua_set_global_hook.detour = reinterpret_cast<void*>(&lua_set_global_detour);
				lua_set_global_hook.target = lua_set_global;
				lua_set_global_hook.create();
				lua_set_global_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("C2 96 84 6B 00 00 00 00");
			auto lua_LotusHudStatus_UpdateFlashMarkers_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_LotusHudStatus_UpdateFlashMarkers_hash = " << lua_LotusHudStatus_UpdateFlashMarkers_hash.as<void*>() << std::endl;
#endif
			if (lua_LotusHudStatus_UpdateFlashMarkers_hash)
			{
				auto lua_LotusHudStatus_UpdateFlashMarkers_fp = lua_LotusHudStatus_UpdateFlashMarkers_hash.add(8).as<luau_CFunction*>();
				lua_LotusHudStatus_UpdateFlashMarkers_og = *lua_LotusHudStatus_UpdateFlashMarkers_fp;
				memGuard::setAllowedAccess(lua_LotusHudStatus_UpdateFlashMarkers_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_LotusHudStatus_UpdateFlashMarkers_fp = lua_LotusHudStatus_UpdateFlashMarkers_detour;
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
			SIG_INST("E8 ? ? ? ? 4C 8B C5 48 8B D7 48 8B CE E8 ? ? ? ? BA FE FF FF FF 48 8B CE E8 ? ? ? ? BA FC FF FF FF 48 8B CE E8");
			auto luau_createtable_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "luau_createtable_callsite = " << luau_createtable_callsite.as<void*>() << std::endl;
#endif
			if (luau_createtable_callsite)
			{
				luau_createtable = luau_createtable_callsite.add(1).rip().as<luau_createtable_t>();
				luau_settable = luau_createtable_callsite.add(41).rip().as<luau_settable_t>();
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
			SIG_INST("48 8B 05 ? ? ? ? 4C 8D ? ? ? ? ? 4D 8B");
			Pointer res[10];
			int nres = Module(nullptr).range.scanWithMultipleResults(sig_inst, res);
			swig_enums.reserve(nres);
			for (int i = 0; i != nres; ++i)
			{
				swig_enums.emplace_back(res[i].add(3).rip().as<SwigEnum*>());
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
				lua_AvatarEntry_excludedFromSimulacrum_get_hook.code_cave = Module(nullptr).range.scan(CompactDetourHook::getCodeCavePattern()).as<void*>();
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

#if VERBOSE_RNG
		{
			SIG_INST("48 89 1D ? ? ? ? 85 C0 74 27 48 B9 2D 7F 95 4C 2D F4 51 58");
			auto lua_seed_mov = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_seed_mov = " << lua_seed_mov.as<void*>() << std::endl;
#endif
			if (lua_seed_mov)
			{
				lua_seed = lua_seed_mov.add(3).rip().as<int64_t*>();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("FF 51 68 4F 00 00 00 00");
			auto lua_SetSeed_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_SetSeed_hash = " << lua_SetSeed_hash.as<void*>() << std::endl;
#endif
			if (lua_SetSeed_hash)
			{
				auto lua_SetSeed_fp = lua_SetSeed_hash.add(8).as<luau_CFunction*>();
				lua_SetSeed_og = *lua_SetSeed_fp;
				memGuard::setAllowedAccess(lua_SetSeed_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_SetSeed_fp = lua_SetSeed_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("05 3F 88 84 00 00 00 00");
			auto lua_ChurnSeed_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_ChurnSeed_hash = " << lua_ChurnSeed_hash.as<void*>() << std::endl;
#endif
			if (lua_ChurnSeed_hash)
			{
				auto lua_ChurnSeed_fp = lua_ChurnSeed_hash.add(8).as<luau_CFunction*>();
				lua_ChurnSeed_og = *lua_ChurnSeed_fp;
				memGuard::setAllowedAccess(lua_ChurnSeed_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_ChurnSeed_fp = lua_ChurnSeed_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("F8 4C 6E DD 00 00 00 00 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? F9 62 5E 0C 00 00 00 00");
			auto lua_SRandom_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_SRandom_hash = " << lua_SRandom_hash.as<void*>() << std::endl;
#endif
			if (lua_SRandom_hash)
			{
				auto lua_SRandom_fp = lua_SRandom_hash.add(8).as<luau_CFunction*>();
				lua_SRandom_og = *lua_SRandom_fp;
				memGuard::setAllowedAccess(lua_SRandom_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_SRandom_fp = lua_SRandom_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("F9 62 5E 0C 00 00 00 00 ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 5B CF 93 5F 00 00 00 00");
			auto lua_SRandomInt_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_SRandomInt_hash = " << lua_SRandomInt_hash.as<void*>() << std::endl;
#endif
			if (lua_SRandomInt_hash)
			{
				auto lua_SRandomInt_fp = lua_SRandomInt_hash.add(8).as<luau_CFunction*>();
				lua_SRandomInt_og = *lua_SRandomInt_fp;
				memGuard::setAllowedAccess(lua_SRandomInt_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_SRandomInt_fp = lua_SRandomInt_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		{
			SIG_INST("51 E0 F5 F1 00 00 00 00");
			auto lua_HashCrc32_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_HashCrc32_hash = " << lua_HashCrc32_hash.as<void*>() << std::endl;
#endif
			if (lua_HashCrc32_hash)
			{
				auto lua_HashCrc32_fp = lua_HashCrc32_hash.add(8).as<luau_CFunction*>();
				lua_HashCrc32_og = *lua_HashCrc32_fp;
				memGuard::setAllowedAccess(lua_HashCrc32_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_HashCrc32_fp = lua_HashCrc32_detour;
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

#if VERBOSE_CRC32C
		{
			SIG_INST("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 83 3D");
			auto crc32c_impl = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "crc32c_impl = " << crc32c_impl << std::endl;
#endif
			if (crc32c_impl)
			{
				crc32c_impl_hook.detour = reinterpret_cast<void*>(&crc32c_impl_detour);
				crc32c_impl_hook.target = crc32c_impl;
				crc32c_impl_hook.create();
				crc32c_impl_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}
#endif

		{
			SIG_INST("4C 8B DC 57 41 57 48 83 EC 78 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 48");
			auto check_string_substitutions = Module(nullptr).range.scan(sig_inst).as<void*>();
			if (check_string_substitutions)
			{
				check_string_substitutions_hook.detour = reinterpret_cast<void*>(&check_string_substitutions_detour);
				check_string_substitutions_hook.target = check_string_substitutions;
				check_string_substitutions_hook.code_cave = Module(nullptr).range.scan(CompactDetourHook::getCodeCavePattern()).as<void*>();
#if LOGGING
				std::cout << "check_string_substitutions_hook.code_cave = " << check_string_substitutions_hook.code_cave << std::endl;
#endif
				check_string_substitutions_hook.create();
				check_string_substitutions_hook.enable();
			}
			else
			{
				std::cout << ObfusString("An optional pattern scan has failed. Functionality may be limited beyond core precepts.") << std::endl;
			}
		}

		if (auto hotfix = string::fromFile(ObfusString("OpenWF/hotfix.bin").str()); !hotfix.empty())
		{
			if (g_archive.loadHotfix(hotfix.data(), hotfix.size(), soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE)))
			{
				std::cout << ObfusString("Hotfix applied") << std::endl;
			}
			else
			{
				std::cout << ObfusString("Failed to apply hotfix as it was made for a different DLL version") << std::endl;
				g_archive.loadBuiltin();
			}
		}
		else
		{
			g_archive.loadBuiltin();
		}
		start_bgscript();

#if LABEL_REPLACEMENTS
		load_label_replacements();
#endif

		if (!auto_start_scripts.empty())
		{
			ObfusString base_path("OpenWF/scripts/");
			for (const auto& path : auto_start_scripts)
			{
				start_script_from_file(base_path.str() + path);
			}
		}

		{
			Thread thrd([](Capture&&)
			{
				ServerWebService srv([](soup::Socket& s, soup::HttpRequest&& req, soup::ServerWebService&)
				{
#if LOGGING
					std::cout << "Request to builtin HTTP server: " << req.path << std::endl;
#endif
					auto arr = string::explode(req.path, '?');
#if SELF_HOST_CACHE_MANIFEST
					if (!owfOverlay::isInited()
						&& (arr.at(0).find(ObfusString("/0").str()) != std::string::npos
							|| arr.at(0).find(ObfusString("/7").str()) != std::string::npos
							)
						)
					{
						if (arr.at(0).find(ObfusString("/0/H.Cache.bin!D_---------------------w").str()) != std::string::npos)
						{
							// Example request: /origin/075B4E6D/0/H.Cache.bin!D_---------------------w

							// This is H.Cache.bin for 35.6.1, but hash checks are patched out so it doesn't matter.
							const uint8_t bin[] = {
								0x53, 0x48, 0x43, 0x43, 0x1f, 0x00, 0x00, 0x00, 0x02, 0x8a, 0x03, 0x00, 0x00, 0x0e, 0x02, 0x00,
								0x00, 0x80, 0x00, 0x08, 0x18, 0x00, 0x00, 0x71, 0x41, 0x8c, 0x06, 0x00, 0x02, 0x00, 0x88, 0x01,
								0xfe, 0x03, 0x0e, 0xf5, 0x7d, 0xd5, 0xba, 0x70, 0x15, 0x00, 0x01, 0x99, 0xc0, 0x58, 0x8e, 0x11,
								0x8a, 0xe6, 0x5e, 0x46, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x11, 0x2f, 0x42,
								0x2e, 0x43, 0x61, 0x63, 0x68, 0x65, 0x2e, 0x44, 0x78, 0x31, 0x31, 0x2e, 0x62, 0x69, 0x6e, 0xd4,
								0xdb, 0x97, 0x7e, 0xe9, 0x35, 0xac, 0xb0, 0x92, 0x1f, 0x67, 0x78, 0x10, 0x0c, 0x05, 0x2f, 0x83,
								0x6e, 0x03, 0x20, 0x32, 0x3c, 0xf7, 0x21, 0x97, 0x5a, 0x8c, 0x6e, 0x89, 0xa6, 0xbf, 0xd7, 0xc8,
								0x89, 0xdf, 0x0c, 0x2d, 0x04, 0x79, 0x14, 0x57, 0x69, 0x6e, 0x64, 0x6f, 0x77, 0x73, 0xdb, 0x5b,
								0x28, 0x33, 0xd1, 0x30, 0x6f, 0x49, 0x88, 0xa7, 0xd3, 0xe9, 0x6c, 0x24, 0x24, 0x5a, 0xd3, 0x59,
								0x79, 0x20, 0x17, 0x5f, 0x64, 0x65, 0xb7, 0x95, 0x4e, 0xa7, 0x5e, 0x99, 0x24, 0xc8, 0x49, 0xd9,
								0x91, 0x77, 0xa6, 0x11, 0x08, 0x1f, 0x53, 0x04, 0x00, 0x65, 0x6e, 0xab, 0xb3, 0x5d, 0x2d, 0x8b,
								0x7a, 0xff, 0x7e, 0x59, 0xff, 0x62, 0xc4, 0x44, 0xd1, 0x14, 0x9b, 0x52, 0x73, 0xce, 0xf4, 0xed,
								0x07, 0x7c, 0xfa, 0x3e, 0xa4, 0xea, 0x0b, 0x9f, 0xd2, 0x73, 0x75, 0xd6, 0xa1, 0x66, 0x72, 0x4d,
								0x27, 0x69, 0x94, 0x5c, 0xf3, 0x37, 0x3b, 0x3d, 0x07, 0x5f, 0xb1, 0x4a, 0x92, 0x78, 0x10, 0x50,
								0x69, 0x74, 0xff, 0xcf, 0x22, 0x02, 0x66, 0xe9, 0xc1, 0x5e, 0x0c, 0x6d, 0xa5, 0x9c, 0xb7, 0xba,
								0xb3, 0xfe, 0x52, 0x6a, 0x61, 0x2c, 0x28, 0xd3, 0xdb, 0x15, 0xd7, 0x14, 0x11, 0x06, 0x96, 0xf0,
								0x07, 0x45, 0xf5, 0x5a, 0xec, 0x61, 0x6b, 0x6f, 0xe2, 0xdc, 0x6f, 0x85, 0xed, 0xa8, 0x51, 0xf6,
								0x48, 0x7a, 0x3c, 0x91, 0x6e, 0x38, 0x7c, 0xc7, 0x60, 0x70, 0x6c, 0x18, 0x76, 0x1f, 0xf6, 0xc2,
								0xc5, 0x0f, 0xe5, 0xbd, 0x0f, 0x77, 0x8b, 0x85, 0x5c, 0xd3, 0xcc, 0x54, 0x74, 0xe6, 0xea, 0x2c,
								0x82, 0x21, 0xd3, 0xec, 0x6e, 0x37, 0xf1, 0x69, 0x6a, 0x49, 0x90, 0xf2, 0x5e, 0x52, 0x72, 0x75,
								0xc6, 0x64, 0x74, 0xf2, 0x71, 0x7f, 0x83, 0x92, 0xe3, 0x0d, 0x5a, 0x2c, 0x5c, 0x8c, 0xdf, 0x67,
								0x53, 0x74, 0x63, 0x8b, 0xa7, 0x56, 0x5c, 0x72, 0xd1, 0x64, 0x6f, 0xfa, 0xca, 0x98, 0xde, 0xc5,
								0x74, 0x85, 0x21, 0x61, 0x68, 0xce, 0xf0, 0xd8, 0x74, 0xd5, 0xc2, 0xa2, 0xc6, 0x28, 0x13, 0xad,
								0x4b, 0xa9, 0x6a, 0xf8, 0x0e, 0x55, 0x72, 0x1c, 0x66, 0x33, 0x01, 0x11, 0x0e, 0xb8, 0x04, 0xcb,
								0x87, 0x38, 0x1d, 0xb2, 0xbe, 0x5e, 0x4b, 0x53, 0x75, 0x6b, 0x5a, 0x07, 0x45, 0x5b, 0x6b, 0xaf,
								0x12, 0x3d, 0xcc, 0x38, 0x0b, 0x15, 0x94, 0x48, 0x16, 0x67, 0x78, 0x78, 0x13, 0xcb, 0x9b, 0x1c,
								0x3c, 0x6d, 0xe2, 0x38, 0x64, 0x02, 0xd8, 0xfc, 0x56, 0xeb, 0x14, 0x87, 0xda, 0x94, 0x09, 0x7a,
								0x68, 0x02, 0x1a, 0xc2, 0x2a, 0xcd, 0x04, 0xca, 0x34, 0x01, 0xd5, 0x70, 0xf9, 0xa6, 0x70, 0x8a,
								0x7b, 0x62, 0x04, 0x00, 0x20, 0x00, 0x00, 0x27, 0x07, 0x05, 0xfb, 0x09, 0x03, 0x29, 0xcb, 0x3f,
								0xcb, 0x3f, 0x0a, 0x3f, 0x09, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f,
								0x09, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f, 0x09, 0x3f, 0x09, 0x3f, 0x0a, 0x3f, 0x0a, 0x3f, 0x0a, 0x00,
								0x00, 0x03, 0x01, 0x04, 0x07, 0x00, 0x00, 0x25, 0x0e, 0x22, 0x0f, 0x04, 0x12, 0x02, 0x00, 0x10,
								0x05, 0x0e, 0x08, 0x0d, 0x08, 0x0e, 0x07, 0x0e, 0x07, 0x0e, 0x07, 0x0e, 0x07, 0x0e, 0x08, 0x0e,
								0x07, 0x0e, 0x07, 0x0e, 0x08, 0x0e, 0x08, 0x0e, 0x07, 0x0d, 0x08, 0x10, 0x05, 0x22, 0x90, 0x00,
								0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
								0x00, 0x52, 0x0f, 0xc4, 0x0a, 0x2d,
							};
							ServerWebService::sendText(s, (const char*)bin, sizeof(bin));
						}
						else
						{
							// Example request when a language is absent: /0_th/Languages.bin!2B_6YcNcvcYL-muKObP94qcog
							// Example request when graphicsDriver is set to DX11 but the DX11 cache is outdated: /7/EE/Shaders/Particles/LensFlareDistortion.hlsl!18_iw9zmeZqZEKy3SlF8p8O+A

							ObfusString msg("The game is missing some assets required to start. Please verify the language and graphics driver in the launcher match your client_config.json or command line arguments.");
							MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);

							exit(1); // Might as well save the user some time.
						}
						return;
					}
#endif
					const auto route_hash = soup::joaat::hash(urlenc::decode(arr.at(0)));
					switch (route_hash)
					{
					case soup::joaat::compileTimeHash("/"):
						if (arr.size() > 1 && soup::joaat::hash(arr[1].substr(0, 5)) == soup::joaat::compileTimeHash("lang="))
						{
							webui_lang_code = arr[1].substr(5);
						}
						else
						{
							webui_lang_code = lang_code;
						}
#if PRIVATE
						if (std::string html = string::fromFile("OpenWF/index.html"); !html.empty())
						{
							ServerWebService::sendHtml(s, html);
						}
						else
#endif
						{
							uint32_t size;
							const char* data = g_archive.find(soup::joaat::compileTimeHash("OpenWF/index.html"), size);
							ServerWebService::sendHtml(s, data, size);
						}
						break;

					case soup::joaat::compileTimeHash("/dict.js"):
						if (std::string content = string::fromFile(ObfusString("OpenWF/dict.js").str()); !content.empty())
						{
							ServerWebService::sendData(s, ObfusString("text/javascript;charset=utf-8"), content);
						}
						else
						{
							uint32_t size;
							auto data = g_archive.find(soup::joaat::concat(soup::joaat::concat(soup::joaat::compileTimeHash("OpenWF/webui_dicts/"), webui_lang_code), ObfusString(".js").str()), size);
							if (!data)
							{
								data = g_archive.find(soup::joaat::compileTimeHash("OpenWF/webui_dicts/en.js"), size);
							}
							ServerWebService::sendData(s, ObfusString("text/javascript;charset=utf-8"), data, size);
						}
						break;

					case soup::joaat::compileTimeHash("/ping"):
						ServerWebService::sendText(s, ObfusString("pong"));
						break;

					case soup::joaat::compileTimeHash("/save_config"):
						save_config();
						ServerWebService::sendText(s, {});
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

					case soup::joaat::compileTimeHash("/ee_log_in_console"):
						if (arr.size() > 1)
						{
							ee_log_in_console = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(ee_log_in_console));
						break;

					case soup::joaat::compileTimeHash("/dont_resolve_labels"):
						if (arr.size() > 1)
						{
							dont_resolve_labels = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(dont_resolve_labels));
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
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/server_host"):
						if (arr.size() > 1
							&& server_host != arr[1]
							)
						{
							do_logout();
							server_host = arr[1];
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
						ServerWebService::sendText(s, {});
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
						ServerWebService::sendText(s, {});
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
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/status"):
						{
							JsonObject obj;
							populate_initial_status(obj);
							populate_running_scripts(obj);
							populate_full_script_log(obj);
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
							owfConsole::activate(BOOTSTRAPPER_TITLE);
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/scripts"):
						{
							JsonArray arr;
							for (auto& file : std::filesystem::recursive_directory_iterator(ObfusString("OpenWF/scripts").str()))
							{
								if (std::filesystem::is_regular_file(file))
								{
									auto name = string::fixType(file.path().u8string()).substr(15);
									soup::string::replaceAll(name, '\\', '/');
									arr.children.emplace_back(soup::make_unique<JsonString>(std::move(name)));
								}
							}
							ServerWebService::sendText(s, arr.encodePretty());
						}
						break;

					case soup::joaat::compileTimeHash("/start_script"):
						if (!prohibit_scripts)
						{
							start_script_from_file(urlenc::decode(arr[1]));
							ServerWebService::sendText(s, {});
						}
						break;

					case soup::joaat::compileTimeHash("/start_script_inline"):
						if (!prohibit_scripts)
						{
							start_script_from_string(urlenc::decode(arr[1]));
							ServerWebService::sendText(s, {});
						}
						break;

					case soup::joaat::compileTimeHash("/stop_script"):
						{
							std::lock_guard lock(running_scripts_mtx);
							if (auto scr = get_script_by_name(urlenc::decode(arr[1])))
							{
								scr->stop_requested = true;
							}
							ServerWebService::sendText(s, {});
						}
						break;

					case soup::joaat::compileTimeHash("/stop_bgscript"):
						if (bgscript)
						{
							bgscript->stop_requested = true;
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/start_bgscript"):
						if (!bgscript)
						{
							start_bgscript();
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/restart_bgscript"):
						restart_bgscript();
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/autostart_scripts"):
						{
							JsonArray arr;
							for (const auto& name : auto_start_scripts)
							{
								arr.children.emplace_back(soup::make_unique<JsonString>(name));
							}
							ServerWebService::sendText(s, arr.encodePretty());
						}
						break;

					case soup::joaat::compileTimeHash("/add_autostart_script"):
						if (auto name = urlenc::decode(arr[1]); std::find(auto_start_scripts.begin(), auto_start_scripts.end(), name) == auto_start_scripts.end())
						{
							auto_start_scripts.emplace_back(std::move(name));
							save_config();
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/remove_autostart_script"):
						if (auto it = std::find(auto_start_scripts.begin(), auto_start_scripts.end(), urlenc::decode(arr[1])); it != auto_start_scripts.end())
						{
							auto_start_scripts.erase(it);
							save_config();
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/clear_script_log"):
						{
							std::lock_guard lock(script_log_mtx);
							script_log.clear();
						}
						{
							JsonObject obj;
							populate_full_script_log(obj);
							owf_broadcast_message(obj.encode());
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/apply_hotfix"):
						{
							owfArchive archive;
							if (auto hotfix = string::fromFile(ObfusString("OpenWF/hotfix.bin").str()); !hotfix.empty())
							{
								if (!archive.loadHotfix(hotfix.data(), hotfix.size(), soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE)))
								{
									ServerWebService::sendText(s, ObfusString("Failed to apply hotfix as it was made for a different DLL version").str());
									break;
								}
								if (archive.data == g_archive.data)
								{
									ServerWebService::sendText(s, ObfusString("No changes").str());
									break;
								}
								ServerWebService::sendText(s, ObfusString("Hotfix applied").str());
							}
							else
							{
								archive.loadBuiltin();
								if (archive.data == g_archive.data)
								{
									ServerWebService::sendText(s, ObfusString("No changes").str());
									break;
								}
								ServerWebService::sendText(s, ObfusString("Reverting to pre-hotfix state").str());
							}

							{
								std::lock_guard lock(g_archive_mtx);
								g_archive = std::move(archive);
							}

							restart_bgscript();
						}
						break;

					case soup::joaat::compileTimeHash("/version"):
						ServerWebService::sendText(s, ObfusString(BOOTSTRAPPER_TITLE).str());
						break;

#if LABEL_REPLACEMENTS
					case soup::joaat::compileTimeHash("/check_label_replacements"):
						ServerWebService::send204(s);
						break;

					case soup::joaat::compileTimeHash("/reload_label_replacements"):
						load_label_replacements();
						ServerWebService::send204(s);
						break;
#endif

					default:
						{
							bool handled = false;
							std::lock_guard lock(running_scripts_mtx);
							for (auto& scr : running_scripts)
							{
								if (auto route = scr->findCustomRoute(route_hash))
								{
									ServerWebService::sendData(s, route->mime.c_str(), route->content);
									scr->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_SERVED, req.path);
									handled = true;
									break;
								}
							}
							if (!handled && bgscript)
							{
								if (auto route = bgscript->findCustomRoute(route_hash))
								{
									ServerWebService::sendData(s, route->mime.c_str(), route->content);
									bgscript->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_SERVED, req.path);
									handled = true;
								}
							}
							if (!handled)
							{
								ServerWebService::send404(s);
							}
						}
						break;
					}
				});
				srv.on_websocket_connection_established = [](Socket& s, const HttpRequest&, ServerWebService&)
				{
					s.custom_data.addStructToMap(owfWebsocketTag, owfWebsocketTag{});

					JsonObject obj;
					populate_initial_status(obj);
					populate_running_scripts(obj);
					populate_full_script_log(obj);
					ServerWebService::wsSendText(s, obj.encode());
				};
				srv.on_websocket_message = [](WebSocketMessage& msg, Socket& s, ServerWebService&)
				{
					if (joaat::hash(msg.data) == joaat::compileTimeHash("script_log"))
					{
						JsonObject obj;
						populate_full_script_log(obj);
						ServerWebService::wsSendText(s, obj.encode());
					}
				};
				if (serv.bind(61558, &srv))
				{
					serv.run();
				}
				else
				{
					std::cout << ObfusString("Failed to bind TCP/61558. The game will fail to start. Try temporarily closing your web browser if this persists.").str() << std::endl;
				}
			});
			thrd.detach();
		}
	}
	return TRUE;
}

struct owfBroadcastMessageTask final : public Task
{
	const std::string msg;

	owfBroadcastMessageTask(std::string&& msg)
		: msg(std::move(msg))
	{
	}

	void onTick() final
	{
		for (const auto& w : Scheduler::get()->workers)
		{
			if (w->type == soup::WORKER_TYPE_SOCKET
				&& static_cast<Socket*>(w.get())->custom_data.isStructInMap(owfWebsocketTag)
				)
			{
				ServerWebService::wsSendText(*static_cast<Socket*>(w.get()), msg);
			}
		}
		setWorkDone();
	}
};

void owf_broadcast_message(std::string&& msg)
{
	serv.add<owfBroadcastMessageTask>(std::move(msg));
}
