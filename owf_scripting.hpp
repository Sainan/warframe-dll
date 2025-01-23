#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <lua.h> // lua_State

#include "owf_overlay.hpp"

inline soup::Mutex script_log_mtx;
inline std::string script_log;

inline uintptr_t ChatRedux_table = 0;
inline uintptr_t ChatRedux_SystemMessage_method = 0;

inline std::string bgscript_status_string;

inline std::string active_input_filter;

enum owfScriptEventType : uint8_t
{
	OWF_EVT_BLOCKED_CHAT_MESSAGE = 1,
	OWF_EVT_CUSTOM_ROUTE_SERVED = 2,
	OWF_EVT_CALLBACK = 3,
};

struct owfScript
{
	std::string name;
	lua_State* main;
	lua_State* coro = nullptr;
	bool stop_requested = false;

	std::unordered_set<owfOverlay::DrawItem*> overlay_items;

	struct Event
	{
		owfScriptEventType type;
		std::string data;
	};
	struct CustomRoute
	{
		std::string mime;
		std::string content;
	};
	std::unordered_set<std::string> blocked_chat_prefixes;
	std::unordered_map<uint32_t, CustomRoute> custom_routes;
	std::unordered_set<std::string> callbacks;
	std::deque<Event> events;

	static void logNl(const std::string& msg);
	static void log(const std::string& msg);

	owfScript();

	bool loadFile(std::string&& path);
	bool loadString(std::string&& code);

	bool tick();

	bool isBlockingMessage(const std::string& msg) const noexcept
	{
		for (const auto& prefix : blocked_chat_prefixes)
		{
			if (msg.starts_with(prefix))
			{
				return true;
			}
		}
		return false;
	}

	const CustomRoute* findCustomRoute(uint32_t hash) const noexcept
	{
		if (auto e = custom_routes.find(hash); e != custom_routes.end())
		{
			return &e->second;
		}
		return nullptr;
	}

	~owfScript()
	{
		lua_close(main);

		if (!overlay_items.empty())
		{
			bool need_redraw = false;
			for (auto& id : overlay_items)
			{
				owfOverlay::remove(id);
				need_redraw |= (id->type >= 0);
			}
			if (need_redraw)
			{
				owfOverlay::redraw();
			}
			overlay_items.clear();
		}
	}
};

inline soup::Mutex running_scripts_mtx;
inline std::vector<soup::UniquePtr<owfScript>> running_scripts;
inline owfScript* bgscript = nullptr;
