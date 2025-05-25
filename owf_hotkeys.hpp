#pragma once

#include <string>
#include <vector>

#include <Mutex.hpp>

struct owfHotkey
{
	int vk;
	bool has_ctrl;
	bool ctrl;
	bool has_shift;
	bool shift;
	bool has_alt;
	bool alt;
	bool was_pressed = false;
	std::string script;

	bool isPressed() const noexcept
	{
		if (GetAsyncKeyState(vk) & 0x8000)
		{
			if (!has_ctrl || ctrl == (bool)(GetAsyncKeyState(VK_CONTROL) & 0x8000))
			{
				if (!has_shift || shift == (bool)(GetAsyncKeyState(VK_SHIFT) & 0x8000))
				{
					if (!has_alt || alt == (bool)(GetAsyncKeyState(VK_MENU) & 0x8000))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	bool wasJustPressed() noexcept
	{
		const bool pressed = isPressed();
		if (pressed)
		{
			if (!was_pressed)
			{
				was_pressed = true;
				return true;
			}
		}
		else
		{
			was_pressed = false;
		}
		return false;
	}
};

inline soup::Mutex hotkeys_mtx;
inline std::vector<owfHotkey> hotkeys;

void load_hotkeys();
