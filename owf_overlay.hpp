#pragma once

#include <cstdint>
#include <mutex> // lock_guard
#include <string>
#include <vector>

#include <Mutex.hpp>
#include <RasterFont.hpp>
#include <UniquePtr.hpp>

struct owfOverlay
{
	static bool isInited();
	static void init();
	static void setPrelogin(bool prelogin);
	static void redraw();
	static unsigned int getWidth();
	static unsigned int getHeight();

	struct DrawItem
	{
		enum Type : int8_t
		{
			RECT,
			TEXT,
		};

		int8_t type;
		uint8_t r;
		uint8_t g;
		uint8_t b;
		int x;
		int y;

		int8_t getType() const noexcept
		{
			return type < 0 ? type * -1 : type;
		}
	};

	inline static soup::Mutex mtx;
	inline static std::vector<soup::UniquePtr<DrawItem>> data;

	static void remove(DrawItem* item)
	{
		std::lock_guard lock(mtx);
		for (auto i = data.begin(); i != data.end(); ++i)
		{
			if (i->get() == item)
			{
				data.erase(i);
				break;
			}
		}
	}

	struct Rect : public DrawItem
	{
		unsigned int width;
		unsigned int height;

		Rect(int x, int y, unsigned int width, unsigned int height, uint8_t r, uint8_t g, uint8_t b)
			: DrawItem(RECT, r, g, b, x, y), width(width), height(height)
		{
		}
	};

	static DrawItem* addRect(int x, int y, unsigned int width, unsigned int height, uint8_t r, uint8_t g, uint8_t b)
	{
		std::lock_guard lock(mtx);
		return data.emplace_back(soup::make_unique<Rect>(x, y, width, height, r, g, b)).get();
	}

	struct Text : public DrawItem
	{
		const soup::RasterFont* font;
		std::string text;
		uint8_t scale;

		Text(int x, int y, std::string&& text, const soup::RasterFont* font, uint8_t r, uint8_t g, uint8_t b, uint8_t scale = 1)
			: DrawItem(TEXT, r, g, b, x, y), font(font), text(std::move(text)), scale(scale)
		{
		}
	};

	static DrawItem* addText(int x, int y, std::string&& text, const soup::RasterFont* font, uint8_t r, uint8_t g, uint8_t b, uint8_t scale = 1)
	{
		std::lock_guard lock(mtx);
		return data.emplace_back(soup::make_unique<Text>(x, y, std::move(text), font, r, g, b, scale)).get();
	}
};
