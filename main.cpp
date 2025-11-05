#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.11.15"

#define REDIRECT_REQUESTS true
#define SERVER_IPS_ONLY true
#define ASK_SERVER_FOR_TUNABLES true
#define DISABLE_XP_BASED_LEVEL_CAPPING true
#define PROVIDE_VERSION_INFO true
#define METADATA_PATCHES true

// LOGGING should be true when using this
#define VERBOSE_RNG false
#define VERBOSE_CRC32C false
#define VERBOSE_MD5 false
#define VERBOSE_SERPROPTXT false
#define VERBOSE_OODLE false
#define VERBOSE_SENDCNXLESS false
#define VERBOSE_LZF false
#define VERBOSE_UNCOMPRESSPKT false

// Writes all IRC traffic to EE.log
#define VERBOSE_IRC false

#include "main.hpp"

#include <iostream>
#include <mutex>

#include <CallsiteHook.hpp>
#include <cat.hpp>
#include <CompactDetourHook.hpp>
#include <DetachedScheduler.hpp>
#include <DetourHook.hpp>
#include <FileReader.hpp>
#include <filesystem.hpp>
#include <HttpRequest.hpp>
#include <HttpRequestTask.hpp>
#include <joaat.hpp>
#include <json.hpp>
#include <memGuard.hpp>
#include <Module.hpp>
#include <Mutex.hpp>
#include <ObfusString.hpp>
#include <Pattern.hpp>
#include <pattern_macros.hpp>
#include <Process.hpp>
#include <Regex.hpp>
#include <ReplacementHook.hpp>
#include <Server.hpp>
#include <ServerWebService.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <structing.hpp>
#include <Thread.hpp>
#include <unicode.hpp>
#include <Uri.hpp>
#include <urlenc.hpp>
#include <WebSocketMessage.hpp>

//#include <wininet.h>
//#pragma comment(lib, "wininet")
#include <Lmcons.h> // UNLEN
#pragma comment(lib, "Advapi32.lib") // GetUserNameW

#include "whirlpool.hpp"

#include <lauxlib.h>

#include "modules/ee-notation-parser/EeNotationParser.hpp"

using namespace soup;

#include "owf_config.hpp"
#include "owf_console.hpp"
#include "owf_hotkeys.hpp"
#include "owf_label_replacements.hpp"
#include "owf_luau.hpp"
#include "owf_overlay.hpp"
#include "owf_repo.hpp"
#include "owf_scripting.hpp"
#include "owf_structs.hpp"
#include "owf_tunables.hpp"

const char* g_bootstrapper_title = BOOTSTRAPPER_TITLE;

static uint32_t server_remote_ip_hash = 0;
static bool disabled_xp_based_level_cap = false;
static bool did_auto_login = false;
static bool metadata_patches_in_use = false;

// Exports for Ordis' old Helper.dll:
// ??4CExampleExport@@QEAAAEAV0@$$QEAV0@@Z
// ??4CExampleExport@@QEAAAEAV0@AEBV0@@Z
class __declspec(dllexport) CExampleExport
{
};

static HMODULE og_dwmapi;
static FARPROC og_DwmGetCompositionTimingInfo;
extern "C" __declspec(dllexport) void DwmGetCompositionTimingInfo() { og_DwmGetCompositionTimingInfo(); }

static HMODULE og_wtsapi32;
static FARPROC og_WTSRegisterSessionNotification;
static FARPROC og_WTSUnRegisterSessionNotification;
static FARPROC og_WTSFreeMemory;
static FARPROC og_WTSQuerySessionInformationA;
extern "C" __declspec(dllexport) void WTSRegisterSessionNotification() { og_WTSRegisterSessionNotification(); }
extern "C" __declspec(dllexport) void WTSUnRegisterSessionNotification() { og_WTSUnRegisterSessionNotification(); }
extern "C" __declspec(dllexport) void WTSFreeMemory() { og_WTSFreeMemory(); }
extern "C" __declspec(dllexport) void WTSQuerySessionInformationA() { og_WTSQuerySessionInformationA(); }

using GetFileVersionInfoA_t = BOOL(*)(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData);
//using GetFileInformationByHandle_t = BOOL(*)(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
using GetFileVersionInfoExA_t = BOOL(*)(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData);
using GetFileVersionInfoExW_t = BOOL(*)(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData);
using GetFileVersionInfoSizeA_t = DWORD(*)(LPCSTR lptstrFilename, LPDWORD lpdwHandle);
using GetFileVersionInfoSizeExA_t = DWORD(*)(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle);
using GetFileVersionInfoSizeExW_t = DWORD(*)(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle);
using GetFileVersionInfoSizeW_t = DWORD(*)(LPCWSTR lptstrFilename, LPDWORD lpdwHandle);
using GetFileVersionInfoW_t = BOOL(*)(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData);
using VerFindFileA_t = DWORD(*)(DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir, LPSTR szCurDir, PUINT puCurDirLen, LPSTR szDestDir, PUINT puDestDirLen);
using VerFindFileW_t = DWORD(*)(DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir, LPWSTR szCurDir, PUINT puCurDirLen, LPWSTR szDestDir, PUINT puDestDirLen);
using VerInstallFileA_t = DWORD(*)(DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT puTmpFileLen);
using VerInstallFileW_t = DWORD(*)(DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT puTmpFileLen);
using VerLanguageNameA_t = DWORD(*)(DWORD wLang, LPSTR szLang, DWORD cchLang);
using VerLanguageNameW_t = DWORD(*)(DWORD wLang, LPWSTR szLang, DWORD cchLang);
using VerQueryValueA_t = BOOL(*)(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen);
using VerQueryValueW_t = BOOL(*)(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen);
static HMODULE og_version;
static GetFileVersionInfoA_t og_GetFileVersionInfoA;
//static GetFileInformationByHandle_t og_GetFileInformationByHandle;
static GetFileVersionInfoExA_t og_GetFileVersionInfoExA;
static GetFileVersionInfoExW_t og_GetFileVersionInfoExW;
static GetFileVersionInfoSizeA_t og_GetFileVersionInfoSizeA;
static GetFileVersionInfoSizeExA_t og_GetFileVersionInfoSizeExA;
static GetFileVersionInfoSizeExW_t og_GetFileVersionInfoSizeExW;
static GetFileVersionInfoSizeW_t og_GetFileVersionInfoSizeW;
static GetFileVersionInfoW_t og_GetFileVersionInfoW;
static VerFindFileA_t og_VerFindFileA;
static VerFindFileW_t og_VerFindFileW;
static VerInstallFileA_t og_VerInstallFileA;
static VerInstallFileW_t og_VerInstallFileW;
static VerLanguageNameA_t og_VerLanguageNameA;
static VerLanguageNameW_t og_VerLanguageNameW;
static VerQueryValueA_t og_VerQueryValueA;
static VerQueryValueW_t og_VerQueryValueW;
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
extern "C" __declspec(dllexport) BOOL GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) { return og_GetFileVersionInfoA(lptstrFilename, dwHandle, dwLen, lpData); }
//extern "C" __declspec(dllexport) BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation) { return og_GetFileInformationByHandle(hFile, lpFileInformation); }
extern "C" __declspec(dllexport) BOOL GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) { return og_GetFileVersionInfoExA(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData); }
extern "C" __declspec(dllexport) BOOL GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) { return og_GetFileVersionInfoExW(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData); }
extern "C" __declspec(dllexport) DWORD GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) { return og_GetFileVersionInfoSizeA(lptstrFilename, lpdwHandle); }
extern "C" __declspec(dllexport) DWORD GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle) { return og_GetFileVersionInfoSizeExA(dwFlags, lpwstrFilename, lpdwHandle); }
extern "C" __declspec(dllexport) DWORD GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle) { return og_GetFileVersionInfoSizeExW(dwFlags, lpwstrFilename, lpdwHandle); }
extern "C" __declspec(dllexport) DWORD GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) { return og_GetFileVersionInfoSizeW(lptstrFilename, lpdwHandle); }
extern "C" __declspec(dllexport) BOOL GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) { return og_GetFileVersionInfoW(lptstrFilename, dwHandle, dwLen, lpData); }
extern "C" __declspec(dllexport) DWORD VerFindFileA(DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir, LPSTR szCurDir, PUINT puCurDirLen, LPSTR szDestDir, PUINT puDestDirLen) { return og_VerFindFileA(uFlags, szFileName, szWinDir, szAppDir, szCurDir, puCurDirLen, szDestDir, puDestDirLen); }
extern "C" __declspec(dllexport) DWORD VerFindFileW(DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir, LPWSTR szCurDir, PUINT puCurDirLen, LPWSTR szDestDir, PUINT puDestDirLen) { return og_VerFindFileW(uFlags, szFileName, szWinDir, szAppDir, szCurDir, puCurDirLen, szDestDir, puDestDirLen); }
extern "C" __declspec(dllexport) DWORD VerInstallFileA(DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT puTmpFileLen) { return og_VerInstallFileA(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, puTmpFileLen); }
extern "C" __declspec(dllexport) DWORD VerInstallFileW(DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT puTmpFileLen) { return og_VerInstallFileW(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, puTmpFileLen); }
extern "C" __declspec(dllexport) DWORD VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang) { return og_VerLanguageNameA(wLang, szLang, cchLang); }
extern "C" __declspec(dllexport) DWORD VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang) { return og_VerLanguageNameW(wLang, szLang, cchLang); }
extern "C" __declspec(dllexport) BOOL VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen) { return og_VerQueryValueA(pBlock, lpSubBlock, lplpBuffer, puLen); }
extern "C" __declspec(dllexport) BOOL VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen) { return og_VerQueryValueW(pBlock, lpSubBlock, lplpBuffer, puLen); }


// Cache tunables for faster access
static bool prohibit_skip_mission_start_timer = false;
static bool prohibit_freecam = false;
static bool prohibit_scripts = false;


static void save_config()
{
	JsonObject config;

	config.add(ObfusString("fallback_language"), fallback_language);
	config.add(ObfusString("fallback_languageVO"), fallback_languageVO);
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
	config.add(ObfusString("logout_on_request_failure"), logout_on_request_failure);
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
	config.add(ObfusString("alternative_loading"), alternative_loading);
	config.add(ObfusString("dont_resolve_labels"), dont_resolve_labels);
	config.add(ObfusString("save_all_metadata"), save_all_metadata);
	config.add(ObfusString("write_all_metadata_reads_to_console"), write_all_metadata_reads_to_console);
	config.add(ObfusString("write_all_metadata_reads_to_ee_log"), write_all_metadata_reads_to_ee_log);
	config.add(ObfusString("write_patched_metadata_reads_to_console"), write_patched_metadata_reads_to_console);
	config.add(ObfusString("write_patched_metadata_reads_to_ee_log"), write_patched_metadata_reads_to_ee_log);
	config.add(ObfusString("client_http_port"), client_http_port);

	string::toFile(ObfusString("OpenWF/Client Config.json").str(), config.encodePretty());
}


static std::string get_core_string_utf8(std::string key)
{
	if (auto e = g_core_dict.find(key); e != g_core_dict.end())
	{
		return e->second;
	}
#if PRIVATE
	if (g_core_dict.empty())
	{
		key.append(" (could not be resolved because core dict is not initialised yet)");
	}
#endif
	return key;
}

static std::wstring get_core_string(std::string key)
{
	return soup::unicode::utf8_to_utf16(get_core_string_utf8(key));
}


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
	ObfusString localhost("127.0.0.1");
	*reinterpret_cast<HINTERNET*>(a1 + 104) = InternetConnectA(
		*reinterpret_cast<HINTERNET*>(a1 + 96),
		localhost.c_str(),
		client_http_port,
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

	ObfusString localhost("127.0.0.1");
	host_1 = localhost.c_str();
	port = client_http_port;

	return reinterpret_cast<decltype(&winhttp_connect_detour)>(winhttp_connect_hook.original)(a1, a2, a3, host_1, port, nullptr, nullptr);
}


static CompactDetourHook game_http_request_hook;

struct GameHttpRequest
{
	/* 0x00 */ GameString url;
	char pad[0x28];
	/* 0x38 */ GameString body;
};
static_assert(offsetof(GameHttpRequest, body) == 0x38);

struct LegacyGameHttpRequest
{
	/* 0x00 */ LegacyGameString url;
	char pad[0x28];
	/* 0x48 */ LegacyGameString body;
};
static_assert(offsetof(LegacyGameHttpRequest, body) == 0x48);

struct GameHttpRequestU18
{
	/* 0x00 */ LegacyGameStringU18 url;
	char pad[0x30];
	/* 0x48 */ LegacyGameStringU18 body;
};
static_assert(offsetof(GameHttpRequestU18, body) == 0x48);

struct GameHttpRequestU8
{
	/* 0x00 */ LegacyGameStringU18 url;
	char pad[0x18];
	/* 0x30 */ LegacyGameStringU18 body;
};
static_assert(offsetof(GameHttpRequestU8, body) == 0x30);

static bool can_use_server_host()
{
	if (server_remote_ip_hash) // Connecting to a server outside of the localnet?
	{
		std::lock_guard lock(g_client_tunables_mtx);
		bool blacklisted = g_client_tunables.isStringInArray(joaat::compileTimeHash("ipbl"), server_remote_ip_hash);
		if (g_client_tunables.getInt(joaat::compileTimeHash("invipbl")))
		{
			blacklisted = !blacklisted;
		}
		if (blacklisted)
		{
			return false;
		}
		if (
			auth_query.empty() // Not currently logged in?
			&& g_repo.timestamp + g_client_tunables.getInt(joaat::compileTimeHash("remote_allowed_days")) * 86400 < time::unixSeconds() // Current build is too old?
			)
		{
			return false; // To prevent downgrade attacks, disallow this remote connection.
		}
	}
	return true;
}

static void process_game_http_request(soup::Uri& uri, const char*& body_data, size_t& body_size, std::string& body_buf, bool strip_tls)
{
#if REDIRECT_REQUESTS
	uri.host = can_use_server_host() ? server_host : ObfusString("127.0.0.1").str();
	if (strip_tls)
	{
		uri.scheme = ObfusString("http").str();
		uri.port = http_port;
	}
	else
	{
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
		if (auto jr = json::decode(body_data, body_size); jr && jr->isObj())
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
				body_data = body_buf.data();
				body_size = body_buf.size();
			}
		}
#if PROVIDE_VERSION_INFO
		if (build_label[0])
		{
			if (!uri.query.empty())
			{
				uri.query.push_back('&');
			}
			uri.query.append(ObfusString("buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			if (build_hash[0])
			{
				uri.query.append(build_hash, 22);
			}
		}
		uri.query.append(ObfusString("&clientMod=").str());
		uri.query.append(urlenc::encode(ObfusString(BOOTSTRAPPER_TITLE).str()));
		if (metadata_patches_in_use)
		{
			uri.query.append(ObfusString("&metadataPatchesInUse=1").str());
		}
#endif
		{
			std::lock_guard lock(g_server_tunables_mtx);
			if (auto e = g_server_tunables.strings.find(soup::joaat::compileTimeHash("token")); e != g_server_tunables.strings.end())
			{
				uri.query.append(ObfusString("&token=").str());
				uri.query.append(e->second);
			}
		}
	}
	else if (uri.path == ObfusString("/api/inbox.php").str())
	{
		auth_query = uri.query;
	}
	else if (uri.path.find(ObfusString("/worldState.php").str()) != std::string::npos)
	{
#if PROVIDE_VERSION_INFO
		if (build_label[0])
		{
			if (!uri.query.empty())
			{
				uri.query.push_back('&');
			}
			uri.query.append(ObfusString("buildLabel=").str());
			uri.query.append(build_label, 16);
			uri.query.push_back('/');
			if (build_hash[0])
			{
				uri.query.append(build_hash, 22);
			}
		}
#endif
	}
	else if (uri.path == ObfusString("/api/logout.php").str())
	{
		owfOverlay::setPrelogin(true);
		auth_query.clear();
	}
#if true // PS can be relatively sensitive data but is often shared alongside server logs.
	if (auto jr = json::decode(body_data, body_size); jr && jr->isObj())
	{
		if (auto it = jr->reinterpretAsObj().findIt(ObfusString("PS").str()); it != jr->reinterpretAsObj().end() && it->second->isStr())
		{
			ObfusString msg("W0RFXVN0ZXZlIGxpa2VzIGJpZyBidXR0cw");
			if (auto sep = it->second->reinterpretAsStr().value.find(';'); sep != std::string::npos)
			{
				it->second->reinterpretAsStr().value.erase(sep + 1);
				it->second->reinterpretAsStr().value.append(msg.str());
			}
			else
			{
				it->second->reinterpretAsStr().value = std::move(msg.str());
			}
			body_buf = jr->encode();
			body_data = body_buf.data();
			body_size = body_buf.size();
		}
	}
#endif
#else
	if (uri.path == "/api/heartbeat.php")
	{
		MessageBoxA(0, "Anti-cheat has been triggered. The game will be put down.", BOOTSTRAPPER_TITLE, 0);
		exit(1);
	}
#endif
}

template <typename T, bool strip_tls = false>
static void* game_http_request_detour(void* a1, T* request, void* a3)
{
#if LOGGING
	std::cout << "game_http_request for " << (const char*)request->url.getData() << std::endl;
	/*if (request->body.getSize() != 0)
	{
		std::cout << request->body.getData() << std::endl;
	}*/
#endif

	Uri uri((const char*)request->url.getData());
	const char* body_data = request->body.getData();
	size_t body_size = request->body.getSize();
	std::string body_buf;
	process_game_http_request(uri, body_data, body_size, body_buf, strip_tls);
	std::string url_buf = uri.toString();
	request->url.setUnownedData(url_buf.data(), url_buf.size());
	if (body_data != request->body.getData())
	{
		request->body.setUnownedData(body_data, body_size);
	}

	const auto ret = reinterpret_cast<decltype(&game_http_request_detour<T>)>(game_http_request_hook.original)(a1, request, a3);

#if LOGGING
	// This now contains the response
	/*if (request->body.getSize() != 0)
	{
		std::cout << request->body.getData() << std::endl;
	}*/
#endif

	return ret;	
}


