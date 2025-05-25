#pragma once

#include <string>
#include <vector>

#include <Mutex.hpp>

struct owfHotkey
{
	int vk;
	bool ctrl;
	bool shift;
	bool alt;
	bool was_pressed = false;
	std::string script;

	bool isPressed() const noexcept
	{
		if (GetAsyncKeyState(vk) & 0x8000)
		{
			if (!ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000))
			{
				if (!shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000))
				{
					if (!alt || (GetAsyncKeyState(VK_MENU) & 0x8000))
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
