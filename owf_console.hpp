#pragma once

#include <string>

#include <ObfusString.hpp>

#include "owf_web.hpp" // owf_broadcast_message

struct owfConsole
{
	inline static bool active = false;
	inline static std::string title;
	inline static HANDLE handle = INVALID_HANDLE_VALUE;

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
		SOUP_IF_LIKELY (!active)
		{
			active = true;

			AllocConsole();
			SetConsoleTitleA(title.c_str());
			SetConsoleCP(CP_UTF8);
			SetConsoleOutputCP(CP_UTF8);

			owfConsole::handle = GetStdHandle(STD_OUTPUT_HANDLE);

			owf_broadcast_message(soup::ObfusString(R"({"console":true})").str());
		}
	}

	static void deactivate()
	{
		SOUP_IF_LIKELY (active)
		{
			active = false;

			setSharedOutput();
			handle = INVALID_HANDLE_VALUE;

			const auto conWnd = GetConsoleWindow();
			FreeConsole();
			PostMessage(conWnd, WM_CLOSE, 0, 0);

			owf_broadcast_message(soup::ObfusString(R"({"console":false})").str());
		}
	}

	static void setExclusiveOutput()
	{
		HANDLE h = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		SetStdHandle(STD_OUTPUT_HANDLE, h);
	}

	static void setSharedOutput()
	{
		SetStdHandle(STD_OUTPUT_HANDLE, handle);
	}
};

struct owfConOut
{
	void write(const char* data, size_t size)
	{
		WriteFile(owfConsole::handle, data, size, nullptr, nullptr);
	}

	owfConOut& operator << (const char* msg)
	{
		write(msg, strlen(msg));
		return *this;
	}

	owfConOut& operator << (const std::string& msg)
	{
		write(msg.data(), msg.size());
		return *this;
	}

	owfConOut& operator << (int16_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (uint16_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (int32_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (uint32_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (int64_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (uint64_t val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (float val) { return operator<<(std::to_string(val)); }
	owfConOut& operator << (double val) { return operator<<(std::to_string(val)); }

	template <typename T, SOUP_RESTRICT(std::is_pointer_v<T>)>
	owfConOut& operator << (T val)
	{
		std::stringstream stream;
		stream << val;
		return operator<<(stream.str());
	}

	// Shim for std::endl, std::flush, etc.
	owfConOut& operator<<(std::ostream&(*manip)(std::ostream&))
	{
		if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl))
		{
			write("\r\n", 2);
		}
		return *this;
	}
};
inline owfConOut conout;