static DetourHook encstr_append_hook;
static EncryptedString::AppendData* last_enc_str = nullptr;
static std::string dec_buf;

static void encstr_append_detour(EncryptedString::AppendData* a1, int a2)
{
	//std::cout << "encstr_append: " << (void*)a1 << ", " << std::string(a1->data, a1->size) << std::endl;
	if (last_enc_str != a1)
	{
		last_enc_str = a1;
		dec_buf.clear();
	}
	dec_buf.append(a1->data, a1->size);
	return reinterpret_cast<decltype(&encstr_append_detour)>(encstr_append_hook.original)(a1, a2);
}

static DetourHook encstr_discharge_hook;
//static std::atomic<size_t> leaked_memory = 0;

using string_resize_t = void(*)(GameString*, size_t);
static string_resize_t string_resize;

static void encstr_discharge_detour(EncryptedString* a1, GameString* out)
{
	//std::cout << "encstr_discharge: " << (void*)a1->app << std::endl;
#if REDIRECT_REQUESTS
	if (string_resize)
	{
		string_resize(out, dec_buf.size());
		memcpy(out->getData(), dec_buf.data(), dec_buf.size());
	}
	else
	{
		// If we don't have string_resize, we'll need to be a bit more stupid.
		std::lock_guard lock(label_replacements_mtx);
		auto ps = fossilise_string(dec_buf.data(), dec_buf.size());
		out->setUnownedData(ps->data, ps->size);
		//auto data = soup::malloc(dec_buf.size());
		//memcpy(data, dec_buf.data(), dec_buf.size());
		//out->setUnownedData((const char*)data, dec_buf.size());
		//leaked_memory += dec_buf.size();
	}
#else
	reinterpret_cast<decltype(&encstr_discharge_detour)>(encstr_discharge_hook.original)(a1, out);
#endif

	dec_buf.clear();
}


/*static DetourHook queue_http_request_internal_hook;

// called multiple times if flags=6
static void queue_http_request_internal_detour(void* a1, GameString* url, GameString* body, const char* encoding, void* callback_data, int flags)
{
	std::cout << "queue_http_request_internal: url=" << url->getData() << ", encoding=" << (encoding ? encoding : "NULL") << std::endl;
	if (encoding)
	{
		std::cout << "Decrypted body: " << dec_buf << std::endl;
	}
	reinterpret_cast<decltype(&queue_http_request_internal_detour)>(queue_http_request_internal_hook.original)(a1, url, body, encoding, callback_data, flags);
	dec_buf.clear();
}*/


#if PRIVATE
static DetourHook Curl_resolv_hook;

static void* Curl_resolv_detour(void* a1, const char* hostname, int port, bool allowDOH, void* a5)
{
#if LOGGING
	std::cout << "Curl_resolv for " << hostname << ", port " << port << std::endl;
#endif

	ObfusString localhost("127.0.0.1");
	if (can_use_server_host()
		? server_host != hostname
		: localhost.str() != hostname
		)
	{
		MessageBoxA(0, "HOSTNAME MISMATCH", "HOSTNAME MISMATCH", 0);
	}

	return reinterpret_cast<decltype(&Curl_resolv_detour)>(Curl_resolv_hook.original)(a1, can_use_server_host() ? server_host.c_str() : localhost.c_str(), port, allowDOH, a5);
}
#endif


static ReplacementHook ssl_verify_internal_hook;

static int64_t ssl_verify_internal_detour(void* a1, void* a2)
{
	//std::cout << "ssl_verify_internal called" << std::endl;
	return 1; // "Verify success"
}


static ReplacementHook Curl_ossl_verifyhost_hook;

