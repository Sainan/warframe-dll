#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.6.0"

#define LOGGING false
#define PRIVATE false

#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true
#define PROVIDE_VERSION_INFO true

#include <iostream>

#include <DetourHook.hpp>
#include <HttpRequest.hpp>
#include <joaat.hpp>
#include <json.hpp>
#include <memGuard.hpp>
#include <Module.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <pattern_macros.hpp>
#include <Process.hpp>
#include <Server.hpp>
#include <ServerWebService.hpp>
#include <string.hpp>
#include <structing.hpp>
#include <Thread.hpp>
#include <Uri.hpp>

using namespace soup;

static bool console_attached = false;
static bool disabled_xp_based_level_cap = false;
#if PROVIDE_VERSION_INFO
static const char* build_label = nullptr; // e.g. "2024.12.14.10.37 Retail Windows x64"
static const char* build_hash = nullptr;
#endif

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
	char data[16];
	char* ptr;

	char* getData()
	{
		if (data[15] == -1)
		{
			return ptr;
		}
		return data;
	}

	void setData(const char* new_data)
	{
		ptr = (char*)new_data;
		data[15] = -1;
	}

	void setShortData(const char* new_data)
	{
		memset(data, 0, sizeof(data));
		strncpy(data, new_data, sizeof(data) - 1);
	}
};

struct Arguments
{
	PAD(0, 0x189) bool got_graphicsDriver;
	PAD(0x189 + 1, 0x190) GameString graphicsDriver;
	PAD(0x190 + sizeof(GameString), 0x1AC) bool got_language;
	PAD(0x1AC + 1, 0x1B0) GameString language;
	PAD(0x1B0 + sizeof(GameString), 0x1C0) bool got_cluster;
	PAD(0x1C0 + 1, 0x1C8) GameString cluster;
};
static_assert(offsetof(Arguments, got_graphicsDriver) == 0x189);
static_assert(offsetof(Arguments, graphicsDriver) == 0x190);
static_assert(offsetof(Arguments, got_language) == 0x1AC);
static_assert(offsetof(Arguments, language) == 0x1B0);
static_assert(offsetof(Arguments, got_cluster) == 0x1C0);
static_assert(offsetof(Arguments, cluster) == 0x1C8);


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

