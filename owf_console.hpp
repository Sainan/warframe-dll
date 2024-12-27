struct owfConsole
{
	inline static bool active = false;

	static void activate()
	{
		AllocConsole();
		SetConsoleTitleA(BOOTSTRAPPER_TITLE);
		{
			FILE* f;
			freopen_s(&f, ObfusString("CONIN$"), ObfusString("r"), stdin);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stderr);
			freopen_s(&f, ObfusString("CONOUT$"), ObfusString("w"), stdout);
		}
		active = true;
	}

	static void deactivate()
	{
		active = false;
		const auto conWnd = GetConsoleWindow();
		FreeConsole();
		PostMessage(conWnd, WM_CLOSE, 0, 0);
	}
};