static int64_t Curl_ossl_verifyhost_detour(void* a1, void* a2)
{
	//std::cout << "Curl_ossl_verifyhost_detour called" << std::endl;
	/*auto ret = reinterpret_cast<decltype(&Curl_ossl_verifyhost_detour)>(Curl_ossl_verifyhost_hook.original)(a1, a2);
	std::cout << "Curl_ossl_verifyhost returned " << ret << std::endl;*/
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


struct FireAndForgetMessageBoxData
{
	std::string msg;
	UINT type;
};

static void fire_and_forget_messagebox(std::string msg, UINT type)
{
	soup::Thread t([](Capture&& _cap)
	{
		auto& cap = _cap.get<FireAndForgetMessageBoxData>();
		MessageBoxA(0, cap.msg.c_str(), BOOTSTRAPPER_TITLE, cap.type);
	}, FireAndForgetMessageBoxData{ std::move(msg), type });
	t.detach();
}

void memoise_server_tunables()
{
	prohibit_skip_mission_start_timer = g_server_tunables.getBool(joaat::compileTimeHash("prohibit_skip_mission_start_timer"));
	prohibit_freecam = g_server_tunables.getBool(joaat::compileTimeHash("prohibit_freecam"));
	prohibit_scripts = g_server_tunables.getBool(joaat::compileTimeHash("prohibit_scripts"));
}

#if ASK_SERVER_FOR_TUNABLES
struct owfTunablesTask : public soup::Task
{
	HttpRequestTask hrt;

	owfTunablesTask()
		: hrt(HttpRequest(server_host + ":" + std::to_string(https_port), ObfusString("/custom/tunables.json?clientMod=" BOOTSTRAPPER_TITLE).str()), &Socket::certchain_validator_none)
	{
		hrt.hr.use_tls = true;
	}

	void onTick() final
	{
		if (hrt.tickUntilDone())
		{
			bool ok = false;
			if (hrt.result)
			{
#if !SERVER_IPS_ONLY
				if (hrt.sock)
				{
					server_host = hrt.sock->peer.ip.toString();
					server_remote_ip_hash = hrt.sock->peer.ip.isLocalnet() ? 0 : soup::joaat::hash(server_host);
#if false
					std::cout << "server_host = " << server_host << std::endl;
					std::cout << "server_remote_ip_hash = " << server_remote_ip_hash << std::endl;
					std::cout << "can_use_server_host = " << can_use_server_host() << std::endl;
#endif
				}
#endif

				if (hrt.result->status_code == 200)
				{
					std::lock_guard lock(g_server_tunables_mtx);
					ok = g_server_tunables.load(hrt.result->body.data(), hrt.result->body.size());
				}
			}

			{
				std::lock_guard lock(g_server_tunables_mtx);
				memoise_server_tunables();
			}

			if (!ok)
			{
				std::lock_guard lock(g_client_tunables_mtx);
				ok = g_client_tunables.getInt(joaat::hash("silent_tunables_error"));
			}

			if (!ok)
			{
				// Would print this to console but there's no guarantee it's still open at this point or will stay open for long enough.
				auto msg = ObfusString("Failed to verify that the server at ").str();
				msg.append(hrt.hr.getHost());
				msg.append(ObfusString(" is online and running compatible software. Login may fail.").str());
				fire_and_forget_messagebox(std::move(msg), MB_OK | MB_ICONWARNING);
			}

			owfOverlay::redraw();

			setWorkDone();
		}
	}
};
#endif

static DetachedScheduler task_runner;

static void on_got_server_host()
{
#if SERVER_IPS_ONLY
	IpAddr server_ip;
	if (!server_ip.fromString(server_host))
	{
		server_ip = SOUP_IPV4_NWE(127, 0, 0, 1);
	}
	server_host = server_ip.toString();
	server_remote_ip_hash = server_ip.isLocalnet() ? 0 : soup::joaat::hash(server_host);
#else
	string::lower(server_host);
	if (server_host.find(ObfusString("warframe.com").str()) != std::string::npos)
	{
		server_host = ObfusString("127.0.0.1").str();
	}
	else
	{
		while (server_host.c_str()[server_host.size()] == '.')
		{
			server_host.pop_back();
		}
		std::lock_guard lock(g_client_tunables_mtx);
		if (g_client_tunables.isStringInArray(joaat::compileTimeHash("hnbl"), joaat::hash(server_host)))
		{
			server_host = ObfusString("127.0.0.1").str();
		}
	}
#endif

	{
		auto msg = get_core_string_utf8(ObfusString("gotsh").str());
		soup::string::replaceAll(msg, ObfusString("|HOST|").str(), server_host);
		std::wcout << soup::unicode::utf8_to_utf16(msg) << std::endl;
	}
	if (autologin && !did_auto_login)
	{
		std::wcout << get_core_string(ObfusString("alpend")) << std::endl;
	}

#if ASK_SERVER_FOR_TUNABLES
	task_runner.add<owfTunablesTask>();
#endif
}

static void do_logout()
{
	if (!auth_query.empty())
	{
		HttpRequest hr(server_host + ":" + std::to_string(https_port), ObfusString("/api/logout.php?").str() + auth_query);
		hr.use_tls = true;
		SOUP_UNUSED(hr.execute(&Socket::certchain_validator_none));
		auth_query.clear();

		owfOverlay::setPrelogin(true);
	}
}


static DetourHook parse_arguments_hook;
static bool processed_args = false;
static uint64_t* device_id_ptr = nullptr;

static std::string process_args_str(const char* str)
{
	std::string arguments_to_inject;
	if (!processed_args)
	{
		processed_args = true;
		bool got_language = false;
		bool got_languageVO = false;
		bool got_graphicsDriver = false;
		bool got_cluster = false;
		for (const auto& arg : string::explode<std::string>(str, ' '))
		{
			if (arg.size() > 15 && arg.substr(0, 15) == ObfusString("-owfServerHost:").str())
			{
				server_host = arg.substr(15);
			}
			else if (arg.size() > 10 && arg.substr(0, 10) == ObfusString("-language:").str())
			{
				got_language = true;
			}
			else if (arg.size() > 12 && arg.substr(0, 12) == ObfusString("-languageVO:").str())
			{
				got_languageVO = true;
			}
			else if (arg.size() > 16 && arg.substr(0, 16) == ObfusString("-graphicsDriver:").str())
			{
				got_graphicsDriver = true;
			}
			else if (arg.size() > 9 && arg.substr(0, 9) == ObfusString("-cluster:").str())
			{
				got_cluster = true;
			}
		}
		on_got_server_host();

		if (!got_language && !fallback_language.empty())
		{
			arguments_to_inject.append(ObfusString("-language:").str());
			arguments_to_inject.append(fallback_language);
			arguments_to_inject.push_back(' ');
		}
		if (game_version >= GV(39, 0, 0) && !got_languageVO && !fallback_languageVO.empty())
		{
			arguments_to_inject.append(ObfusString("-languageVO:").str());
			arguments_to_inject.append(fallback_languageVO);
			arguments_to_inject.push_back(' ');
		}
		if (!got_graphicsDriver && !fallback_graphicsDriver.empty())
		{
			arguments_to_inject.append(ObfusString("-graphicsDriver:").str());
			arguments_to_inject.append(fallback_graphicsDriver);
			arguments_to_inject.push_back(' ');
		}
		if (!got_cluster)
		{
			arguments_to_inject.append(ObfusString("-cluster:").str());
			arguments_to_inject.append(fallback_cluster);
			arguments_to_inject.push_back(' ');
		}

		// This prevents the game from modifying H.Misc.cache by pre-populating the "device id".
		// It needs to be done here because DllMain runs before static initialisers.
		if (device_id_ptr)
		{
			//std::cout << "The device id is a crispy obfuscated 0 aka. " << *device_id_ptr << std::endl;

			wchar_t name[MAX_COMPUTERNAME_LENGTH > UNLEN ? MAX_COMPUTERNAME_LENGTH + 1 : UNLEN + 1];

			DWORD size = sizeof(name) / sizeof(wchar_t);
			GetComputerNameW(name, &size);
			uint32_t computer_name_hash = soup::joaat::hashRange((const char*)name, size * sizeof(wchar_t));

			size = sizeof(name) / sizeof(wchar_t);
			GetUserNameW(name, &size);
			uint32_t user_name_hash = soup::joaat::hashRange((const char*)name, size * sizeof(wchar_t));

			*device_id_ptr = (static_cast<uint64_t>(computer_name_hash) << 32) | user_name_hash;
			// Because the device_id is an obfuscated int, the observed "date" will differ across game versions.
		}
	}
#if LOGGING
	if (!arguments_to_inject.empty())
	{
		std::cout << "parse_arguments (injected): " << arguments_to_inject << std::endl;
	}
	std::cout << "parse_arguments: " << str << std::endl;
#endif
	return arguments_to_inject;
}

template <typename Str/*, bool has_languageVO, bool has_graphicsDriver*/>
static void parse_arguments_detour(uintptr_t arguments, Str* str, void* a3)
{
	if (auto arguments_to_inject = process_args_str(str->getData()); !arguments_to_inject.empty())
	{
		Str tmp;
		tmp.setUnownedData(arguments_to_inject.data(), arguments_to_inject.size());
		reinterpret_cast<decltype(&parse_arguments_detour<Str/*, has_languageVO, has_graphicsDriver*/>)>(parse_arguments_hook.original)(arguments, &tmp, a3);
	}

	reinterpret_cast<decltype(&parse_arguments_detour<Str/*, has_languageVO, has_graphicsDriver*/>)>(parse_arguments_hook.original)(arguments, str, a3);
}


/*static DetourHook SquadSetCountdownTimer_hook;

static __int64 SquadSetCountdownTimer_detour(void* a1, float seconds)
{
	//std::cout << "SquadSetCountdownTimer(" << seconds << ")" << std::endl;
	if (skip_mission_start_timer && !prohibit_skip_mission_start_timer && seconds == 5.9f)
	{
		seconds = 0.0f;
	}
	return reinterpret_cast<decltype(&SquadSetCountdownTimer_detour)>(SquadSetCountdownTimer_hook.original)(a1, seconds);
}*/

static luau_CFunction lua_SquadSetCountdownTimer_og;

static int lua_SquadSetCountdownTimer_detour(luau_State* L)
{
	if (skip_mission_start_timer && !prohibit_skip_mission_start_timer && L->intop[1].type == LUAU_NUMBER && L->intop[1].value.as_float == 5.9f)
	{
		L->intop[1].value.as_float = 0.0f;
	}
	return lua_SquadSetCountdownTimer_og(L);
}


static float last_dmg = 0.0f;

static float get_dmg_to_display(int dmg_int)
{
	if (!high_damage_numbers_patch)
	{
		return static_cast<float>(dmg_int);
	}

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


static DetourHook init_cache_fetching_hook;

static void init_cache_fetching_detour(void* a1, bool a2, bool is_stripped, bool a4, bool a5, bool a6, uint8_t a7)
{
	is_stripped = false;
	reinterpret_cast<decltype(&init_cache_fetching_detour)>(init_cache_fetching_hook.original)(a1, a2, is_stripped, a4, a5, a6, a7);
}


static DetourHook legacy_dns_lookup_hook;

static bool should_block_dns_lookup(const char* data, size_t size)
{
#if LOGGING
	std::cout << "legacy_dns_lookup: " << data << std::endl;
#endif

	std::lock_guard lock(g_client_tunables_mtx);
	return g_client_tunables.isStringInArray(joaat::compileTimeHash("dns"), joaat::hashRange(data, size));
}

template <typename T>
static bool legacy_dns_lookup_detour(void* out, T* name, bool a3)
{
	if (should_block_dns_lookup(name->getData(), name->getSize()))
	{
		name->setUnownedData(server_host.data(), server_host.size());
	}
	return reinterpret_cast<decltype(&legacy_dns_lookup_detour<T>)>(legacy_dns_lookup_hook.original)(out, name, a3);
}


static DetourHook write_to_log_file_hook;
static void* write_to_log_file_a1 = nullptr;
static ObfusString log_sep("]: ");

static void write_to_log_file_detour(void* const a1, char* const data, size_t _size)
{
	write_to_log_file_a1 = a1;
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

				case soup::joaat::compileTimeHash("Cache mani"): // "Cache manifest hash "
					if (size == 43)
					{
						memcpy(build_hash, message + 20, 22);
					}
					break;

				case soup::joaat::compileTimeHash("Cache lang"): // "Cache languages enabled: _xx"
					if (size == 28)
					{
						lang_code = std::string(message + 26, 2);
					}
					break;

				case soup::joaat::compileTimeHash("Using lang"): // "Using language: _xx"
					if (size == 19)
					{
						lang_code = std::string(message + 17, 2);
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
							std::lock_guard lock(g_client_tunables_mtx);
							active_input_filter_allows_hotkeys = !g_client_tunables.isStringInArray(joaat::compileTimeHash("nhkif"), joaat::hash(active_input_filter));
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

static void write_to_ee_log(const char* data, size_t size)
{
	if (write_to_log_file_a1)
	{
		reinterpret_cast<decltype(&write_to_log_file_detour)>(write_to_log_file_hook.original)(write_to_log_file_a1, const_cast<char*>(data), size);
	}
}

static void write_to_ee_log(const char* str)
{
	return write_to_ee_log(str, strlen(str));
}


static luau_CFunction lua_FlashMgr_GetConfigBool_og;

static int lua_FlashMgr_GetConfigBool_detour(luau_State* L)
{
	SOUP_IF_LIKELY (L->intop[1].type == LUAU_STRING)
	{
		if (autologin && !did_auto_login)
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

		if (alternative_loading)
		{
			ObfusString str("Server.FastLoad");
			if (strcmp(L->intop[1].getString(), str.c_str()) == 0)
			{
#if LOGGING
				std::cout << "Reporting Server.FastLoad as true" << std::endl;
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
#if LOGGING
	std::cout << "lua_set_global: " << name;
#endif
#if true
	switch (soup::joaat::hash(name))
	{
	case soup::joaat::compileTimeHash("gRegion"):
		regionmgr = L->outtop[-1].type == LUAU_USERDATA ? static_cast<RegionMgr*>(L->outtop[-1].getObject()) : nullptr;
#if LOGGING
		std::cout << " = " << regionmgr;
#endif
		if (!owfOverlay::isInited())
		{
			owfOverlay::init();
		}
		break;

	case soup::joaat::compileTimeHash("gFlashMgr"):
		flashmgr = L->outtop[-1].type == LUAU_USERDATA ? L->outtop[-1].getObject() : nullptr;
#if LOGGING
		std::cout << " = " << flashmgr;
#endif
		break;

	case soup::joaat::compileTimeHash("gGameData"):
		gamedata = L->outtop[-1].type == LUAU_USERDATA ? L->outtop[-1].getObject() : nullptr;
#if LOGGING
		std::cout << " = " << gamedata;
#endif
		break;

	case soup::joaat::compileTimeHash("gPlayerProfileMgr"):
		profilemgr = L->outtop[-1].type == LUAU_USERDATA ? L->outtop[-1].getObject() : nullptr;
#if LOGGING
		std::cout << " = " << profilemgr;
#endif
		break;

	case soup::joaat::compileTimeHash("gClient"):
		gClient = L->outtop[-1].type == LUAU_USERDATA ? L->outtop[-1].getObject() : nullptr;
#if LOGGING
		std::cout << " = " << gClient;
#endif
		break;

	case soup::joaat::compileTimeHash("gMatchingService"):
		matchingservice = L->outtop[-1].type == LUAU_USERDATA ? *(void**)(L->outtop[-1].value.as_uintptr + 0x18) : nullptr;
#if LOGGING
		std::cout << " = " << matchingservice;
#endif
		break;
	}
#endif
#if LOGGING
	std::cout << std::endl;
#endif
	return reinterpret_cast<decltype(&lua_set_global_detour)>(lua_set_global_hook.original)(L, name);
}


static JsonArray get_available_scripts()
{
	JsonArray arr;
	for (auto& file : std::filesystem::recursive_directory_iterator(ObfusString("OpenWF/Scripts").str()))
	{
		if (std::filesystem::is_regular_file(file))
		{
			auto name = string::fixType(file.path().u8string()).substr(15);
			soup::string::replaceAll(name, '\\', '/');
			arr.children.emplace_back(soup::make_unique<JsonString>(std::move(name)));
		}
	}
	return arr;
}

static void populate_autostart_scripts(JsonObject& obj)
{
	auto arr = soup::make_unique<JsonArray>();
	for (const auto& name : auto_start_scripts)
	{
		arr->children.emplace_back(soup::make_unique<JsonString>(name));
	}
	obj.add(ObfusString("autostart_scripts"), std::move(arr));
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
	auto scr = new owfScript();
	bool ok = scr->loadFile(std::move(path));
	std::lock_guard lock(running_scripts_mtx);
	if (ok)
	{
		running_scripts.emplace_back(scr);
	}
	broadcast_running_scripts_locked();
}

static void start_script_from_string(const std::string& code)
{
	auto scr = new owfScript();
	bool ok = scr->loadString(code, code);
	std::lock_guard lock(running_scripts_mtx);
	if (ok)
	{
		running_scripts.emplace_back(scr);
	}
	broadcast_running_scripts_locked();
}

static luau_CFunction lua_LotusHudStatus_UpdateFlashMarkers_og;

using raise_script_error_t = bool(*)(const char** err);
static raise_script_error_t* raise_script_error_fp = nullptr;

static int lua_LotusHudStatus_UpdateFlashMarkers_detour(luau_State* L)
{
#if true
	const auto og_outtop = luau_savestack(L, L->outtop);
	const auto og_intop = luau_savestack(L, L->intop);
	const auto og_lngjmp = L->global_state_error_longjump_data();
	const auto og_panic = L->global_state_panic_func();
	raise_script_error_t og_raise;

	luau_L = L;
	if (game_version >= GV(37, 0, 0)) // These offsets are very likely wrong for 35.5.0 and below
	{
		L->global_state_error_longjump_data() = nullptr;
		L->global_state_panic_func() = [](luau_State* L, int)
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
	}
	if (raise_script_error_fp)
	{
		og_raise = *raise_script_error_fp;
		*raise_script_error_fp = [](const char** err) -> bool
		{
#if LOGGING
			std::cout << "raise_script_error called" << std::endl;
#endif
			luau_error_msg = *err;
#if LOGGING
			std::cout << luau_error_msg << std::endl;
#endif
			throw 0;
		};
	}

	{
		std::lock_guard mtx(running_scripts_mtx);
		if (active_input_filter_allows_hotkeys && !prohibit_scripts)
		{
			if (DWORD pid; GetWindowThreadProcessId(GetForegroundWindow(), &pid), pid == GetCurrentProcessId())
			{
				if (hotkeys_mtx.tryLock())
				{
					for (auto& hk : hotkeys)
					{
						if (hk.wasJustPressed())
						{
							start_script_from_string(hk.script);
						}
					}
					hotkeys_mtx.unlock();
				}
			}
		}
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
				delete &**i; static_assert(std::is_same_v<decltype(&**i), owfScript*>);
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
	if (luau_savestack(L, L->outtop) != og_outtop)
	{
		owfScript::logNl("Not all values were popped from LuaU stack");
	}
#endif
	if (game_version >= GV(37, 0, 0))
	{
		L->outtop = luau_restorestack(L, og_outtop);
		L->intop = luau_restorestack(L, og_intop);
		L->global_state_error_longjump_data() = og_lngjmp;
		L->global_state_panic_func() = og_panic;
	}
	if (raise_script_error_fp)
	{
		*raise_script_error_fp = og_raise;
	}
	luau_L = nullptr;
#endif

	return lua_LotusHudStatus_UpdateFlashMarkers_og(L);
}


static ReplacementHook get_profile_dir_hook;
static uint32_t get_profile_dir_offset;

static GameString* get_profile_dir_detour(uintptr_t a1)
{
	auto str = reinterpret_cast<GameString*>(a1 + get_profile_dir_offset);
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


static luau_CFunction lua_OpenWebBrowser_og;

static int lua_OpenWebBrowser_detour(luau_State* L)
{
#if LOGGING
	std::cout << "lua_OpenWebBrowser: " << L->intop[0].getString() << std::endl;
#endif
	ObfusString sub("warframe.com");
	if (strstr(L->intop[0].getString(), sub.c_str()) == nullptr)
	{
		return lua_OpenWebBrowser_og(L);
	}
	return 0;
}


static luau_CFunction lua_FlashInstance_GetStringVariable_og;

static int lua_FlashInstance_GetStringVariable_detour(luau_State* L)
{
	auto ret = lua_FlashInstance_GetStringVariable_og(L);
	//std::cout << "lua_FlashInstance_GetStringVariable: " << L->intop[1].getString() << " -> " << L->outtop[-1].getString() << std::endl;
	if (soup::joaat::hash(L->intop[1].getString()) == soup::joaat::compileTimeHash("Window.SendMessageBar.MessageBox"))
	{
		bool block = false;
		{
			const std::string current_draft = L->outtop[-1].getString();

			std::lock_guard lock(running_scripts_mtx);
			for (auto& scr : running_scripts)
			{
				if (auto pBlock = scr->findChatSendSubscription(current_draft))
				{
					block |= *pBlock;
					if (L->intop[-3].type == LUAU_NIL) // Heuristic to determine if the message was just submitted
					{
						scr->events.emplace_back(OWF_EVT_SUBMIT_CHAT_MESSAGE, (uint32_t)*pBlock, current_draft);
					}
				}
			}
		}

		if (block && luau_pushstring)
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
					ObfusString name("mPanelList");
					luau_pushstring(L, name.c_str());
					if (luau_gettable(L, i - 1) > 0)
					{
						ChatRedux_table = L->outtop[i - 1].value.as_uintptr;
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
static void load_label_replacements()
{
	const auto path = ObfusString("OpenWF/Label Replacements.cat.txt").str();

	if (!std::filesystem::exists(path))
	{
		string::toFile(path, ObfusString("/Menu/ProjectName: Warframe [OpenWF]").str());
	}

	std::lock_guard lock(label_replacements_mtx);
	label_replacements.clear();
	FileReader fr(path);
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

static CompactDetourHook check_string_substitutions_hook;
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


#if METADATA_PATCHES
struct MetadataPatch
{
	std::string prefix;
	std::vector<std::pair<std::string, std::string>> replacements;
	std::vector<std::pair<soup::Regex, std::string>> substitutions;
	std::vector<std::pair<std::string, std::string>> query_assignments;

	std::string final_data;
	bool is_implicit = false;
	bool applied = false;
};
static Mutex metadata_patches_mtx;
static std::unordered_map<uint32_t, MetadataPatch> metadata_patches;
static MetadataPatch* current_patch = nullptr;
static void load_metadata_patches()
{
	std::lock_guard lock(metadata_patches_mtx);
	metadata_patches.clear();

	auto L = luaL_newstate();
	owfScript::openLibs(L);

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		size_t len;
		const char* str = luaL_checklstring(L, 1, &len);
		const auto hash = soup::joaat::hashRange(str, len);
		if (auto e = metadata_patches.find(hash); e != metadata_patches.end())
		{
			current_patch = &e->second;
		}
		else
		{
			current_patch = &metadata_patches.emplace(hash, MetadataPatch{}).first->second;
		}
		current_patch->prefix.append(pluto_checkstring(L, 2));
		metadata_patches_in_use = true;
		return 0;
	});
	{ ObfusString name("new_patch"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (current_patch)
		{
			current_patch->replacements.emplace_back(pluto_checkstring(L, 1), pluto_checkstring(L, 2));
		}
		return 0;
	});
	{ ObfusString name("add_replacement"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		int pushed = 0;
		if (current_patch)
		{
			try
			{
				current_patch->substitutions.emplace_back(soup::Regex(pluto_checkstring(L, 1), luaL_checkstring(L, 3)), pluto_checkstring(L, 2));
			}
			catch (const std::exception& e)
			{
				lua_pushstring(L, e.what()); ++pushed;
			}
		}
		return pushed;
	});
	{ ObfusString name("add_substitution"); lua_setglobal(L, name.c_str()); }

	lua_pushcfunction(L, [](lua_State* L) -> int
	{
		if (current_patch)
		{
			current_patch->query_assignments.emplace_back(pluto_checkstring(L, 1), pluto_checkstring(L, 2));
		}
		return 0;
	});
	{ ObfusString name("add_query_assignment"); lua_setglobal(L, name.c_str()); }

	size_t size;
	auto data = g_repo.find(soup::joaat::compileTimeHash("OpenWF/helpers/load_metadata_patches.pluto"), size);
	if (luaL_loadbuffer(L, data, size, nullptr) != LUA_OK
		|| lua_pcall(L, 0, 0, 0) != LUA_OK
		)
	{
		owfScript::logNl(lua_type(L, -1) == LUA_TSTRING ? pluto_checkstring(L, -1) : ObfusString("Non-string script error").str());
	}

	lua_close(L);
}

static CallsiteHook object_type_serialise_propery_text_hook;
static void object_type_serialise_propery_text_detour(void* a1, GameString* str, int a3, char a4)
{
	ObjectType* objectType;
	__asm mov objectType, r11;

	const char* path = resolve_string_handle(objectType->getPathHandle());
	const char* name = resolve_string_handle(objectType->name_handle);

	uint32_t hash = 0;
	hash = joaat::partialStr(path, hash);
	hash = joaat::partialStr(name, hash);
	joaat::finalise(hash);

	bool should_write_to_console = write_all_metadata_reads_to_console;
	bool should_write_to_ee_log = write_all_metadata_reads_to_ee_log;

	std::lock_guard lock(metadata_patches_mtx);
	if (auto e = metadata_patches.find(hash); e != metadata_patches.end())
	{
		auto& patch = e->second;
		auto& buf = patch.final_data;
		buf.clear();
		buf.reserve(patch.prefix.size() + str->getSize());
		buf.append(patch.prefix);
		if (patch.replacements.empty() && patch.substitutions.empty() && patch.query_assignments.empty())
		{
			buf.append(str->getData(), str->getSize());
		}
		else
		{
			std::string text(str->getData(), str->getSize());
			for (const auto& replacement : patch.replacements)
			{
				string::replaceAll(text, replacement.first, replacement.second);
			}
			for (const auto& substitution : patch.substitutions)
			{
				text = substitution.first.substituteAll(text, substitution.second);
			}
			if (!patch.query_assignments.empty())
			{
				try
				{
					EeNotationParser par;
					auto jr = par.parse(text);
					for (const auto& qa : patch.query_assignments)
					{
						if (auto n = jr->query(qa.first.c_str()))
						{
							if (n->isStr())
							{
								n->reinterpretAsStr().value = qa.second;
							}
							else if (n->isInt())
							{
								n->reinterpretAsInt().value = std::stod(qa.second);
							}
							else
							{
								n->asFloat().value = soup::string::toIntOpt<int64_t>(qa.second).value();
							}
						}
					}
					text = EeNotationParser::unparse(*jr);
				}
				catch (const std::exception& e)
				{
					std::cout << ObfusString("[Metadata Patches] Error applying query assignment: ").str() << e.what() << std::endl;
				}
			}
			buf.append(text);
		}
		str->setUnownedData(buf.data(), buf.size());

		if (!patch.is_implicit)
		{
			should_write_to_console = write_patched_metadata_reads_to_console;
			should_write_to_ee_log = write_patched_metadata_reads_to_ee_log;
		}
		patch.applied = true;
	}
	else if (save_all_metadata)
	{
		metadata_patches.emplace(hash, MetadataPatch{
			.final_data = std::string(str->getData(), str->getSize()),
			.is_implicit = true,
			.applied = true,
		});
	}

	if (should_write_to_console)
	{
		ObfusString prefix("Reading metadata for ");
		std::cout.write(prefix.data(), prefix.size());
		std::cout << path << name << "\n";
	}
	if (should_write_to_ee_log)
	{
		ObfusString prefix("[OpenWF] Reading metadata for ");
		write_to_ee_log(prefix.data(), prefix.size());
		write_to_ee_log(path);
		write_to_ee_log(name);
		write_to_ee_log("\n", 1);
	}

	return reinterpret_cast<decltype(&object_type_serialise_propery_text_detour)>(object_type_serialise_propery_text_hook.original)(a1, str, a3, a4);
}
#endif


#if false
static DetourHook ScriptMgr_startInstance_hook;

static bool ScriptMgr_startInstance_detour(void* _this, ScriptInstance* inst/*, void* a3, void* a4*/)
{
	if (inst->script_type)
	{
		const char* path = resolve_string_handle(inst->script_type->getPathHandle());
		const char* name = resolve_string_handle(inst->script_type->name_handle);
		const char* func_name = resolve_string_handle(inst->func_name_handle);

#if LOGGING
		//std::cout << "ScriptMgr_startInstance: " << path << name << ", " << func_name << "\n";
#endif

		uint32_t hash = 0;
		hash = joaat::partialStr(path, hash);
		hash = joaat::partialStr(name, hash);
		hash = joaat::partialStr(func_name, hash);
		joaat::finalise(hash);

		bool block = false;
		{
			std::lock_guard lock(running_scripts_mtx);
			for (auto& scr : running_scripts)
			{
				if (auto e = scr->findSubscribedScriptTrigger(hash))
				{
					block |= *e;
					std::string data = path;
					data.append(name);
					data.push_back(':');
					data.append(func_name);
					scr->events.emplace_back(OWF_EVT_SCRIPT_TRIGGERED, std::move(data));
				}
			}
		}
		SOUP_IF_UNLIKELY (block)
		{
			return false;
		}
	}

	return reinterpret_cast<decltype(&ScriptMgr_startInstance_detour)>(ScriptMgr_startInstance_hook.original)(_this, inst/*, a3, a4*/);
}
#endif


static DetourHook irc_send_raw_hook;

static void irc_send_raw_detour(void* a1, GameString* str, bool bLogIt)
{
#if VERBOSE_IRC
	bLogIt = true;
#endif
#if LOGGING
	std::cout << "irc_send_raw: " << std::string(str->getData(), str->getSize()) << std::endl;
#endif
#if REDIRECT_REQUESTS
	if (str->getSize() > 36 && soup::joaat::hashRange(str->getData(), 4) == soup::joaat::compileTimeHash("NICK")) // NICK & USER are sent in the same message
	{
		std::string buf(str->getData(), str->getSize() - 40); // Copy everything except the 'realname' part
		auto arr = string::explode(auth_query, '&');
		if (arr.size() > 1)
		{
			buf.append(arr[1]);
		}
		GameString tmp;
		tmp.setUnownedData(buf.data(), buf.size());
		return reinterpret_cast<decltype(&irc_send_raw_detour)>(irc_send_raw_hook.original)(a1, &tmp, bLogIt);
	}
#endif
	if (str->getSize() > 10 && soup::joaat::hashRange(str->getData(), 8) == soup::joaat::compileTimeHash("PRIVMSG "))
	{
		std::string_view sv(str->getData(), str->getSize());
		const auto sep = sv.find(ObfusString(" :").str());
		if (sep != std::string::npos)
		{
			std::string_view message(str->getData() + sep + 2, str->getSize() - (sep + 2));
			//std::cout << "channel_name = " << sv.substr(8, sep - 8) << std::endl;
			//std::cout << "message = " << message << std::endl;
			{
				std::lock_guard lock(running_scripts_mtx);
				for (auto& scr : running_scripts)
				{
					if (scr->isSubscribedToOutgoingMessage(message))
					{
						scr->events.emplace_back(OWF_EVT_OUTGOING_CHAT_MESSAGE, std::string(str->getData() + 8, str->getSize() - 8));
					}
				}
			}
		}
	}
	return reinterpret_cast<decltype(&irc_send_raw_detour)>(irc_send_raw_hook.original)(a1, str, bLogIt);
}


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


#if VERBOSE_MD5
static DetourHook MD5_append_hook;

static void MD5_append_detour(void* state, const char* data, size_t size)
{
	std::cout << "MD5_append: state = " << state << ", data = " << string::bin2hex(std::string(data, size)) << ", caller offset = " << Pointer(_ReturnAddress()).sub(Module(nullptr).range.base.as<uintptr_t>()).as<void*>() << std::endl;
	return reinterpret_cast<decltype(&MD5_append_detour)>(MD5_append_hook.original)(state, data, size);
}
#endif


#if VERBOSE_SERPROPTXT
static DetourHook serialise_propery_text_hook;

static void serialise_propery_text_detour(void* a1, GameString* str, int a3, char a4)
{
	std::cout << "serialise_propery_text: a3 = " << a3 << ", a4 = " << a4 << ", caller offset = " << Pointer(_ReturnAddress()).sub(Module(nullptr).range.base.as<uintptr_t>()).as<void*>() << std::endl;
	std::cout.write(str->getData(), str->getSize());
	return reinterpret_cast<decltype(&serialise_propery_text_detour)>(serialise_propery_text_hook.original)(a1, str, a3, a4);
}
#endif


#if VERBOSE_OODLE
struct GameOodleNetworkState
{
	PAD(0, 0x38) uint32_t htbits;
	/* 0x40 */ GameBuffer compacted_state;
	/* 0x50 */ GameBuffer window;
	/* 0x60 */ GameBuffer state;
	/* 0x70 */ GameBuffer shared;
};
static_assert(offsetof(GameOodleNetworkState, htbits) == 0x38);
static_assert(offsetof(GameOodleNetworkState, compacted_state) == 0x40);
static_assert(offsetof(GameOodleNetworkState, window) == 0x50);
static_assert(offsetof(GameOodleNetworkState, state) == 0x60);
static_assert(offsetof(GameOodleNetworkState, shared) == 0x70);

static DetourHook init_oodle_network_state_hook;

static __int64 init_oodle_network_state_detour(GameOodleNetworkState* st)
{
	std::cout << "compacted state size: " << st->compacted_state.size << " (allocated " << st->compacted_state.capacity << ")" << std::endl;
	string::toFile("net_compacted_state.bin", st->compacted_state.data, st->compacted_state.size);
	const auto ret = reinterpret_cast<decltype(&init_oodle_network_state_detour)>(init_oodle_network_state_hook.original)(st);
	std::cout << "htbits: " << st->htbits << std::endl;
	std::cout << "window size: " << st->window.size << " (allocated " << st->window.capacity << ")" << std::endl;
	string::toFile("net_window.bin", st->window.data, st->window.size);
	return ret;
}

static DetourHook compress_packet_oodle_net_hook;

static bool compress_packet_oodle_net_detour(GameBuffer* uncompressed, GameBuffer* compressed, GameOodleNetworkState* st)
{
	std::cout << "compress_packet_oodle_net: " << std::string(uncompressed->data, uncompressed->size) << std::endl;
	return reinterpret_cast<decltype(&compress_packet_oodle_net_detour)>(compress_packet_oodle_net_hook.original)(uncompressed, compressed, st);
}

static DetourHook compress_packet_oodle_lz_hook;

static bool compress_packet_oodle_lz_detour(GameBuffer* uncompressed, GameBuffer* compressed, int a3)
{
	std::cout << "compress_packet_oodle_lz: " << std::string(uncompressed->data, uncompressed->size) << std::endl;
	return reinterpret_cast<decltype(&compress_packet_oodle_lz_detour)>(compress_packet_oodle_lz_hook.original)(uncompressed, compressed, a3);
}

static DetourHook oodle_compress_hook;

static bool oodle_compress_detour(char* out, size_t* out_size, const char* data, size_t size, int a5)
{
	std::cout << "oodle_compress: " << std::string(data, size) << std::endl;
	return reinterpret_cast<decltype(&oodle_compress_detour)>(oodle_compress_hook.original)(out, out_size, data, size, a5);
}
#endif


#if VERBOSE_SENDCNXLESS
static DetourHook SendConnectionlessData_hook;

static __int64 SendConnectionlessData_detour(void* a1, GameBuffer* data, void* a3, char a4, char a5)
{
	std::cout << "SendConnectionlessData: " << string::bin2hex(data->data, data->size) << std::endl;
	return reinterpret_cast<decltype(&SendConnectionlessData_detour)>(SendConnectionlessData_hook.original)(a1, data, a3, a4, a5);
}
#endif


#if VERBOSE_LZF
static DetourHook lzf_compress_hook;

static unsigned int lzf_compress_detour(const char* uncompressed, unsigned int uncompressed_size, char* compressed, unsigned int compressed_size)
{
	std::cout << "lzf_compress input: " << string::bin2hex(uncompressed, uncompressed_size) << std::endl;
	compressed_size = reinterpret_cast<decltype(&lzf_compress_detour)>(lzf_compress_hook.original)(uncompressed, uncompressed_size, compressed, compressed_size);
	std::cout << "lzf_compress output: " << string::bin2hex(compressed, compressed_size) << std::endl;
	return compressed_size;
}

static DetourHook lzf_decompress_hook;

static unsigned int lzf_decompress_detour(const char* compressed, unsigned int compressed_size, char* uncompressed, unsigned int uncompressed_size)
{
	std::cout << "lzf_decompress input: " << string::bin2hex(compressed, compressed_size) << std::endl;
	uncompressed_size = reinterpret_cast<decltype(&lzf_decompress_detour)>(lzf_decompress_hook.original)(compressed, compressed_size, uncompressed, uncompressed_size);
	std::cout << "lzf_decompress output: " << string::bin2hex(uncompressed, uncompressed_size) << std::endl;
	return uncompressed_size;
}
#endif


#if VERBOSE_UNCOMPRESSPKT
struct PacketData
{
	PAD(0x00, 0x08) void* GameOodleNetworkState;
	PAD(0x10, 0x18) GameBuffer uncompressed;
};
static_assert(offsetof(PacketData, uncompressed) == 0x18);

static DetourHook UncompressPacket_hook;

static bool UncompressPacket_detour(PacketData* data, GameBuffer* buffer)
{
	std::cout << "UncompressPacket input: " << string::bin2hex(buffer->data, buffer->size) << std::endl;
	if (reinterpret_cast<decltype(&UncompressPacket_detour)>(UncompressPacket_hook.original)(data, buffer))
	{
		std::cout << "UncompressPacket output: " << string::bin2hex(buffer->data, buffer->size) << std::endl;
		return true;
	}
	return false;
}
#endif


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

// Called before core dict is initialised!
static bool check_ec(const std::error_code& ec)
{
	if (ec)
	{
		ObfusString msg("Filesystem error. It's likely your anti-virus is interfering; please ensure the game folder is excluded from it.");
		MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
		return false;
	}
	return true;
}

static void log_optional_scan_failure(bool important)
{
	if (important)
	{
		std::wcout << get_core_string(ObfusString("sigfailimp").str()) << std::endl;
	}
	else
	{
		std::wcout << get_core_string(ObfusString("sigfailopt").str()) << std::endl;
	}
}

static void report_critical_failure(std::wstring msg)
{
	int ndlls = 0;
	if (std::filesystem::is_regular_file(ObfusString("wtsapi32.dll").str())) ++ndlls;
	if (std::filesystem::is_regular_file(ObfusString("dwmapi.dll").str())) ++ndlls;
	if (std::filesystem::is_regular_file(ObfusString("version.dll").str())) ++ndlls;
	if (ndlls > 1)
	{
		msg.push_back(' ');
		msg.append(get_core_string(ObfusString("appmdll").str()));
	}

	auto title = soup::unicode::utf8_to_utf16(BOOTSTRAPPER_TITLE);
	MessageBoxW(0, msg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

static Server serv;

struct owfWebsocketTag
{
	static inline uint32_t last_id = 0;

	uint32_t id;
};

struct owfContentTask : public Task
{
	SharedPtr<Worker> s;
	HttpRequestTask hrt;

	owfContentTask(Socket& _s, HttpRequest&& hr)
		: s(Scheduler::get()->getShared(_s)), hrt(std::move(hr), &Socket::certchain_validator_none)
	{
		ServerWebService::setKeepAlive(_s, true);
	}

	void onTick()
	{
		if (static_cast<Socket*>(s.get())->isWorkDoneOrClosed())
		{
#if LOGGING
			std::cout << "owfContentTask: client socket is gone, aborting" << std::endl;
#endif
			setWorkDone();
		}
		else if (hrt.tickUntilDone())
		{
			if (hrt.result.has_value() && hrt.result->status_code == 200)
			{
#if LOGGING
				std::cout << "owfContentTask: 200" << std::endl;
#endif
				ServerWebService::sendContent(*static_cast<Socket*>(s.get()), std::move(*hrt.result));
			}
			else
			{
#if LOGGING
				std::cout << "owfContentTask: 404" << std::endl;
#endif
				if (!owfOverlay::isInited())
				{
					if (hrt.hr.path.find(ObfusString("/0/B.Cache.Windows_").str()) != std::string::npos)
					{
						if (hrt.hr.path.substr(19, 2) == ObfusString("xx").str())
						{
							auto msg = ObfusString("The Windows_xx cache is missing or outdated.").str();
							/*msg.append(ObfusString("\r\n\r\nTroubleshooting:").str());
							msg.append(ObfusString("\r\n- Verify game files. It is expected that the launcher deletes the Bootstrapper DLL so run the Download Latest DLL script afterwards.").str());*/
							MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
						}
						else
						{
							auto msg = ObfusString("The language that the game was supposed to launch with (").str();
							msg.append(hrt.hr.path.substr(19, 2));
							msg.append(ObfusString(") is missing or outdated.").str());
							/*msg.append(ObfusString("\r\n\r\nTroubleshooting:").str());
							msg.append(ObfusString("\r\n- Verify client config. It can be found in the OpenWF folder.").str());
							msg.append(ObfusString("\r\n- Verify launcher settings. It is expected that the launcher deletes the Bootstrapper DLL so run the Download Latest DLL script afterwards.").str());
							msg.append(ObfusString("\r\n- Verify command line arguments. If in use, they may overwrite the client config.").str());*/
							MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
						}
						exit(1);
					}
					if (hrt.hr.path.find(ObfusString("/0/B.Cache.Dx").str()) != std::string::npos)
					{
						auto msg = ObfusString("The graphicsDriver that the game was supposed to launch with (dx").str();
						msg.append(hrt.hr.path.substr(13, 2));
						msg.append(ObfusString(") is missing or outdated.").str());
						/*msg.append(ObfusString("\r\n\r\nTroubleshooting:").str());
						msg.append(ObfusString("\r\n- Verify client config. It can be found in the OpenWF folder.").str());
						msg.append(ObfusString("\r\n- Verify launcher settings. It is expected that the launcher deletes the Bootstrapper DLL so run the Download Latest DLL script afterwards.").str());
						msg.append(ObfusString("\r\n- Verify command line arguments. If in use, they may overwrite the client config.").str());*/
						MessageBoxA(0, msg.c_str(), BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);

						exit(1);
					}
				}
				ServerWebService::send404(*static_cast<Socket*>(s.get()));
			}
			setWorkDone();
		}
	}
};

static void start_bgscript()
{
	std::string code;
#if PRIVATE
	code = string::fromFile("OpenWF/bgscript.pluto");
	if (code.empty())
#endif
	{
		size_t size;
		auto data = g_repo.find(soup::joaat::compileTimeHash("OpenWF/bgscript.pluto"), size);
		code = std::string(data, size);
	}

	std::lock_guard lock(running_scripts_mtx);
	bgscript = new owfScript();
	bgscript->loadString(ObfusString("OpenWF Background Script"), std::move(code));
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

static void populate_full_script_log(JsonObject& obj)
{
	std::lock_guard lock(script_log_mtx);
	obj.add(ObfusString("script_log"), script_log);
	obj.add(ObfusString("script_log_len"), static_cast<int64_t>(script_log.size()));
}

static void populate_full_status(JsonObject& obj)
{
	obj.add(ObfusString("server_host"), server_host);

	obj.add(ObfusString("high_damage_numbers_patch"), high_damage_numbers_patch);
	obj.add(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
	obj.add(ObfusString("simulacrum_blacklisted"), simulacrum_blacklisted);
	obj.add(ObfusString("simulacrum_whitelisted"), simulacrum_whitelisted);
	obj.add(ObfusString("pause_always_stops_time"), pause_always_stops_time);
	obj.add(ObfusString("alternative_loading"), alternative_loading);
	obj.add(ObfusString("ee_log_in_console"), ee_log_in_console);
	obj.add(ObfusString("dont_resolve_labels"), dont_resolve_labels);

	obj.add(ObfusString("fov_override"), fov_override);

	obj.add(ObfusString("console"), owfConsole::active);

	obj.add(ObfusString("available_scripts"), soup::make_unique<JsonArray>(get_available_scripts()));
	populate_running_scripts(obj);
	populate_autostart_scripts(obj);
	populate_full_script_log(obj);
}

template <typename T>
static void owf_broadcast_value(std::string name, T value)
{
	JsonObject obj;
	obj.add(std::move(name), value);
	owf_broadcast_message(obj.encode());
}

bool owf_command(const std::string& in, JsonObject& out)
{
	auto args = string::explode(in, '?');
	SOUP_IF_UNLIKELY (args.empty())
	{
		return false;
	}
	switch (joaat::hash(args[0]))
	{
	case joaat::compileTimeHash("logout"):
		do_logout();
		return true;

	case joaat::compileTimeHash("save_config"):
		save_config();
		return true;

	case joaat::compileTimeHash("reload_hotkeys"):
		load_hotkeys();
		return true;

#if LABEL_REPLACEMENTS
	case joaat::compileTimeHash("reload_label_replacements"):
		load_label_replacements();
		return true;
#endif

#if METADATA_PATCHES
	case joaat::compileTimeHash("reload_metadata_patches"): // Unused and undocumented for now because most types are never gonna be reloaded by the game.
		load_metadata_patches();
		return true;
#endif

	case joaat::compileTimeHash("available_scripts"):
		out.add(ObfusString("available_scripts"), soup::make_unique<JsonArray>(get_available_scripts()));
		return true;

	case joaat::compileTimeHash("running_scripts"):
		populate_running_scripts(out);
		return true;

	case joaat::compileTimeHash("autostart_scripts"):
		populate_autostart_scripts(out);
		return true;

	case joaat::compileTimeHash("script_log"):
		populate_full_script_log(out);
		return true;

	case joaat::compileTimeHash("clear_script_log"):
		{
			std::lock_guard lock(script_log_mtx);
			script_log.clear();
		}
		{
			JsonObject obj;
			populate_full_script_log(obj);
			owf_broadcast_message(obj.encode());
		}
		return true;

	case joaat::compileTimeHash("stop_script"):
		{
			std::lock_guard lock(running_scripts_mtx);
			if (auto scr = get_script_by_name(args.at(1)))
			{
				scr->stop_requested = true;
			}
		}
		return true;

	case soup::joaat::compileTimeHash("high_damage_numbers_patch"):
		if (args.size() > 1)
		{
			high_damage_numbers_patch = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("high_damage_numbers_patch"), high_damage_numbers_patch);
		}
		else
		{
			out.add(ObfusString("high_damage_numbers_patch"), high_damage_numbers_patch);
		}
		return true;

	case soup::joaat::compileTimeHash("skip_mission_start_timer"):
		if (args.size() > 1)
		{
			skip_mission_start_timer = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
		}
		else
		{
			out.add(ObfusString("skip_mission_start_timer"), skip_mission_start_timer);
		}
		return true;

	case soup::joaat::compileTimeHash("simulacrum_blacklisted"):
		if (args.size() > 1)
		{
			simulacrum_blacklisted = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("simulacrum_blacklisted"), simulacrum_blacklisted);
		}
		else
		{
			out.add(ObfusString("simulacrum_blacklisted"), simulacrum_blacklisted);
		}
		return true;

	case soup::joaat::compileTimeHash("simulacrum_whitelisted"):
		if (args.size() > 1)
		{
			simulacrum_whitelisted = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("simulacrum_whitelisted"), simulacrum_whitelisted);
		}
		else
		{
			out.add(ObfusString("simulacrum_whitelisted"), simulacrum_whitelisted);
		}
		return true;

	case soup::joaat::compileTimeHash("alternative_loading"):
		if (args.size() > 1)
		{
			alternative_loading = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("alternative_loading"), alternative_loading);
		}
		else
		{
			out.add(ObfusString("alternative_loading"), alternative_loading);
		}
		return true;

	case soup::joaat::compileTimeHash("ee_log_in_console"):
		if (args.size() > 1)
		{
			ee_log_in_console = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("ee_log_in_console"), ee_log_in_console);
		}
		else
		{
			out.add(ObfusString("ee_log_in_console"), ee_log_in_console);
		}
		return true;

	case soup::joaat::compileTimeHash("dont_resolve_labels"):
		if (args.size() > 1)
		{
			dont_resolve_labels = (args[1].size() == 4);
			owf_broadcast_value(ObfusString("dont_resolve_labels"), dont_resolve_labels);
		}
		else
		{
			out.add(ObfusString("dont_resolve_labels"), dont_resolve_labels);
		}
		return true;

	case soup::joaat::compileTimeHash("fov_override"):
		if (args.size() > 1)
		{
			fov_override = static_cast<float>(string::toIntOpt<int64_t>(args[1]).value()) / 10000.0f;
			owf_broadcast_value(ObfusString("fov_override"), fov_override);
		}
		else
		{
			out.add(ObfusString("fov_override"), fov_override);
		}
		break;
	}
	return false;
}

static soup::Pattern hash_to_pattern(uint32_t hash)
{
	char data[23];
	data[0] = soup::string::charset_hex[(hash >> 4) & 0xf];
	data[1] = soup::string::charset_hex[(hash >> 0) & 0xf];
	data[2] = ' ';
	data[3] = soup::string::charset_hex[(hash >> 12) & 0xf];
	data[4] = soup::string::charset_hex[(hash >> 8) & 0xf];
	data[5] = ' ';
	data[6] = soup::string::charset_hex[(hash >> 20) & 0xf];
	data[7] = soup::string::charset_hex[(hash >> 16) & 0xf];
	data[8] = ' ';
	data[9] = soup::string::charset_hex[(hash >> 28) & 0xf];
	data[10] = soup::string::charset_hex[(hash >> 24) & 0xf];
	data[11] = ' ';
	data[12] = '0';
	data[13] = '0';
	data[14] = ' ';
	data[15] = '0';
	data[16] = '0';
	data[17] = ' ';
	data[18] = '0';
	data[19] = '0';
	data[20] = ' ';
	data[21] = '0';
	data[22] = '0';
	return Pattern(data, sizeof(data));
}

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, PVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		if (auto proc = soup::Process::current(); proc->name != "Warframe.x64.exe")
		{
			MessageBoxA(0, "Please don't keep the Bootstrapper DLL (wtsapi32.dll, dwmapi.dll, or version.dll) in the same folder as any executable other than Warframe.x64.exe.", BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			return FALSE;
		}

		if (!std::filesystem::exists("Warframe.x64.exe"))
		{
			MessageBoxA(0, "Launched with incorrect working directory; it must be the folder where Warframe.x64.exe is.", BOOTSTRAPPER_TITLE, MB_OK | MB_ICONERROR);
			return FALSE;
		}

		owfConsole::setTitle(BOOTSTRAPPER_TITLE);
		owfConsole::activate();

#if LOGGING
		std::cout << "base address = " << soup::Process::current()->open()->range.base.as<void*>() << std::endl;
#endif

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
			og_WTSFreeMemory = GetProcAddress(og_wtsapi32, "WTSFreeMemory");
			og_WTSQuerySessionInformationA = GetProcAddress(og_wtsapi32, "WTSQuerySessionInformationA");
		}

		{
			std::wstring path(_wgetenv(L"windir"));
			path.append(LR"(\System32\version.dll)");
			og_version = LoadLibraryW(path.c_str());
#if LOGGING
			std::cout << "og_version = " << (void*)og_version << std::endl;
#endif
			og_GetFileVersionInfoA = (GetFileVersionInfoA_t)GetProcAddress(og_version, "GetFileVersionInfoA");
			//og_GetFileInformationByHandle = (GetFileInformationByHandle_t)GetProcAddress(og_version, "GetFileInformationByHandle");
			og_GetFileVersionInfoExA = (GetFileVersionInfoExA_t)GetProcAddress(og_version, "GetFileVersionInfoExA");
			og_GetFileVersionInfoExW = (GetFileVersionInfoExW_t)GetProcAddress(og_version, "GetFileVersionInfoExW");
			og_GetFileVersionInfoSizeA = (GetFileVersionInfoSizeA_t)GetProcAddress(og_version, "GetFileVersionInfoSizeA");
			og_GetFileVersionInfoSizeExA = (GetFileVersionInfoSizeExA_t)GetProcAddress(og_version, "GetFileVersionInfoSizeExA");
			og_GetFileVersionInfoSizeExW = (GetFileVersionInfoSizeExW_t)GetProcAddress(og_version, "GetFileVersionInfoSizeExW");
			og_GetFileVersionInfoSizeW = (GetFileVersionInfoSizeW_t)GetProcAddress(og_version, "GetFileVersionInfoSizeW");
			og_GetFileVersionInfoW = (GetFileVersionInfoW_t)GetProcAddress(og_version, "GetFileVersionInfoW");
			og_VerFindFileA = (VerFindFileA_t)GetProcAddress(og_version, "VerFindFileA");
			og_VerFindFileW = (VerFindFileW_t)GetProcAddress(og_version, "VerFindFileW");
			og_VerInstallFileA = (VerInstallFileA_t)GetProcAddress(og_version, "VerInstallFileA");
			og_VerInstallFileW = (VerInstallFileW_t)GetProcAddress(og_version, "VerInstallFileW");
			og_VerLanguageNameA = (VerLanguageNameA_t)GetProcAddress(og_version, "VerLanguageNameA");
			og_VerLanguageNameW = (VerLanguageNameW_t)GetProcAddress(og_version, "VerLanguageNameW");
			og_VerQueryValueA = (VerQueryValueA_t)GetProcAddress(og_version, "VerQueryValueA");
			og_VerQueryValueW = (VerQueryValueW_t)GetProcAddress(og_version, "VerQueryValueW");
		}

		{
			DWORD dwHandle;
			DWORD version_info_size = og_GetFileVersionInfoSizeA("Warframe.x64.exe", &dwHandle);

			void* data = soup::malloc(version_info_size);
			og_GetFileVersionInfoA("Warframe.x64.exe", 0, version_info_size, data);

			/*struct LANGANDCODEPAGE {
				WORD wLanguage;
				WORD wCodePage;
			} *lpTranslate;
			UINT cbTranslate;
			VerQueryValueA(data, "\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate);
			for (INT i=0; i < (cbTranslate/4); i++)
			{
				std::cout << "Lang " << lpTranslate[i].wLanguage << ", c.p. " << lpTranslate[i].wCodePage << std::endl;
			}*/

			LPVOID value_data;
			UINT value_size;
			og_VerQueryValueA(data, "\\StringFileInfo\\040904B0\\ProductVersion", &value_data, &value_size);
			memcpy(build_label, value_data, 16);

			soup::free(data);
		}

#if LOGGING
		std::cout << "build_label = " << std::string(build_label, 16) << std::endl;
#endif

		std::error_code ec{};
		std::filesystem::create_directory(ObfusString("OpenWF").str(), ec);
		SOUP_RETHROW_FALSE(check_ec(ec));
		if (!std::filesystem::exists(ObfusString("OpenWF/Client Config.json").str()))
		{
			if (std::filesystem::exists(ObfusString("OpenWF/client_config.json").str()))
			{
				std::filesystem::rename(ObfusString("OpenWF/client_config.json").str(), ObfusString("OpenWF/Client Config.json").str(), ec);
				SOUP_RETHROW_FALSE(check_ec(ec));
			}
			else if (std::filesystem::exists(ObfusString("client_config.json").str()))
			{
				std::filesystem::rename(ObfusString("client_config.json").str(), ObfusString("OpenWF/Client Config.json").str(), ec);
				SOUP_RETHROW_FALSE(check_ec(ec));
			}
		}
		{
			UniquePtr<JsonNode> config = json::decodeFile(ObfusString("OpenWF/Client Config.json").str());
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
				server_host = ObfusString("127.0.0.1").str();
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
#if !CONFIG_LOADED_ONLY_ONCE
				fallback_language.clear();
#endif
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("fallback_languageVO")); it != config->reinterpretAsObj().end() && it->second->isStr())
			{
				fallback_languageVO = it->second->reinterpretAsStr().value;
			}
			else
			{
#if !CONFIG_LOADED_ONLY_ONCE
				fallback_languageVO.clear();
#endif
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("logout_on_request_failure")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				logout_on_request_failure = it->second->reinterpretAsBool().value;
			}
			else
			{
				logout_on_request_failure = true;
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
				string::lower(autologin_email);
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
				auto_start_scripts = { ObfusString("samples/Chat Commands.pluto").str() };
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

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("alternative_loading")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				alternative_loading = it->second->reinterpretAsBool().value;
			}
			else
			{
				alternative_loading = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("dont_resolve_labels")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				dont_resolve_labels = it->second->reinterpretAsBool().value;
			}
			else
			{
				dont_resolve_labels = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("save_all_metadata")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				save_all_metadata = it->second->reinterpretAsBool().value;
			}
			else
			{
				save_all_metadata = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("write_all_metadata_reads_to_console")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				write_all_metadata_reads_to_console = it->second->reinterpretAsBool().value;
			}
			else
			{
				write_all_metadata_reads_to_console = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("write_all_metadata_reads_to_ee_log")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				write_all_metadata_reads_to_ee_log = it->second->reinterpretAsBool().value;
			}
			else
			{
				write_all_metadata_reads_to_ee_log = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("write_patched_metadata_reads_to_console")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				write_patched_metadata_reads_to_console = it->second->reinterpretAsBool().value;
			}
			else
			{
				write_patched_metadata_reads_to_console = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("write_patched_metadata_reads_to_ee_log")); it != config->reinterpretAsObj().end() && it->second->isBool())
			{
				write_patched_metadata_reads_to_ee_log = it->second->reinterpretAsBool().value;
			}
			else
			{
				write_patched_metadata_reads_to_ee_log = false;
			}

			if (auto it = config->reinterpretAsObj().findIt(ObfusString("client_http_port")); it != config->reinterpretAsObj().end() && it->second->isInt())
			{
				static_assert(CONFIG_LOADED_ONLY_ONCE);
				client_http_port = it->second->reinterpretAsInt().value;
			}
			else
			{
				client_http_port = 6155;
			}
		}
		save_config();

		g_repo.loadBuiltinArchive();
		if (auto hotfix = string::fromFile(ObfusString("OpenWF/Hotfix.owf").str()); !hotfix.empty())
		{
			if (g_repo.loadHotfix(hotfix.data(), hotfix.size(), soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE)))
			{
				std::cout << ObfusString("Hotfix applied") << std::endl;
			}
			else
			{
				std::cout << ObfusString("Ignoring hotfix because it was made for a different DLL version") << std::endl;
			}
		}

		g_core_dict = g_repo.getCoreDict(fallback_language);
		/*for (auto& e : g_repo.getCoreDict(fallback_language))
		{
			g_core_dict.emplace(soup::joaat::hash(e.first), std::move(e.second));
		}*/

		{
			auto build_label_int = static_cast<uint64_t>(build_label[ 0] - '0') * 100000000000ull +
				static_cast<uint64_t>(build_label[ 1] - '0') * 10000000000ull +
				static_cast<uint64_t>(build_label[ 2] - '0') * 1000000000ull +
				static_cast<uint64_t>(build_label[ 3] - '0') * 100000000ull +
				static_cast<uint64_t>(build_label[ 5] - '0') * 10000000ull +
				static_cast<uint64_t>(build_label[ 6] - '0') * 1000000ull +
				static_cast<uint64_t>(build_label[ 8] - '0') * 100000ull +
				static_cast<uint64_t>(build_label[ 9] - '0') * 10000ull +
				static_cast<uint64_t>(build_label[11] - '0') * 1000ull +
				static_cast<uint64_t>(build_label[12] - '0') * 100ull +
				static_cast<uint64_t>(build_label[14] - '0') * 10ull +
				static_cast<uint64_t>(build_label[15] - '0');

			game_version = static_cast<uint16_t>(g_repo.getVersionedInt(soup::joaat::compileTimeHash("OpenWF/vv/game_versions.json"), build_label_int));

#if LOGGING
			std::cout << "build_label_int = " << build_label_int << std::endl;
			std::cout << "game_version = " << game_version << std::endl;
#endif
		}
//#if !PRIVATE
		if (game_version == GV(65, 53, 5))
		{
			auto msg = get_core_string(ObfusString("toonew").str());
			auto title = soup::unicode::utf8_to_utf16(BOOTSTRAPPER_TITLE);
			MessageBoxW(0, msg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
			return FALSE;
		}
//#endif

		std::wcout << get_core_string(ObfusString("freenote").str()) << std::endl;

		owfScript::init();

#if PRIVATE
		auto t = time::millis();
#endif

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
			}
		}*/

		// 2018.02.22.14.34 (M:8004325165498360760)
		// This breaks update 36
		/*{
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
			}
		}*/

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
			void* winhttp_connect;
			if (game_version >= GV(31, 5, 0))
			{
				SIG_INST("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 68 0C 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 0C 00 00");
				winhttp_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else
			{
				SIG_INST("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? 00 00 44 0F B7 AC");
				winhttp_connect = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
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
				log_optional_scan_failure(game_version >= GV(33, 6, 0));
			}
		}

		{
			Pointer game_http_request_caller;
			size_t offset;
			if (game_version >= GV(19, 12, 0))
			{
				SIG_INST("48 8D 53 18 E8 ? ? ? ? 48 8D 8B");
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 5;
			}
			else if (game_version >= GV(18, 18, 0))
			{
				SIG_INST("48 8D 53 18 48 8B CF E8 ? ? ? ? 48 8B 05"); // 2016.12.16.14.33, 2016.09.30.12.04, 2016.08.19.17.12
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
			else if (game_version >= GV(18, 5, 0))
			{
				SIG_INST("48 8D 53 18 48 8B CF E8 ? ? ? ? 48 8B 4F 48"); // 2016.03.31.15.16, 2016.03.04.10.06
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
			else if (game_version >= GV(18, 0, 0))
			{
				SIG_INST("48 8D 53 18 48 8B CF 40 88 6A 30 E8"); // 2015.12.05.18.07
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 12;
			}
			else if (game_version >= GV(15, 0, 0))
			{
				SIG_INST("48 8B CF 44 88 72 30 E8"); // 2015.10.21.12.48
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
			else if (game_version >= GV(12, 0, 0))
			{
				SIG_INST("48 8B CE 44 88 62 30 E8"); // 2014.02.07.16.15
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
			else if (game_version >= GV(9, 0, 0))
			{
				SIG_INST("48 8B CE 44 88 62 58 E8"); // 2013.07.15.20.46
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
			else
			{
				SIG_INST("48 8B CF 40 88 6A 58 E8"); // 2013.05.23.16.06
				game_http_request_caller = Module(nullptr).range.scan(sig_inst);
				offset = 8;
			}
#if LOGGING
			std::cout << "game_http_request_caller = " << game_http_request_caller.as<void*>() << std::endl;
#endif
			if (!game_http_request_caller)
			{
				report_critical_failure(get_core_string(ObfusString("sigfailbad").str()));
			}
			auto game_http_request = game_http_request_caller.add(offset).rip().as<void*>();
			if (game_version >= GV(35, 5, 0))
			{
				game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour<GameHttpRequest>);
			}
			else if (game_version >= GV(19, 0, 0))
			{
				game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour<LegacyGameHttpRequest, true>);
			}
			else if (game_version >= GV(12, 0, 0))
			{
				game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour<GameHttpRequestU18, true>);
			}
			else
			{
				game_http_request_hook.detour = reinterpret_cast<void*>(&game_http_request_detour<GameHttpRequestU8, true>);
			}
			game_http_request_hook.target = game_http_request;
			game_http_request_hook.code_cave = Module(nullptr).range.scan(CompactDetourHook::getCodeCavePattern()).as<void*>(); // Needed for 2017.03.06.15.49
#if LOGGING
			std::cout << "game_http_request_hook.code_cave = " << game_http_request_hook.code_cave << std::endl;
#endif
			game_http_request_hook.create();
			game_http_request_hook.enable();
		}

		// 38.5.0
		/*{
			SIG_INST("74 11 44 38 2D ? ? ? ? 74 08");
			auto WebGet_EncryptPost_cmp = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "WebGet_EncryptPost_cmp = " << WebGet_EncryptPost_cmp.as<void*>() << std::endl;
#endif
			if (WebGet_EncryptPost_cmp)
			{
				auto WebGet_EncryptPost = WebGet_EncryptPost_cmp.add(5).rip().as<bool*>();
				*WebGet_EncryptPost = false;
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}*/

		// Disable request encryption for 38.5.0 and above
		if (game_version >= GV(38, 5, 0))
		{
			{
				SIG_INST("40 53 57 41 54 48 83 EC 20 44 8B E2 48 8B F9 48 85 C9"); // 38.5.0, 38.5.2, 38.5.3
				auto encstr_append = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
				std::cout << "encstr_append = " << encstr_append << std::endl;
#endif
				if (encstr_append)
				{
					encstr_append_hook.detour = reinterpret_cast<void*>(&encstr_append_detour);
					encstr_append_hook.target = encstr_append;
					encstr_append_hook.create();
					encstr_append_hook.enable();
				}
			}

			{
				SIG_INST("48 8B C4 48 89 50 10 53 55 41 56 48 83 EC 50 48 89 70 18"); // 38.5.3
				auto encstr_discharge = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
				std::cout << "encstr_discharge = " << encstr_discharge << std::endl;
#endif
				if (encstr_discharge)
				{
					encstr_discharge_hook.detour = reinterpret_cast<void*>(&encstr_discharge_detour);
					encstr_discharge_hook.target = encstr_discharge;
					encstr_discharge_hook.create();
					encstr_discharge_hook.enable();
				}
			}

			if (!encstr_discharge_hook.target)
			{
				SIG_INST("48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 30 ? ? ? 4C 8B F2"); // 38.5.0, 38.5.2
				auto encstr_discharge = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
				std::cout << "encstr_discharge = " << encstr_discharge << std::endl;
#endif
				if (encstr_discharge)
				{
					encstr_discharge_hook.detour = reinterpret_cast<void*>(&encstr_discharge_detour);
					encstr_discharge_hook.target = encstr_discharge;
					encstr_discharge_hook.create();
					encstr_discharge_hook.enable();
				}
			}

			if (!encstr_append_hook.target || !encstr_discharge_hook.target)
			{
				report_critical_failure(get_core_string(ObfusString("sigfailenc").str()));
			}

			{
				//SIG_INST("48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ? ? ? ? 80 7B 0F FF 75 1D");
				//string_resize = Module(nullptr).range.scan(sig_inst).as<string_resize_t>();
				SIG_INST("48 8D 4B 18 33 D2 E8 ? ? ? ? 33 D2 48 8D 4B 38 E8 ? ? ? ? 48 8B 4C 24 30");
				auto string_resize_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
				std::cout << "string_resize_callsite = " << string_resize_callsite.as<void*>() << std::endl;
#endif
				if (string_resize_callsite)
				{
					string_resize = string_resize_callsite.add(7).rip().as<string_resize_t>();
				}
				else
				{
					log_optional_scan_failure(false);
				}
			}
		}

		// 38.5.0
		/*{
			SIG_INST("48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 30 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 20 01 00 00");
			auto queue_http_request_internal = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "queue_http_request_internal = " << queue_http_request_internal << std::endl;
#endif
			if (queue_http_request_internal)
			{
				queue_http_request_internal_hook.detour = reinterpret_cast<void*>(&queue_http_request_internal_detour);
				queue_http_request_internal_hook.target = queue_http_request_internal;
				queue_http_request_internal_hook.create();
				queue_http_request_internal_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}*/

#if PRIVATE
		{
			void* Curl_resolv;
			if (game_version >= GV(37, 0, 0))
			{
				SIG_INST("40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC A0 00 00 00 48 8B 05");
				Curl_resolv = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else
			{
				SIG_INST("48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 40 48 8B 39");
				Curl_resolv = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
#if LOGGING
			std::cout << "Curl_resolv = " << Curl_resolv << std::endl;
#endif
			if (Curl_resolv)
			{
				Curl_resolv_hook.detour = reinterpret_cast<void*>(&Curl_resolv_detour);
				Curl_resolv_hook.target = Curl_resolv;
				Curl_resolv_hook.create();
				Curl_resolv_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

		if (game_version >= GV(35, 5, 0)) // Just stripping TLS for older versions
		{
			Pointer ssl_verify_internal_caller;
			if (game_version >= GV(26, 1, 0))
			{
				SIG_INST("49 8B D4 48 8B ? E8 ? ? ? ? 85 C0 7F");
				ssl_verify_internal_caller = Module(nullptr).range.scan(sig_inst);
			}
			else
			{
				SIG_INST("49 8B D4 48 8B ? E8 ? ? ? ? 85 C0 7F ? 8B 8E"); // 2019.10.31.22.42
				ssl_verify_internal_caller = Module(nullptr).range.scan(sig_inst);
			}
#if LOGGING
			std::cout << "ssl_verify_internal_caller = " << ssl_verify_internal_caller.as<void*>() << std::endl;
#endif
			if (!ssl_verify_internal_caller)
			{
				report_critical_failure(get_core_string(ObfusString("sigfailbad").str()));
			}
			auto ssl_verify_internal = ssl_verify_internal_caller.add(7).rip().as<void*>();
			ssl_verify_internal_hook.detour = reinterpret_cast<void*>(&ssl_verify_internal_detour);
			ssl_verify_internal_hook.target = ssl_verify_internal;
			//ssl_verify_internal_hook.create();
			ssl_verify_internal_hook.enable();
		}

		if (game_version >= GV(35, 5, 0)) // Just stripping TLS for older versions
		{
			void* Curl_ossl_verifyhost;
			if (game_version >= GV(37, 0, 0))
			{
				SIG_INST("40 53 55 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 49 8B 10");
				Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(29, 0, 0))
			{
				SIG_INST("40 53 55 56 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 78 4C 8B 31");
				Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(26, 1, 0))
			{
				SIG_INST("48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 78 4C 8B 39"); // 2020.03.24.20.24, 2019.12.13.00.31, 2019.11.22.21.24
				Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else
			{
				SIG_INST("40 53 55 56 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 70 4C 8B 39"); // 2019.10.31.22.42, 2019.09.09.12.43
				Curl_ossl_verifyhost = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
#if LOGGING
			std::cout << "Curl_ossl_verifyhost = " << Curl_ossl_verifyhost << std::endl;
#endif
			if (!Curl_ossl_verifyhost)
			{
				report_critical_failure(get_core_string(ObfusString("sigfailbad").str()));
			}
			Curl_ossl_verifyhost_hook.detour = reinterpret_cast<void*>(&Curl_ossl_verifyhost_detour);
			Curl_ossl_verifyhost_hook.target = Curl_ossl_verifyhost;
			//Curl_ossl_verifyhost_hook.create();
			Curl_ossl_verifyhost_hook.enable();
		}

		// This hook allows WorldSeed to be absent or just any value.
		// 16.5 seemingly does not validate the WorldSeed.
		if (game_version >= GV(17, 0, 0))
		{
			void* verify_worldstate_integrity;
			if (game_version >= GV(35, 5, 0))
			{
				SIG_INST("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 83 EC 70 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 F0 48 8B D9");
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(23, 0, 0))
			{
				SIG_INST("48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC ? 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 37 48 8B D9 84 D2"); // 2023.07.26.16.38 (33.6.0), 2024.02.16.17.13 (35.1.0), 2018.06.14.23.21 (23.0.0)
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(22, 15, 0))
			{
				SIG_INST("48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 37 48 8B D9 84 D2"); // 2018.05.17.16.28 (22.20.0), 2018.04.20.02.04 (22.18.0), 2018.03.15.19.39 (22.16.0), 2018.03.07.14.18 (22.15.0)
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(21, 0, 0))
			{
				SIG_INST("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 47 48 8B D9 84 D2 0F 84"); // 2018.02.22.14.34 (22.13.4), 2017.06.29.02.13 (21.0.0)
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(19, 0, 0))
			{
				SIG_INST("48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 48 65 48 8B 04 25"); // 2017.03.06.15.49 (19.13.0)
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(18, 7, 1))
			{
				SIG_INST("48 89 5C 24 18 56 57 41 56 48 83 EC 60 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 50 8B 05"); // 2016.09.30.12.04, 2016.03.31.15.16
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(18, 5, 0))
			{
				SIG_INST("48 89 5C 24 18 56 48 83 EC 60 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 50 8B 05"); // 2016.03.04.10.06
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else
			{
				SIG_INST("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 50 8B 05"); // 2015.12.05.18.07
				verify_worldstate_integrity = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
#if LOGGING
			std::cout << "verify_worldstate_integrity = " << verify_worldstate_integrity << std::endl;
#endif
			if (!verify_worldstate_integrity)
			{
				report_critical_failure(get_core_string(ObfusString("sigfailbad").str()));
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
			Pointer parse_arguments_callsite;
			if (game_version >= GV(19, 0, 0))
			{
				//SIG_INST("48 8D 0D ? ? ? ? 49 8D 43 E8 49 89 43 E8 49 8D 43 E8 49 89 43 F0 E8");
				SIG_INST("48 8D 0D ? ? ? ? 49 8D 43 ? 49 89 43 ? 49 8D 43 ? 49 89 43 ? E8"); // 2019.05.22.23.12
				parse_arguments_callsite = Module(nullptr).range.scan(sig_inst);
			}
			else
			{
				// Also made this 24 bytes just to match the above pattern
				SIG_INST("89 43 ? 49 8D 43 ? 49 89 ? ? 49 89 43 ? 49 8D 43 ? 49 89 43 ? E8"); // 2016.09.30.12.04
				parse_arguments_callsite = Module(nullptr).range.scan(sig_inst);
			}
#if LOGGING
			std::cout << "parse_arguments_callsite = " << parse_arguments_callsite.as<void*>() << std::endl;
#endif
			if (parse_arguments_callsite)
			{
				auto parse_arguments = parse_arguments_callsite.add(24).rip().as<void*>();

				/*if (game_version >= GV(39, 0, 0))
				{
					parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour<GameString, true, true>);
				}
				else*/ if (game_version >= GV(35, 5, 0))
				{
					parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour<GameString/*, false, true*/>);
				}
				/*else if (game_version >= GV(28, 0, 0))
				{
					parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour<LegacyGameString, false, true>);
				}*/
				else if (game_version >= GV(19, 0, 0))
				{
					parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour<LegacyGameString/*, false, false*/>);
				}
				else
				{
					parse_arguments_hook.detour = reinterpret_cast<void*>(&parse_arguments_detour<LegacyGameStringU18/*, false, false*/>);
				}
				parse_arguments_hook.target = parse_arguments;
				parse_arguments_hook.create();
				parse_arguments_hook.enable();
			}
			else
			{
				on_got_server_host();
				log_optional_scan_failure(false);
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

		// Emulate a non-stripped build so that no H.Cache is needed (breaks dialogue)
		// Needed for versions prior to echoes of duviri. Doesn't seem to cause any issues.
		if (game_version < GV(33, 6, 0))
		{
			if (game_version >= GV(31, 6, 0))
			{
				SIG_INST("0F B6 44 24 70 40 0F B6 CF 88 05");
				auto insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
#if LOGGING
				std::cout << "is_stripped_insn = " << (void*)insn << std::endl;
#endif
				if (insn)
				{
					memGuard::setAllowedAccess(insn, 5, memGuard::ACC_RWX);
					insn[0] = 0x31;
					insn[1] = 0xc0;
					insn[2] = 0x90;
					insn[3] = 0x90;
					insn[4] = 0x90;
				}
				else
				{
					log_optional_scan_failure(true);
				}
			}
			else if (game_version >= GV(15, 0, 0))
			{
				uint8_t* insn;
				if (game_version >= GV(30, 0, 0))
				{
					SIG_INST("0F B6 84 24 ? 00 00 00 40 0F B6 CF 88 05"); // 2021.09.08.19.27
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else if (game_version >= GV(29, 3, 2))
				{
					SIG_INST("0F B6 84 24 90 00 00 00 0F B6 8C 24 A8 00 00 00 88 05"); // 2020.11.04.18.58
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else if (game_version >= GV(26, 0, 0))
				{
					SIG_INST("0F B6 84 24 ? 00 00 00 0F B6 CB 88 05"); // 2020.03.24.20.24, 2019.10.31.22.42
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else if (game_version >= GV(23, 9, 1))
				{
					SIG_INST("0F B6 84 24 80 00 00 00 88 05"); // 2019.09.09.12.43
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else if (game_version >= GV(23, 0, 0))
				{
					SIG_INST("0F B6 84 24 A0 00 00 00 88 05 ? ? ? ? 0F B6 84 24"); // 2018.06.14.23.21
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else if (game_version >= GV(19, 0, 0))
				{
					SIG_INST("0F B6 84 24 B0 00 00 00 88 05 ? ? ? ? 0F B6 84 24"); // 2018.02.22.14.34
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
				else
				{
					SIG_INST("0F B6 84 24 ? 00 00 00 40 88 2D ? ? ? ? 88 05"); // 2016.09.30.12.04, 2015.03.21.08.17
					insn = Module(nullptr).range.scan(sig_inst).as<uint8_t*>();
				}
#if LOGGING
				std::cout << "is_stripped_insn = " << (void*)insn << std::endl;
#endif
				if (insn)
				{
					memGuard::setAllowedAccess(insn, 8, memGuard::ACC_RWX);
					// xor eax, eax
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
					log_optional_scan_failure(true);
				}
			}
			else
			{
				SIG_INST("88 44 24 20 E8 ? ? ? ? 83 7B 0C 01 75"); // 2013.05.23.16.06, 2013.06.07.23.44, 2013.07.04.20.17
				const auto init_cache_fetching_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
				std::cout << "init_cache_fetching_callsite = " << init_cache_fetching_callsite.as<void*>() << std::endl;
#endif
				if (init_cache_fetching_callsite)
				{
					auto init_cache_fetching = init_cache_fetching_callsite.add(5).rip().as<void*>();

					init_cache_fetching_hook.detour = reinterpret_cast<void*>(&init_cache_fetching_detour);
					init_cache_fetching_hook.target = init_cache_fetching;
					init_cache_fetching_hook.create();
					init_cache_fetching_hook.enable();
				}
				else
				{
					log_optional_scan_failure(true);
				}
			}
		}

		if (game_version < GV(33, 0, 0))
		{
			void* legacy_dns_lookup;
			if (game_version >= GV(15, 0, 0))
			{
				SIG_INST("40 55 56 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? C6 41 06 01"); // 2016.12.16.14.33
				legacy_dns_lookup = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else if (game_version >= GV(11, 0, 0))
			{
				SIG_INST("48 89 5C 24 20 55 56 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 83 7A 08 00"); // 2013.11.29.16.33
				legacy_dns_lookup = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
			else
			{
				SIG_INST("40 55 53 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 83 7A 08 00"); // 2013.11.12.14.03
				legacy_dns_lookup = Module(nullptr).range.scan(sig_inst).as<void*>();
			}
#if LOGGING
			std::cout << "legacy_dns_lookup = " << legacy_dns_lookup << std::endl;
#endif
			if (legacy_dns_lookup)
			{
				if (game_version >= GV(19, 0, 0))
				{
					legacy_dns_lookup_hook.detour = reinterpret_cast<void*>(&legacy_dns_lookup_detour<LegacyGameString>);
				}
				else
				{
					legacy_dns_lookup_hook.detour = reinterpret_cast<void*>(&legacy_dns_lookup_detour<LegacyGameStringU18>);
				}
				legacy_dns_lookup_hook.target = legacy_dns_lookup;
				legacy_dns_lookup_hook.create();
				legacy_dns_lookup_hook.enable();
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfaillegacy").str()) << std::endl;
			}
		}

		{
			SIG_INST("48 C1 C8 ? 48 89 05 ? ? ? ? 48 33 C1 48 89 05 ? ? ? ? C3"); // Alternatively: CC 48 B9 ? ? ? ? ? ? ? ? 48 8D 05
			auto device_id_insn = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "device_id_insn = " << device_id_insn.as<void*>() << std::endl;
#endif
			if (device_id_insn)
			{
				//device_id_mask = device_id_insn.add(3).as<uint64_t&>();
				device_id_ptr = device_id_insn.add(7).rip().as<uint64_t*>();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

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
				std::wcout << get_core_string(ObfusString("sigfailxp").str()) << std::endl;
			}
		}
#endif

		/*{
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
				log_optional_scan_failure(false);
			}
		}*/

		{
			ObfusString str("SquadSetCountdownTimer");
			auto lua_SquadSetCountdownTimer_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
#if LOGGING
			std::cout << "lua_SquadSetCountdownTimer_hash = " << lua_SquadSetCountdownTimer_hash.as<void*>() << std::endl;
#endif
			if (lua_SquadSetCountdownTimer_hash)
			{
				auto lua_SquadSetCountdownTimer_fp = lua_SquadSetCountdownTimer_hash.add(8).as<luau_CFunction*>();
				lua_SquadSetCountdownTimer_og = *lua_SquadSetCountdownTimer_fp;
				memGuard::setAllowedAccess(lua_SquadSetCountdownTimer_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_SquadSetCountdownTimer_fp = lua_SquadSetCountdownTimer_detour;
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfailsmst").str()) << std::endl;
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
			auto dmg_number_patch_addr = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "dmg_number_patch_addr = " << dmg_number_patch_addr.as<void*>() << std::endl;
#endif
			if (get_total_damage_hook.target && dmg_number_patch_addr)
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
				*(void**)(detour_bytes + 21 + 2) = dmg_number_patch_addr.add(17).as<void*>(); // no jump at jz = compact numbers on -> go to `call log10f`
				*(void**)(detour_bytes + 40 + 2) = dmg_number_patch_addr.add(10).rip().as<void*>(); // jumped at jz = compact numbers off -> go to branch

				void* detour = memGuard::alloc(sizeof(detour_bytes), memGuard::ACC_RWX);
				memcpy(detour, detour_bytes, sizeof(detour_bytes));

				uint8_t trampoline[] = {
					0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)
					0x41, 0xff, 0xe2, // jmp r10
				};
				*(void**)(trampoline + 2) = detour;
				memGuard::setAllowedAccess(dmg_number_patch_addr.as<void*>(), sizeof(trampoline), memGuard::ACC_RWX);
				memcpy(dmg_number_patch_addr.as<void*>(), trampoline, sizeof(trampoline));
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfailhdnp").str()) << std::endl;
			}
		}

		{
			Pointer nrs_jnz;
			if (game_version >= GV(17, 0, 0))
			{
				//SIG_INST("0F 85 4A 20 00 00");
				SIG_INST("0F 85 ? ? ? ? 48 89 9C 24 ? ? ? ? 4C 89 BC 24 ? ? ? ? E8");
				nrs_jnz = Module(nullptr).range.scan(sig_inst);
			}
			else
			{
				SIG_INST("0F 85 ? ? ? ? 49 8D 9D ? ? ? ? 48 8D 15 ? ? ? ? 48 8B CB E8"); // 2015.05.14.16.29
				nrs_jnz = Module(nullptr).range.scan(sig_inst);
			}
#if LOGGING
			std::cout << "nrs_jnz = " << nrs_jnz.as<void*>() << std::endl;
#endif
			if (nrs_jnz)
			{
				if (disable_nrs_connection)
				{
					memGuard::setAllowedAccess(nrs_jnz.as<void*>(), 2, memGuard::ACC_RWX);
					nrs_jnz.as<uint8_t*>()[0] = 0x90;
					nrs_jnz.as<uint8_t*>()[1] = 0xE9;
				}
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfailnrs").str()) << std::endl;
			}
		}

		if (game_version >= GV(23, 10, 0)) // Seems to match something unexpected in 2018.06.14.23.21 & 2018.02.22.14.34
		{
			// "Sys [Error]: Could not write to "
			SIG_INST("48 8B 0D ? ? ? ? 48 85 C9 74 14 41 B8 20 00 00 00 48 8D 15 ? ? ? ? E8");
			auto write_to_log_file_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "write_to_log_file_callsite = " << write_to_log_file_callsite.as<void*>() << std::endl;
#endif
			if (write_to_log_file_callsite)
			{
				write_to_log_file_hook.detour = reinterpret_cast<void*>(&write_to_log_file_detour);
				write_to_log_file_hook.target = write_to_log_file_callsite.add(26).rip().as<void*>();
				write_to_log_file_hook.create();
				write_to_log_file_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(33, 0, 0))
		{
			SIG_INST("FC 94 94 BF 00 00 00 00 ? ? ? ? ? ? ? ? C0 99 E8 D0 00 00 00 00");
			auto lua_FlashMgr_GetConfigBool_hash = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_FlashMgr_GetConfigBool_hash = " << lua_FlashMgr_GetConfigBool_hash.as<void*>() << std::endl;
#endif
			if (lua_FlashMgr_GetConfigBool_hash)
			{
				auto lua_FlashMgr_GetConfigBool_fp = lua_FlashMgr_GetConfigBool_hash.add(8).as<luau_CFunction*>();
				lua_FlashMgr_GetConfigBool_og = *lua_FlashMgr_GetConfigBool_fp;
				memGuard::setAllowedAccess(lua_FlashMgr_GetConfigBool_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_FlashMgr_GetConfigBool_fp = lua_FlashMgr_GetConfigBool_detour;
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("UpdateFlashMarkers");
			auto lua_LotusHudStatus_UpdateFlashMarkers_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 8B 05 ? ? ? ? FF D0 85 C0 74 02 CD 2C");
			auto raise_script_error_fp_mov = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "raise_script_error_fp_mov = " << raise_script_error_fp_mov.as<void*>() << std::endl;
#endif
			if (raise_script_error_fp_mov)
			{
				raise_script_error_fp = raise_script_error_fp_mov.add(3).rip().as<raise_script_error_t*>();
			}
			else
			{
				log_optional_scan_failure(false);
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
				log_optional_scan_failure(false);
			}
		}*/

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 89 6C 24 18 56 48 83 EC 20 48 8B EA 48 8B F1 48 85 D2");
			luau_pushstring = Module(nullptr).range.scan(sig_inst).as<luau_pushstring_t>();
#if LOGGING
			std::cout << "luau_pushstring = " << (void*)luau_pushstring << std::endl;
#endif
			if (!luau_pushstring)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 75 0F");
			luau_pushpointer = Module(nullptr).range.scan(sig_inst).as<luau_pushpointer_t>();
#if LOGGING
			std::cout << "luau_pushpointer = " << (void*)luau_pushpointer << std::endl;
#endif
			if (!luau_pushpointer)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 75 0F 48 8B 74");
			luau_pushobject = Module(nullptr).range.scan(sig_inst).as<luau_pushobject_t>();
#if LOGGING
			std::cout << "luau_pushobject = " << (void*)luau_pushobject << std::endl;
#endif
			if (!luau_pushobject)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 49 63 F9 48 8B 49 18 49 8B F0");
			luau_pushcclosurek = Module(nullptr).range.scan(sig_inst).as<luau_pushcclosurek_t>();
#if LOGGING
			std::cout << "luau_pushcclosurek = " << (void*)luau_pushcclosurek << std::endl;
#endif
			if (!luau_pushcclosurek)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("BA 01 00 00 00 41 B8 06 00 00 00 48 8B D9 E8 ? ? ? ? BA 02 00 00 00 48 8B CB E8 ? ? ? ? BA 01 00 00 00 48 8B CB E8");
			auto lua_next_callsite = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "lua_next_callsite = " << lua_next_callsite.as<void*>() << std::endl;
#endif
			if (lua_next_callsite)
			{
				luau_next = lua_next_callsite.add(41).rip().as<luau_next_t>();
			}
		}

		if (game_version >= GV(37, 0, 0))
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
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 4C 8B 49 18 41 8B F0");
			luau_createtable = Module(nullptr).range.scan(sig_inst).as<luau_createtable_t>();
#if LOGGING
			std::cout << "luau_createtable = " << (void*)luau_createtable << std::endl;
#endif
			if (!luau_createtable)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("40 53 48 83 EC 20 4C 8B D1 85 D2 7E");
			luau_settable = Module(nullptr).range.scan(sig_inst).as<luau_settable_t>();
#if LOGGING
			std::cout << "luau_settable = " << (void*)luau_settable << std::endl;
#endif
			if (!luau_settable)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			if (game_version >= GV(39, 0, 0))
			{
				SIG_INST("40 53 57 48 83 EC 28 0F B7 41 50 48 8B D9 66 FF C0 49 63 F8");
				luauD_call = Module(nullptr).range.scan(sig_inst).as<luauD_call_t>();
			}
			else
			{
				SIG_INST("48 89 5C 24 18 57 48 83 EC 20 0F B7 41 50 48 8B D9 66 FF C0");
				luauD_call = Module(nullptr).range.scan(sig_inst).as<luauD_call_t>();
			}
#if LOGGING
			std::cout << "luauD_call = " << (void*)luauD_call << std::endl;
#endif
			if (!luauD_call)
			{
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
		{
			SIG_INST("48 8D 05 ? ? ? ? 48 89 35 ? ? ? ? 48 89 05 ? ? ? ? BF 01 00 00 00 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? EB");
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
						//std::cout << "\t- " << (*entry)->type_name << " " << (*entry)->field_name << std::endl;
#endif
						swig_types.emplace(soup::joaat::hashRange((*entry)->type_name, strlen((*entry)->type_name) - 2), (*entry)->type_desc);
#if PRIVATE
						swig_type_names.emplace_back(std::string((*entry)->type_name, strlen((*entry)->type_name) - 2));
#endif
					}
				}
			}
			if (nres == 0)
			{
#if LOGGING
				std::cout << "No results for swig types" << std::endl;
#endif
				log_optional_scan_failure(false);
			}
		}

		if (game_version >= GV(37, 0, 0))
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
#if LOGGING
				std::cout << "No results for swig enums" << std::endl;
#endif
				log_optional_scan_failure(false);
			}
		}

		{
			// "Using profile dir "
			SIG_INST("40 55 53 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 0F B6 81 ? ? ? ? 48 8D 99");
			auto get_profile_dir = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "get_profile_dir = " << get_profile_dir.as<void*>() << std::endl;
#endif
			if (get_profile_dir)
			{
				if (!forced_profile_dir.empty())
				{
					get_profile_dir_hook.detour = reinterpret_cast<void*>(&get_profile_dir_detour);
					get_profile_dir_hook.target = get_profile_dir.as<void*>();
					//get_profile_dir_hook.create();
					get_profile_dir_hook.enable();
				}
				get_profile_dir_offset = get_profile_dir.add(46).as<uint32_t&>();
#if LOGGING
				std::cout << "get_profile_dir_offset = " << get_profile_dir_offset << std::endl;
#endif
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfailfpd").str()) << std::endl;
			}
		}

		{
			ObfusString str("excludedFromSimulacrum");
			auto excludedFromSimulacrum_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				std::wcout << get_core_string(ObfusString("sigfailswb").str()) << std::endl;
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
				std::wcout << get_core_string(ObfusString("sigfailpast").str()) << std::endl;
			}
		}

		if (game_version >= GV(33, 0, 0)) // U32 Veilbreaker (2022.09.06.19.24) seems to crash in this detour
		{
			ObfusString str("GetStringVariable");
			auto lua_FlashInstance_GetStringVariable_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("OpenWebBrowser");
			auto lua_OpenWebBrowser_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
#if LOGGING
			std::cout << "lua_OpenWebBrowser_hash = " << lua_OpenWebBrowser_hash.as<void*>() << std::endl;
#endif
			if (lua_OpenWebBrowser_hash)
			{
				auto lua_OpenWebBrowser_fp = lua_OpenWebBrowser_hash.add(8).as<luau_CFunction*>();
				lua_OpenWebBrowser_og = *lua_OpenWebBrowser_fp;
				memGuard::setAllowedAccess(lua_OpenWebBrowser_fp, sizeof(void*), memGuard::ACC_READ | memGuard::ACC_WRITE);
				*lua_OpenWebBrowser_fp = lua_OpenWebBrowser_detour;
			}
			else
			{
				log_optional_scan_failure(false);
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("SetSeed");
			auto lua_SetSeed_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("ChurnSeed");
			auto lua_ChurnSeed_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("SRandom");
			auto lua_SRandom_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("SRandomInt");
			auto lua_SRandomInt_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("HashCrc32");
			auto lua_HashCrc32_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
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
				log_optional_scan_failure(false);
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
				log_optional_scan_failure(false);
			}
		}
#endif

#if VERBOSE_MD5
		{
			SIG_INST("4D 85 C0 0F 84 ? 00 00 00 48 89 6C 24 10");
			auto MD5_append = Module(nullptr).range.scan(sig_inst).add(9).as<void*>();
#if LOGGING
			std::cout << "MD5_append = " << MD5_append << std::endl;
#endif
			if (MD5_append != (void*)9)
			{
				MD5_append_hook.detour = reinterpret_cast<void*>(&MD5_append_detour);
				MD5_append_hook.target = MD5_append;
				MD5_append_hook.create();
				MD5_append_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if LABEL_REPLACEMENTS
		if (game_version >= GV(33, 0, 0)) // Seems to match something unexpected in 2021.09.08.19.27 (~30.5)
		{
			SIG_INST("4C 8B DC 57 41 ? 48 83 EC 78 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 48");
			auto check_string_substitutions = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "check_string_substitutions = " << check_string_substitutions << std::endl;
#endif
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
				std::wcout << get_core_string(ObfusString("sigfaillr").str()) << std::endl;
			}
		}
#endif

		{
			SIG_INST("48 8B 05 ? ? ? ? 0F B7 CA 48 03 C9 48 C1 EA 10 48 03 14 C8");
			auto string_pool_insn = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "string_pool_insn = " << string_pool_insn.as<void*>() << std::endl;
#endif
			if (string_pool_insn)
			{
				string_pool = string_pool_insn.add(3).rip().as<StringPoolBucket**>();
			}
			else
			{
#if !METADATA_PATCHES
				log_optional_scan_failure(false);
#endif
			}
		}

#if METADATA_PATCHES
		{
			SIG_INST("41 B1 03 48 8D 55 ? 45 33 C0 48 8D 8D ? ? ? ? E8");
			auto object_type_serialise_propery_text_call = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "object_type_serialise_propery_text_call = " << object_type_serialise_propery_text_call.as<void*>() << std::endl;
#endif
			if (object_type_serialise_propery_text_call && string_pool)
			{
				uint8_t detour_bytes[] = {
					0x49, 0x89, 0xF3, // mov r11, rsi
					/* 3 */ 0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // movabs r10, (8 bytes)
					0x41, 0xFF, 0xE2, // jmp r10
				};
				*(void**)(detour_bytes + 5) = (void*)object_type_serialise_propery_text_detour;

				void* detour = memGuard::alloc(sizeof(detour_bytes), memGuard::ACC_RWX);
				memcpy(detour, detour_bytes, sizeof(detour_bytes));

				object_type_serialise_propery_text_hook.detour = detour;
				object_type_serialise_propery_text_hook.target = object_type_serialise_propery_text_call.add(17).as<void*>();
				object_type_serialise_propery_text_hook.code_cave = Module(nullptr).range.scan(CallsiteHook::getCodeCavePattern()).as<void*>();
#if LOGGING
				std::cout << "object_type_serialise_propery_text_hook.code_cave = " << object_type_serialise_propery_text_hook.code_cave << std::endl;
#endif
				object_type_serialise_propery_text_hook.create();
				object_type_serialise_propery_text_hook.enable();
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfailmp").str()) << std::endl;
			}
		}
#endif

#if VERBOSE_SERPROPTXT
		{
			SIG_INST("48 8B C4 48 89 58 08 48 89 68 10 56 57 41 56 48 81 EC A0 00 00 00 0F 29 70 D8");
			auto serialise_propery_text = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "serialise_propery_text = " << serialise_propery_text << std::endl;
#endif
			if (serialise_propery_text)
			{
				serialise_propery_text_hook.detour = reinterpret_cast<void*>(&serialise_propery_text_detour);
				serialise_propery_text_hook.target = serialise_propery_text;
				serialise_propery_text_hook.create();
				serialise_propery_text_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if false // This one is definitely problematic for 40.0.0
		{
			//SIG_INST("48 89 6C 24 18 57 41 56 41 57 48 83 EC 30 4C 8B F1 4D 8B F9 48 8B CA 49 8B F8 48 8B EA E8"); // startInstance (4 arguments, void return)
			SIG_INST("48 89 5C 24 20 55 56 57 48 83 EC 30 48 8B E9 48 8B FA 48 8D 0D"); // startInstanceInternal (2 arguments, bool return)
			auto ScriptMgr_startInstance = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "ScriptMgr_startInstance = " << ScriptMgr_startInstance << std::endl;
#endif
			if (ScriptMgr_startInstance)
			{
				ScriptMgr_startInstance_hook.detour = reinterpret_cast<void*>(&ScriptMgr_startInstance_detour);
				ScriptMgr_startInstance_hook.target = ScriptMgr_startInstance;
				ScriptMgr_startInstance_hook.create();
				ScriptMgr_startInstance_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

		{
			SIG_INST("40 55 53 56 41 57 48 8D 6C 24 C1 48 81 EC A8 00 00 00 48 8B 05");
			auto irc_send_raw = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "irc_send_raw = " << irc_send_raw << std::endl;
#endif
			if (irc_send_raw)
			{
				irc_send_raw_hook.detour = reinterpret_cast<void*>(&irc_send_raw_detour);
				irc_send_raw_hook.target = irc_send_raw;
				irc_send_raw_hook.create();
				irc_send_raw_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

#if VERBOSE_IRC
		{
			SIG_INST("80 3D ? ? ? ? 00 74 ? 40 84 FF 74 ? B2 05");
			auto irc_log_in_cond = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "irc_log_in_cond = " << irc_log_in_cond.as<void*>() << std::endl;
#endif
			if (irc_log_in_cond)
			{
				memGuard::setAllowedAccess(irc_log_in_cond.add(12).as<void*>(), 2, memGuard::ACC_RWX);
				*irc_log_in_cond.add(12).as<uint8_t*>() = 0x90;
				*irc_log_in_cond.add(13).as<uint8_t*>() = 0x90;
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

		// Allow GetOnVehicle with an operator avatar
		// This is honestly such a stupid restriction for them to even have in code, I don't think it even needs a config to disable
		{
			// "an operator is trying to ride "
			SIG_INST("32 C0 48 8B 5C 24 40 48 8B 74 24 48 48 83 C4 30 5F C3 B2 05");
			auto operator_mount_fail = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "operator_mount_fail = " << operator_mount_fail.as<void*>() << std::endl;
#endif
			if (operator_mount_fail)
			{
				memGuard::setAllowedAccess(operator_mount_fail.as<void*>(), 2, memGuard::ACC_RWX);
				operator_mount_fail.as<uint8_t*>()[0] = 0xb0;
				operator_mount_fail.as<uint8_t*>()[1] = 0x01;
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		// Needed to make RequestSlomo work outside of Captura
		{
			SIG_INST("FF 90 ? ? 00 00 84 C0 74 ? F3 0F 11 73");
			auto RequestSlomo_cond = Module(nullptr).range.scan(sig_inst);
#if LOGGING
			std::cout << "RequestSlomo_cond = " << RequestSlomo_cond.as<void*>() << std::endl;
#endif
			if (RequestSlomo_cond)
			{
				memGuard::setAllowedAccess(RequestSlomo_cond.add(8).as<void*>(), 2, memGuard::ACC_RWX);
				RequestSlomo_cond.as<uint8_t*>()[8] = 0x90;
				RequestSlomo_cond.as<uint8_t*>()[9] = 0x90;
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		{
			ObfusString str("WebSubscribeToFailure");
			auto lua_WebSubscribeToFailure_hash = Module(nullptr).range.scan(hash_to_pattern(wf_hash(str.c_str())));
#if LOGGING
			std::cout << "lua_WebSubscribeToFailure_hash = " << lua_WebSubscribeToFailure_hash.as<void*>() << std::endl;
#endif
			if (lua_WebSubscribeToFailure_hash)
			{
				if (!logout_on_request_failure)
				{
					auto lua_WebSubscribeToFailure_code = *lua_WebSubscribeToFailure_hash.add(8).as<uint8_t**>();
					memGuard::setAllowedAccess(lua_WebSubscribeToFailure_code, 3, memGuard::ACC_RWX);
					lua_WebSubscribeToFailure_code[0] = 0x31;
					lua_WebSubscribeToFailure_code[1] = 0xC0;
					lua_WebSubscribeToFailure_code[2] = 0xC3;
				}
			}
			else
			{
				std::wcout << get_core_string(ObfusString("sigfaillorf").str()) << std::endl;
			}
		}

#if VERBOSE_OODLE
		{
			SIG_INST("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ? ? ? ? 44 8B 43 6C 49 3B C0");
			auto init_oodle_network_state = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "init_oodle_network_state = " << init_oodle_network_state << std::endl;
#endif
			if (init_oodle_network_state)
			{
				init_oodle_network_state_hook.detour = reinterpret_cast<void*>(&init_oodle_network_state_detour);
				init_oodle_network_state_hook.target = init_oodle_network_state;
				init_oodle_network_state_hook.create();
				init_oodle_network_state_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		{
			SIG_INST("48 89 5C 24 20 55 56 57 41 56 41 57 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 40 44 8B 71 08 48 8B D9");
			auto compress_packet_oodle_net = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "compress_packet_oodle_net = " << compress_packet_oodle_net << std::endl;
#endif
			if (compress_packet_oodle_net)
			{
				compress_packet_oodle_net_hook.detour = reinterpret_cast<void*>(&compress_packet_oodle_net_detour);
				compress_packet_oodle_net_hook.target = compress_packet_oodle_net;
				compress_packet_oodle_net_hook.create();
				compress_packet_oodle_net_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		{
			SIG_INST("40 53 55 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 48 44 8B 79 08 48 8B F9");
			auto compress_packet_oodle_lz = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "compress_packet_oodle_lz = " << compress_packet_oodle_lz << std::endl;
#endif
			if (compress_packet_oodle_lz)
			{
				compress_packet_oodle_lz_hook.detour = reinterpret_cast<void*>(&compress_packet_oodle_lz_detour);
				compress_packet_oodle_lz_hook.target = compress_packet_oodle_lz;
				compress_packet_oodle_lz_hook.create();
				compress_packet_oodle_lz_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		{
			SIG_INST("40 53 55 57 41 56 41 57 48 81 EC E0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 D0 00 00 00 48 63 9C 24 30 01 00 00 49 8B E9");
			auto oodle_compress = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "oodle_compress = " << oodle_compress << std::endl;
#endif
			if (oodle_compress)
			{
				oodle_compress_hook.detour = reinterpret_cast<void*>(&oodle_compress_detour);
				oodle_compress_hook.target = oodle_compress;
				oodle_compress_hook.create();
				oodle_compress_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if VERBOSE_SENDCNXLESS
		{
			SIG_INST("48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 83 B9");
			auto SendConnectionlessData = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "SendConnectionlessData = " << SendConnectionlessData << std::endl;
#endif
			if (SendConnectionlessData)
			{
				SendConnectionlessData_hook.detour = reinterpret_cast<void*>(&SendConnectionlessData_detour);
				SendConnectionlessData_hook.target = SendConnectionlessData;
				SendConnectionlessData_hook.create();
				SendConnectionlessData_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if VERBOSE_LZF
		{
			SIG_INST("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 4C 89 44 24 18 57 41 54 41 55 41 56 41 57 B8 00 00 04 00 E8 ? ? ? ? 48 2B E0");
			auto lzf_compress = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "lzf_compress = " << lzf_compress << std::endl;
#endif
			if (lzf_compress)
			{
				lzf_compress_hook.detour = reinterpret_cast<void*>(&lzf_compress_detour);
				lzf_compress_hook.target = lzf_compress;
				lzf_compress_hook.create();
				lzf_compress_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}

		{
			SIG_INST("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 44 8B DA 49 8B F8 4C 03 D9");
			auto lzf_decompress = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "lzf_decompress = " << lzf_decompress << std::endl;
#endif
			if (lzf_decompress)
			{
				lzf_decompress_hook.detour = reinterpret_cast<void*>(&lzf_decompress_detour);
				lzf_decompress_hook.target = lzf_decompress;
				lzf_decompress_hook.create();
				lzf_decompress_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if VERBOSE_UNCOMPRESSPKT
		{
			SIG_INST("40 55 56 57 41 54 41 56 48 8D 6C 24 C9 48 81 EC 90 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 1F 4C 8B 0A");
			auto UncompressPacket = Module(nullptr).range.scan(sig_inst).as<void*>();
#if LOGGING
			std::cout << "UncompressPacket = " << UncompressPacket << std::endl;
#endif
			if (UncompressPacket)
			{
				UncompressPacket_hook.detour = reinterpret_cast<void*>(&UncompressPacket_detour);
				UncompressPacket_hook.target = UncompressPacket;
				UncompressPacket_hook.create();
				UncompressPacket_hook.enable();
			}
			else
			{
				log_optional_scan_failure(false);
			}
		}
#endif

#if PRIVATE
		std::cout << "Scans & hooks done in " << (time::millis() - t) << " ms" << std::endl;
#endif

		{
			size_t size;
			auto data = g_repo.find(joaat::compileTimeHash("OpenWF/tunables.json"), size);
			g_client_tunables.loadMsgpack(data, size);
		}

		start_bgscript();

		load_hotkeys();

#if LABEL_REPLACEMENTS
		load_label_replacements();
#endif

#if METADATA_PATCHES
		load_metadata_patches();
#endif

		if (!auto_start_scripts.empty())
		{
			ObfusString base_path("OpenWF/Scripts/");
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
					if (joaat::hash(req.path.substr(0, 8)) == joaat::compileTimeHash("/origin/"))
					{
						req.path.erase(0, 16);
					}
					if (req.path.size() > 1
						&& (req.path[1] == '0'
							|| req.path[1] == '7'
							|| req.path[1] == '8'
							)
						)
					{
						// Try to locate file locally
						{
							std::string local_path = ObfusString("OpenWF/content");
							if (auto cache_req_path = ObfusString("/0/H.Cache.bin!D_---------------------w").str();
								req.path.find(cache_req_path) != std::string::npos
								)
							{
								// Example request: /origin/075B4E6D/0/H.Cache.bin!D_---------------------w
								local_path += cache_req_path;
							}
							else
							{
								local_path += req.path;
							}
							if (auto data = string::fromFile(local_path); !data.empty())
							{
								ServerWebService::sendText(s, std::move(data));
								return;
							}
						}

						// Continue in task to ask SNS
						HttpRequest hr(server_host + ":" + std::to_string(http_port), req.path);
						hr.use_tls = false;
						hr.path_is_encoded = true;
						Scheduler::get()->add<owfContentTask>(s, std::move(hr));

						return;
					}
					auto arr = string::explode(req.path, '?');
					const auto route_hash = soup::joaat::hash(urlenc::decode(arr[0]));
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
							size_t size;
							const char* data = g_repo.find(soup::joaat::compileTimeHash("OpenWF/index.html"), size);
							ServerWebService::sendHtml(s, data, size);
						}
						break;

					case soup::joaat::compileTimeHash("/dict.js"):
						{
							JsonObject obj;
							auto dict = g_repo.getWebuiDict(webui_lang_code);
							for (const auto& e : dict)
							{
								obj.add(std::move(e.first), std::move(e.second));
							}
							ServerWebService::sendData(s, ObfusString("text/javascript;charset=utf-8"), ObfusString("dict=").str() + obj.encode());
						}
						break;

					case soup::joaat::compileTimeHash("/ping"):
						ServerWebService::sendText(s, ObfusString("pong"));
						break;

					case soup::joaat::compileTimeHash("/save_all_metadata"):
						if (arr.size() > 1)
						{
							save_all_metadata = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(save_all_metadata));
						break;

					case soup::joaat::compileTimeHash("/write_all_metadata_reads_to_console"):
						if (arr.size() > 1)
						{
							write_all_metadata_reads_to_console = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(write_all_metadata_reads_to_console));
						break;

					case soup::joaat::compileTimeHash("/write_all_metadata_reads_to_ee_log"):
						if (arr.size() > 1)
						{
							write_all_metadata_reads_to_ee_log = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(write_all_metadata_reads_to_ee_log));
						break;

					case soup::joaat::compileTimeHash("/write_patched_metadata_reads_to_console"):
						if (arr.size() > 1)
						{
							write_patched_metadata_reads_to_console = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(write_patched_metadata_reads_to_console));
						break;

					case soup::joaat::compileTimeHash("/write_patched_metadata_reads_to_ee_log"):
						if (arr.size() > 1)
						{
							write_patched_metadata_reads_to_ee_log = (arr[1].size() == 4);
						}
						ServerWebService::sendText(s, std::to_string(write_patched_metadata_reads_to_ee_log));
						break;

					case soup::joaat::compileTimeHash("/pause_always_stops_time"):
						ServerWebService::sendText(s, std::to_string(pause_always_stops_time));
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

					/*case soup::joaat::compileTimeHash("/freecam"):
						if (regionmgr && !prohibit_freecam)
						{
							if (auto local_player = regionmgr->GetLocalPlayer())
							{
								local_player->controlling_camera = true;
								local_player->getAvatar()->followed_by_camera() = false;
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
								local_player->getAvatar()->followed_by_camera() = false;
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
								local_player->getAvatar()->followed_by_camera() = true;
							}
						}
						ServerWebService::sendText(s, {});
						break;*/

					case soup::joaat::compileTimeHash("/status"):
						{
							JsonObject obj;
							populate_full_status(obj);
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
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/scripts"):
						ServerWebService::sendText(s, get_available_scripts().encodePretty());
						break;

					case soup::joaat::compileTimeHash("/start_script"):
						if (!prohibit_scripts)
						{
							start_script_from_file(urlenc::decode(arr.at(1)));
							ServerWebService::sendText(s, {});
						}
						break;

					case soup::joaat::compileTimeHash("/start_script_inline"):
						if (!prohibit_scripts)
						{
							start_script_from_string(urlenc::decode(arr.at(1)));
							ServerWebService::sendText(s, {});
						}
						break;

					case soup::joaat::compileTimeHash("/stop_bgscript"): // Undocumented
						if (bgscript)
						{
							bgscript->stop_requested = true;
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/start_bgscript"): // Undocumented
						if (!bgscript)
						{
							start_bgscript();
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/restart_bgscript"): // Undocumented
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
						if (auto name = urlenc::decode(arr.at(1)); std::find(auto_start_scripts.begin(), auto_start_scripts.end(), name) == auto_start_scripts.end())
						{
							auto_start_scripts.emplace_back(std::move(name));
							save_config();
						}
						{
							JsonObject obj;
							populate_autostart_scripts(obj);
							owf_broadcast_message(obj.encode());
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/remove_autostart_script"):
						if (auto it = std::find(auto_start_scripts.begin(), auto_start_scripts.end(), urlenc::decode(arr.at(1))); it != auto_start_scripts.end())
						{
							auto_start_scripts.erase(it);
							save_config();
						}
						{
							JsonObject obj;
							populate_autostart_scripts(obj);
							owf_broadcast_message(obj.encode());
						}
						ServerWebService::sendText(s, {});
						break;

					case soup::joaat::compileTimeHash("/apply_hotfix"):
						{
							if (auto hotfix = string::fromFile(ObfusString("OpenWF/Hotfix.owf").str()); !hotfix.empty())
							{
								uint64_t timestamp;
								if (!owfRepo::readHotfixHeader(hotfix.data(), hotfix.size(), soup::joaat::compileTimeHash(BOOTSTRAPPER_TITLE), timestamp))
								{
									ServerWebService::sendText(s, ObfusString("Failed to apply hotfix as it was made for a different DLL version").str());
									break;
								}
								if (timestamp == g_repo.timestamp)
								{
									ServerWebService::sendText(s, ObfusString("No changes").str());
									break;
								}
								{
									std::lock_guard lock(g_repo_mtx);
									if (g_repo.hotfix) // Replacing one hotfix with another?
									{
										g_repo.loadBuiltinArchive();
									}
									g_repo.loadHotfix(hotfix.data(), hotfix.size());
								}
								ServerWebService::sendText(s, ObfusString("Hotfix applied").str());
							}
							else
							{
								const auto prev_timestamp = g_repo.timestamp;
								{
									std::lock_guard lock(g_repo_mtx);
									g_repo.loadBuiltinArchive();
								}
								if (g_repo.timestamp == prev_timestamp)
								{
									ServerWebService::sendText(s, ObfusString("No changes").str());
									break;
								}
								ServerWebService::sendText(s, ObfusString("Reverting to pre-hotfix state").str());
							}

							{
								size_t size;
								auto data = g_repo.find(joaat::compileTimeHash("OpenWF/tunables.json"), size);
								std::lock_guard lock(g_client_tunables_mtx);
								g_client_tunables.loadMsgpack(data, size);
							}

							restart_bgscript();

							load_hotkeys();

							{
								size_t size = 0;
								auto data = g_repo.find(joaat::compileTimeHash("OpenWF/helpers/post_apply_hotfix.pluto"), size);
								if (size)
								{
									start_script_from_string(std::string(data, size));
								}
							}

							ServerWebService::sendText(s, {});
						}
						break;

#if PRIVATE
					case soup::joaat::compileTimeHash("/reload_tunables"): // Undocumented
						{
							size_t size;
							if (auto data = (const char*)filesystem::createFileMapping("OpenWF/tunables.json", size))
							{
								{
									std::lock_guard lock(g_client_tunables_mtx);
									g_client_tunables.load(data, size);
								}
								filesystem::destroyFileMapping(data, size);
							}
							else
							{
								size_t size;
								data = g_repo.find(joaat::compileTimeHash("OpenWF/tunables.json"), size);
								std::lock_guard lock(g_client_tunables_mtx);
								g_client_tunables.loadMsgpack(data, size);
							}
							ServerWebService::sendText(s, {});
						}
						break;
#endif

					case soup::joaat::compileTimeHash("/version"):
						ServerWebService::sendText(s, ObfusString(BOOTSTRAPPER_TITLE).str());
						break;

					case soup::joaat::compileTimeHash("/game_version"):
						{
							JsonObject obj;
							obj.add(ObfusString("build_label"), std::string(build_label, 16));
							obj.add(ObfusString("build_hash"), build_hash[0] ? std::string(build_hash, 22) : std::string());
							ServerWebService::sendText(s, obj.encodePretty());
						}
						break;

#if LABEL_REPLACEMENTS
					case soup::joaat::compileTimeHash("/check_label_replacements"):
						ServerWebService::sendText(s, {});
						break;
#endif

					case soup::joaat::compileTimeHash("/memory"):
						{
							JsonObject obj;
							//obj.add(ObfusString("leaked"), static_cast<int64_t>(leaked_memory.load()));
//#if LABEL_REPLACEMENTS
							obj.add(ObfusString("fossilised"), static_cast<int64_t>(fossilised_memory.load()));
//#endif
							ServerWebService::sendText(s, obj.encodePretty());
						}
						break;

#if METADATA_PATCHES
					case soup::joaat::compileTimeHash("/get_effective_metadata"):
						{
							std::lock_guard lock(metadata_patches_mtx);
							if (auto e = metadata_patches.find(joaat::hash(urlenc::decode(arr.at(1)))); e != metadata_patches.end())
							{
								if (e->second.applied)
								{
									ServerWebService::sendText(s, e->second.final_data);
								}
								else
								{
									ServerWebService::sendText(s, ObfusString("patch not applied (yet)").str());
								}
							}
							else
							{
								ServerWebService::sendText(s, ObfusString("no such patch").str());
							}
						}
						break;

					case soup::joaat::compileTimeHash("/get_effective_metadata_as_json"):
						{
							std::lock_guard lock(metadata_patches_mtx);
							if (auto e = metadata_patches.find(joaat::hash(urlenc::decode(arr.at(1)))); e != metadata_patches.end())
							{
								if (e->second.applied)
								{
									EeNotationParser par;
									auto json = par.parse(e->second.final_data);
									ServerWebService::sendText(s, json->encodePretty());
								}
								else
								{
									ServerWebService::sendText(s, ObfusString("patch not applied (yet)").str());
								}
							}
							else
							{
								ServerWebService::sendText(s, ObfusString("no such patch").str());
							}
						}
						break;
#endif

					default:
						{
							// Try commands
							if (!req.path.empty())
							{
								if (JsonObject out; owf_command(urlenc::decode(req.path.begin() + 1, req.path.end()), out))
								{
									ServerWebService::sendText(s, out.encodePretty());
									break;
								}
							}

							// Try script routes
							bool handled = false;
							std::lock_guard lock(running_scripts_mtx);
							for (auto& scr : running_scripts)
							{
								if (auto route = scr->findStaticCustomRoute(route_hash))
								{
									ServerWebService::sendData(s, route->mime.c_str(), route->content);
									scr->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_SERVED, req.path);
									handled = true;
									break;
								}
								if (auto route = scr->handlesRouteDynamically(route_hash))
								{
									auto spTask = Scheduler::get()->add<owfScriptRouteTask>(s, scr->instance_id);
									scr->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_REQUEST, reinterpret_cast<uint64_t>(spTask.toDumb()), req.path);
									handled = true;
									break;
								}
							}
							if (!handled && bgscript)
							{
								if (auto route = bgscript->findStaticCustomRoute(route_hash))
								{
									ServerWebService::sendData(s, route->mime.c_str(), route->content);
									bgscript->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_SERVED, req.path);
									handled = true;
								}
								if (auto route = bgscript->handlesRouteDynamically(route_hash))
								{
									auto spTask = Scheduler::get()->add<owfScriptRouteTask>(s, bgscript->instance_id);
									bgscript->events.emplace_back(OWF_EVT_CUSTOM_ROUTE_REQUEST, reinterpret_cast<uint64_t>(spTask.toDumb()), req.path);
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
					s.custom_data.addStructToMap(owfWebsocketTag, owfWebsocketTag{ ++owfWebsocketTag::last_id });

					JsonObject obj;
					populate_full_status(obj);
					ServerWebService::wsSendText(s, obj.encode());
				};
				srv.on_websocket_message = [](WebSocketMessage& msg, Socket& s, ServerWebService&)
				{
					if (JsonObject out; owf_command(msg.data, out) && !out.empty())
					{
						ServerWebService::wsSendText(s, out.encode());
						return;
					}

					std::lock_guard lock(running_scripts_mtx);
					for (auto& scr : running_scripts)
					{
						if (scr->handlesWebsocketMessage(msg.data))
						{
							scr->events.emplace_back(OWF_EVT_WEBSOCKET_MESSAGE, s.custom_data.getStructFromMapConst(owfWebsocketTag).id, std::move(msg.data));
							return;
						}
					}
					if (bgscript)
					{
						if (auto route = bgscript->handlesWebsocketMessage(msg.data))
						{
							bgscript->events.emplace_back(OWF_EVT_WEBSOCKET_MESSAGE, s.custom_data.getStructFromMapConst(owfWebsocketTag).id, std::move(msg.data));
							return;
						}
					}
				};
				if (serv.bind(client_http_port, &srv))
				{
					serv.run();
				}
				else
				{
					std::cout << ObfusString("Failed to bind TCP/").str();
					std::cout << client_http_port;
					std::cout << '.';
					if (game_version >= GV(33, 6, 0))
					{
						std::cout << ObfusString(" The game will fail to start.").str();
					}
					std::cout << std::endl;
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
	const uint32_t recipient;

	owfBroadcastMessageTask(std::string&& msg, uint32_t recipient)
		: msg(std::move(msg)), recipient(recipient)
	{
	}

	void onTick() final
	{
		for (const auto& w : Scheduler::get()->workers)
		{
			if (w->type == soup::WORKER_TYPE_SOCKET
				&& static_cast<Socket*>(w.get())->custom_data.isStructInMap(owfWebsocketTag)
				&& (recipient == 0 || recipient == static_cast<Socket*>(w.get())->custom_data.getStructFromMapConst(owfWebsocketTag).id)
				)
			{
				ServerWebService::wsSendText(*static_cast<Socket*>(w.get()), msg);
			}
		}
		setWorkDone();
	}
};

void owf_broadcast_message(std::string&& msg, uint32_t recipient /*= 0*/)
{
	unicode::utf8_sanitise(msg);
	serv.add<owfBroadcastMessageTask>(std::move(msg), recipient);
}

owfScriptRouteTask::owfScriptRouteTask(soup::Socket& _s, size_t script_instance_id)
	: s(Scheduler::get()->getShared(_s)), script_instance_id(script_instance_id)
{
	ServerWebService::setKeepAlive(_s, true);
}

void owfScriptRouteTask::onTick() /*final*/
{
	if (auto response = this->response.load())
	{
		ServerWebService::sendData(*static_cast<Socket*>(s.get()), response->mime.c_str(), std::move(response->content));
		delete response;
		return setWorkDone();
	}
	if (static_cast<Socket*>(s.get())->isWorkDoneOrClosed())
	{
#if LOGGING
		std::cout << "owfScriptRouteTask: client socket is gone" << std::endl;
#endif
		return setWorkDone();
	}
	if (get_script_by_instance_id(script_instance_id) == nullptr)
	{
#if LOGGING
		std::cout << "owfScriptRouteTask: script instance is gone" << std::endl;
#endif
		ServerWebService::sendContent(*static_cast<Socket*>(s.get()), "500 Internal Server Error", ObfusString("Sorry, this request was supposed to be handled by a script, but that script is no longer running now.").str());
		return setWorkDone();
	}
}
