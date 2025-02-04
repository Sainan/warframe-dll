#pragma once

extern void owf_broadcast_message(std::string&& msg);

struct owfConsole
{
	inline static bool active = false;

	static void activate(const char* title)
	{
		active = true;

		AllocConsole();
#if PRIVATE
		std::string str = title;
		str.append(" [Private Build]");
		title = str.c_str();
#endif
		SetConsoleTitleA(title);
		{
			FILE* f;
			freopen_s(&f, ObfusString("CONIN$"), ObfusString("r"), stdin);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stderr);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stdout);
		}
		SetConsoleCP(CP_UTF8);
		SetConsoleOutputCP(CP_UTF8);

		owf_broadcast_message(ObfusString(R"({"console":true})").str());
	}

	static void deactivate()
	{
		active = false;

		const auto conWnd = GetConsoleWindow();
		FreeConsole();
		PostMessage(conWnd, WM_CLOSE, 0, 0);

		owf_broadcast_message(ObfusString(R"({"console":false})").str());
	}
};
