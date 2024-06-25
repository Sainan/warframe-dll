#define LOGGING false
#define PRIVATE false

#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true

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

static std::string server_host;
static uint16_t http_port;
static uint16_t https_port;
static std::string fallback_language;
static std::string fallback_graphicsDriver;
static std::string fallback_cluster;
static bool skip_mission_start_timer;
static float fov_override;

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
	PAD(0, 0x171) bool got_graphicsDriver;
	PAD(0x171 + 1, 0x178) GameString graphicsDriver;
	PAD(0x178 + sizeof(GameString), 0x194) bool got_language;
	PAD(0x194 + 1, 0x198) GameString language;
	PAD(0x198 + sizeof(GameString), 0x1A8) bool got_cluster;
	PAD(0x1A8 + 1, 0x1B0) GameString cluster;
};
static_assert(sizeof(Arguments) == 0x1B0 + sizeof(GameString));

static DetourHook winhttp_connect_hook;
static int num_winhttp_connect_calls = 0;

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

	if (++num_winhttp_connect_calls == 3)
	{
#if PRIVATE
		if (strcmp(host_1, "origin.warframe.com") == 0)
#endif
		{
			std::cout << ObfusString("The game may fail to start as the server is unresponsive. Retrying.") << std::endl;
		}
	}

	return reinterpret_cast<decltype(&winhttp_connect_detour)>(winhttp_connect_hook.original)(a1, a2, a3, server_host.c_str(), port, nullptr, nullptr);
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
#if DISABLE_XP_BASED_LEVEL_CAPPING
	if (uri.path == ObfusString("/api/inventory.php").str()
		&& disabled_xp_based_level_cap
		)
	{
		uri.query += ObfusString("&xpBasedLevelCapDisabled=1").str();
	}
#endif
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
		*reinterpret_cast<float*>(a1 + 2184) = fov_override;
		return fov_override;
	}
	return *reinterpret_cast<float*>(a1 + 2184);
}

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
	config.add(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
	config.add(ObfusString("fov_override"), fov_override);
	string::toFile(ObfusString("client_config.json").str(), config.encodePretty());
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, PVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hmod);

		if (auto proc = soup::Process::current(); proc->name != "Warframe.x64.exe")
		{
			MessageBoxA(0, "Please only put the dwmapi.dll in your Warframe installation folder.", "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
			return FALSE;
		}

#if true
		AllocConsole();
		SetConsoleTitleA("OpenWF Bootstrapper");
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("skip_mission_start_timer")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				skip_mission_start_timer = it->second->reinterpretAsBool().value;
			}
			else
			{
				skip_mission_start_timer = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fov_override")); it != config->reinterpretAsObj().end() && it->second->isFloat())
			{
				fov_override = it->second->reinterpretAsFloat().value;
			}
			else
			{
				fov_override = 0.0f;
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
			SIG_INST("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 68 0C 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 0C 00 00 44 0F");
			auto winhttp_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "winhttp_connect = " << winhttp_connect << std::endl;
#endif
			if (!winhttp_connect)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
			}
			winhttp_connect_hook.detour = reinterpret_cast<void*>(&winhttp_connect_detour);
			winhttp_connect_hook.target = winhttp_connect;
			winhttp_connect_hook.create();
			winhttp_connect_hook.enable();
		}

		{
			SIG_INST("48 8D 53 18 E8 ? ? ? ? 48 8D 8B");
			auto game_http_request_caller = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "game_http_request_caller = " << game_http_request_caller.as<void*>() << std::endl;
#endif
			if (!game_http_request_caller)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
			}
			auto game_http_request = game_http_request_caller.add(5).rip().as<void*>();
			game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour);
			game_http_request_hook.target = game_http_request;
			game_http_request_hook.create();
			game_http_request_hook.enable();
		}

#if PRIVATE
		{
			SIG_INST("48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 40 48 8B 39");
			auto Curl_resolv = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "Curl_resolv = " << Curl_resolv << std::endl;
#endif
			if (!Curl_resolv)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
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
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
			}
			auto ssl_verify_internal = ssl_verify_internal_caller.add(7).rip().as<void*>();
			ssl_verify_internal_hook.detour = reinterpret_cast<void*>(&ssl_verify_internal_detour);
			ssl_verify_internal_hook.target = ssl_verify_internal;
			ssl_verify_internal_hook.create();
			ssl_verify_internal_hook.enable();
		}

		{
			SIG_INST("40 53 55 56 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 78 4C 8B 31");
			auto Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "Curl_ossl_verifyhost = " << Curl_ossl_verifyhost << std::endl;
#endif
			if (!Curl_ossl_verifyhost)
			{
				ObfusString msg("A mandatory pattern scan has failed. The program will crash now.");
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
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
				MessageBoxA(0, msg.c_str(), "OpenWF Bootstrapper", MB_OK | MB_ICONERROR);
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
				*worldstate_update_interval_insn.add(3).rip().as<uint64_t*>() = 1; // default: 300
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
			SIG_INST("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 50 0F 29 74 24 40 0F 57 C0 0F 28 F1");
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
			SIG_INST("F3 0F 10 81 ? ? ? ? C3 CC CC CC CC CC CC CC 48 8B 81 ? ? ? ? 48 8B 00 C3 CC CC CC CC CC 48 8D 81 ? ? ? ? C3 CC CC CC CC CC CC CC CC 48 8B 81 ? ? ? ? 48 8B 00");
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
					ServerWebService::sendHtml(s, ObfusString(R"EOC(<body style="background:#000;color:#fff;">
	<p>FOV Override (0 = disabled): <input id="fov-override" type="range" min="0" value="0" max="2260000" step="10000"></p>
	<script>
		document.getElementById("fov-override").oninput = function()
		{
			fetch("http://localhost:61558/fov_override?" + this.value);
		}
	</script>
</body>)EOC"));
					break;

				case soup::joaat::compileTimeHash("/ping"):
					ServerWebService::sendText(s, ObfusString("pong"));
					break;

				case soup::joaat::compileTimeHash("/skip_mission_start_timer"):
					if (arr.size() > 1
						&& !prohibit_skip_mission_start_timer
						)
					{
						skip_mission_start_timer = (arr[1].size() == 4);
						save_config();
					}
					ServerWebService::sendText(s, std::to_string(skip_mission_start_timer));
					break;

				case soup::joaat::compileTimeHash("/fov_override"):
					if (arr.size() > 1
						&& !prohibit_fov_override
						)
					{
						fov_override = static_cast<float>(string::toInt<int64_t>(arr[1]).value()) / 10000.0f;
						save_config();
					}
					ServerWebService::sendText(s, std::to_string(fov_override));
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
