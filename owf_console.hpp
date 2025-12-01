#pragma once

#include <string>

#include <ObfusString.hpp>

#include "owf_web.hpp" // owf_broadcast_message

struct owfConsole
{
	inline static bool active = false;
	inline static std::string title;

	static void setTitle(std::string&& str)
	{
		title = std::move(str);
#if PRIVATE
		title.append(" [Private Build]");
#endif
		if (active)
		{
			SetConsoleTitleA(title.c_str());
		}
	}

	static void activate()
	{
		active = true;

		AllocConsole();
		SetConsoleTitleA(title.c_str());
		{
			FILE* f;
			freopen_s(&f, "CONIN$", "r", stdin);
			freopen_s(&f, "CONOUT$", "w", stderr);
			freopen_s(&f, "CONOUT$", "w", stdout);
		}
		SetConsoleCP(CP_UTF8);
		SetConsoleOutputCP(CP_UTF8);

		owf_broadcast_message(soup::ObfusString(R"({"console":true})").str());
	}

	static void deactivate()
	{
		active = false;

		const auto conWnd = GetConsoleWindow();
		FreeConsole();
		PostMessage(conWnd, WM_CLOSE, 0, 0);

		owf_broadcast_message(soup::ObfusString(R"({"console":false})").str());
	}
};
