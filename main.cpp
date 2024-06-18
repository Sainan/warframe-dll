#define LOGGING false
#define PRIVATE false

#define DISABLE_XP_BASED_LEVEL_CAPPING true

#include <iostream>

#include <DetourHook.hpp>
#include <json.hpp>
#include <memGuard.hpp>
#include <Module.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <pattern_macros.hpp>
#include <string.hpp>
#include <structing.hpp>
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
static float mission_start_time;

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
	if (seconds == 5.9f)
	{
		seconds = mission_start_time;
	}
	return reinterpret_cast<decltype(&SquadSetCountdownTimer_detour)>(SquadSetCountdownTimer_hook.original)(a1, seconds);
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, PVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hmod);

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
			UniquePtr<JsonNode> config = json::decode(string::fromFile(ObfusString("client_config.json").str()));
			if (!config || !config->isObj())
			{
				config = soup::make_unique<JsonObject>();
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("server_host")); it == config->reinterpretAsObj().end() || !it->second->isStr())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("server_host"), ObfusString("localhost").str());
			}
			server_host = config->reinterpretAsObj().at(ObfusString("server_host")).reinterpretAsStr().value;

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("http_port")); it == config->reinterpretAsObj().end() || !it->second->isInt())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("http_port"), 80);
			}
			http_port = config->reinterpretAsObj().at(ObfusString("http_port")).reinterpretAsInt();

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("https_port")); it == config->reinterpretAsObj().end() || !it->second->isInt())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("https_port"), 443);
			}
			https_port = config->reinterpretAsObj().at(ObfusString("https_port")).reinterpretAsInt();

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_language")); it == config->reinterpretAsObj().end() || !it->second->isStr())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("fallback_language"), ObfusString("en").str());
			}
			fallback_language = config->reinterpretAsObj().at(ObfusString("fallback_language")).reinterpretAsStr().value;

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_graphicsDriver")); it == config->reinterpretAsObj().end() || !it->second->isStr())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("fallback_graphicsDriver"), ObfusString("dx11").str());
			}
			fallback_graphicsDriver = config->reinterpretAsObj().at(ObfusString("fallback_graphicsDriver")).reinterpretAsStr().value;

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_cluster")); it == config->reinterpretAsObj().end() || !it->second->isStr())
			{
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				config->reinterpretAsObj().add(ObfusString("fallback_cluster"), ObfusString("public").str());
			}
			fallback_cluster = config->reinterpretAsObj().at(ObfusString("fallback_cluster")).reinterpretAsStr().value;

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("mission_start_time")); it == config->reinterpretAsObj().end() || !it->second->isFloat())
			{
				std::optional<int64_t> int_value;
				if (it->second->isInt())
				{
					int_value = it->second->reinterpretAsInt().value;
				}
				if (it != config->reinterpretAsObj().end())
				{
					config->reinterpretAsObj().erase(it);
				}
				if (int_value.has_value())
				{
					config->reinterpretAsObj().add(ObfusString("mission_start_time"), static_cast<double>(int_value.value()));
				}
				else
				{
					config->reinterpretAsObj().add(ObfusString("mission_start_time"), 5.9);
				}
			}
			mission_start_time = config->reinterpretAsObj().at(ObfusString("mission_start_time")).reinterpretAsFloat().value;

			string::toFile(ObfusString("client_config.json").str(), config->reinterpretAsObj().encodePretty());
		}

#if !LOGGING
		std::cout << ObfusString("Redirecting requests to ") << server_host << std::endl;
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
	}
	return TRUE;
}