static void* winhttp_new_request_detour(void* a1, void* a2, void* a3, char* path, bool a5)
{
#if LOGGING
	std::cout << "winhttp_new_request: path = " << path << std::endl;
#endif
	ObfusString cache_sub("/0/H.Cache.bin!D_---------------------w");
	if (strstr(path, cache_sub.c_str()) != nullptr)
	{
		if (++num_cache_requests == 3)
		{
			std::cout << ObfusString("The game may fail to start as the server is unresponsive. Retrying.") << std::endl;
		}
#if PROVIDE_VERSION_INFO
		if (build_label && strchr(path, '?') == nullptr)
		{
			auto i = strlen(path);
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
	return reinterpret_cast<decltype(&winhttp_new_request_detour)>(winhttp_new_request_hook.original)(a1, a2, a3, path, a5);
}


static DetourHook game_http_request_hook;

static void* game_http_request_detour(void* a1, GameString* url, void* a3)
{
#if LOGGING
	std::cout << "game_http_request for " << (const char*)url->getData() << std::endl;
#else
	if (console_attached)
	{
		console_attached = false;
		const auto conWnd = GetConsoleWindow();
		FreeConsole();
		PostMessage(conWnd, WM_CLOSE, 0, 0);
	}
#endif

	char bak[sizeof(GameString)];
	memcpy(bak, url, sizeof(bak));

	Uri uri((const char*)url->getData());
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
	else if (uri.path == ObfusString("/api/login.php").str()
		|| uri.path.find(ObfusString("/dynamic/worldState.php").str()) != std::string::npos
		)
	{
#if PROVIDE_VERSION_INFO
		if (build_label && build_hash)
		{
			if (!uri.query.empty())
			{
				uri.query.push_back('&');
			}
			uri.query.append(ObfusString("buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			uri.query.append(build_hash);
		}
#endif
	}
	std::string str = uri.toString();
	url->setData(str.c_str());

	const auto ret = reinterpret_cast<decltype(&game_http_request_detour)>(game_http_request_hook.original)(a1, url, a3);

	memcpy(url, bak, sizeof(bak));
	return ret;
}


#if PRIVATE
static DetourHook Curl_resolv_hook;

static void* Curl_resolv_detour(void* a1, const char* hostname, int port, bool allowDOH, void* a5)
{
#if LOGGING
	std::cout << "Curl_resolv for " << hostname << ", port " << port << std::endl;
#endif

	if (server_host != hostname)
	{
		MessageBoxA(0, "HOSTNAME MISMATCH", "HOSTNAME MISMATCH", 0);
	}

	return reinterpret_cast<decltype(&Curl_resolv_detour)>(Curl_resolv_hook.original)(a1, server_host.c_str(), port, allowDOH, a5);
}
#endif


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


static DetourHook parse_arguments_hook;

static void parse_arguments_detour(Arguments* arguments, GameString* str, void* a3)
{
	reinterpret_cast<decltype(&parse_arguments_detour)>(parse_arguments_hook.original)(arguments, str, a3);
	if (!arguments->got_language)
	{
		arguments->got_language = true;
		arguments->language.setShortData(fallback_language.c_str());
	}
	if (!arguments->got_graphicsDriver)
	{
		arguments->got_graphicsDriver = true;
		arguments->graphicsDriver.setShortData(fallback_graphicsDriver.c_str());
	}
	if (!arguments->got_cluster)
	{
		arguments->got_cluster = true;
		arguments->cluster.setShortData(fallback_cluster.c_str());
	}
}


static DetourHook SquadSetCountdownTimer_hook;

static __int64 SquadSetCountdownTimer_detour(void* a1, float seconds)
{
	//std::cout << "SquadSetCountdownTimer(" << seconds << ")" << std::endl;
	if (skip_mission_start_timer && seconds == 5.9f)
	{
		seconds = 0.0f;
	}
	return reinterpret_cast<decltype(&SquadSetCountdownTimer_detour)>(SquadSetCountdownTimer_hook.original)(a1, seconds);
}


static DetourHook PostProcessInfo_getFov_hook;

static float PostProcessInfo_getFov_detour(uintptr_t a1)
{
	if (fov_override != 0.0f)
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


static Thread server_thrd;
static bool prohibit_skip_mission_start_timer = false;
static bool prohibit_fov_override = false;

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
	string::toFile(ObfusString("client_config.json").str(), config.encodePretty());
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

#if true
		AllocConsole();
		SetConsoleTitleA(BOOTSTRAPPER_TITLE);
		{
			FILE* f;
			freopen_s(&f, ObfusString("CONIN$"), ObfusString("r"), stdin);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stderr);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stdout);
		}
		console_attached = true;
#endif

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
		}
		save_config();

#if !LOGGING
		std::cout << ObfusString("Redirecting requests to ") << server_host << std::endl;
#endif

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

#if PRIVATE
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
#endif

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

		server_thrd.start([](Capture&&)
		{
#if ASK_SERVER_FOR_TUNABLES
			{
				HttpRequest hr(server_host, ObfusString("/custom/tunables.json"));
				hr.port = http_port;
				hr.use_tls = false;
				if (auto res = hr.execute())
				{
					if (auto jr = json::decode(res->body); jr && jr->isObj())
					{
						if (jr->reinterpretAsObj().contains(ObfusString("prohibit_skip_mission_start_timer").str()))
						{
							prohibit_skip_mission_start_timer = true;
							skip_mission_start_timer = false;
							std::cout << ObfusString("Note: skip_mission_start_timer is prohibited on this server.") << std::endl;
						}
						if (jr->reinterpretAsObj().contains(ObfusString("prohibit_fov_override").str()))
						{
							prohibit_fov_override = true;
							fov_override = 0.0f;
							std::cout << ObfusString("Note: fov_override is prohibited on this server.") << std::endl;
						}
					}
				}
			}
#endif

			if (!enable_http_interface)
			{
				return;
			}

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
	<p>High Damage Numbers Patch: <input id="high_damage_numbers_patch" type="checkbox" /></p>
	<p>Skip Mission Start Timer: <input id="skip_mission_start_timer" type="checkbox" /></p>
	<p>FOV Override (0 = disabled): <input id="fov_override" type="range" min="0" value="0" max="2260000" step="10000"></p>
	<button id="save_config">Save changes to client_config.json</button>
	<script>
		fetch("http://localhost:61558/high_damage_numbers_patch").then(res => res.text()).then(res => {
			document.getElementById("high_damage_numbers_patch").checked = (res == "1");
		});
		document.getElementById("high_damage_numbers_patch").onchange = function() {
			fetch("http://localhost:61558/high_damage_numbers_patch?" + this.checked);
		};

		fetch("http://localhost:61558/skip_mission_start_timer").then(res => res.text()).then(res => {
			document.getElementById("skip_mission_start_timer").checked = (res == "1");
		});
		document.getElementById("skip_mission_start_timer").onchange = function() {
			fetch("http://localhost:61558/skip_mission_start_timer?" + this.checked);
		};

		fetch("http://localhost:61558/fov_override").then(res => res.text()).then(res => {
			document.getElementById("fov_override").value = parseFloat(res) * 10000;
		});
		document.getElementById("fov_override").oninput = function () {
			fetch("http://localhost:61558/fov_override?" + this.value);
		};

		document.getElementById("save_config").onclick = function () {
			fetch("http://localhost:61558/save_config");
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
					ServerWebService::sendText(s, ObfusString("ok"));
					break;

				case soup::joaat::compileTimeHash("/skip_mission_start_timer"):
					if (arr.size() > 1
						&& !prohibit_skip_mission_start_timer
						)
					{
						skip_mission_start_timer = (arr[1].size() == 4);
					}
					ServerWebService::sendText(s, std::to_string(skip_mission_start_timer));
					break;

				case soup::joaat::compileTimeHash("/fov_override"):
					if (arr.size() > 1
						&& !prohibit_fov_override
						)
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
				}
			});
			if (serv.bind(61558, &srv))
			{
				serv.run();
			}
		});
	}
	return TRUE;
}
