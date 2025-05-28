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

inline std::string active_input_filter;
inline bool active_input_filter_allows_hotkeys;

#define OWF_SET_GLOBAL(L, name) { ObfusString os(name); lua_setglobal(L, os.c_str()); }
#define OWF_SET_GLOBAL_INT(L, name, value) lua_pushinteger(L, value); OWF_SET_GLOBAL(L, name);
#define OWF_EXPOSE_INT_CONSTANT(L, e) OWF_SET_GLOBAL_INT(L, #e, e);

enum owfScriptEventType : uint8_t
{
	OWF_EVT_BLOCKED_CHAT_MESSAGE = 1,
	OWF_EVT_BLOCKED_OUTGOING_CHAT_MESSAGE = 5,
	OWF_EVT_CUSTOM_ROUTE_SERVED = 2,
	OWF_EVT_CALLBACK = 3,
	OWF_EVT_SCRIPT_TRIGGERED = 4,
	OWF_EVT_WEBSOCKET_MESSAGE = 6,
};

struct owfScript
{
	std::string name;
	lua_State* main;
	lua_State* coro = nullptr;
	const size_t instance_id;
	bool callback_context = false;
	bool stop_requested = false;

	std::unordered_set<owfOverlay::DrawItem*> overlay_items;

	struct Event
	{
		owfScriptEventType type;
		uint32_t intdata;
		std::string data;

		Event(owfScriptEventType type, std::string data)
			: type(type), data(std::move(data))
		{
		}

		Event(owfScriptEventType type, uint32_t intdata, std::string data)
			: type(type), intdata(intdata), data(std::move(data))
		{
		}
	};
	struct CustomRoute
	{
		std::string mime;
		std::string content;
	};
	std::unordered_set<std::string> blocked_chat_prefixes;
	std::unordered_set<std::string> blocked_outgoing_chat_prefixes;
	std::unordered_set<std::string> websocket_message_prefixes;
	std::unordered_map<uint32_t, CustomRoute> custom_routes;
	std::unordered_set<std::string> callbacks;
	std::unordered_map<uint32_t, bool> subscribed_script_triggers;
	std::deque<Event> events;

	static void logNl(std::string msg);
	static void log(std::string msg);

	static void openLibs(lua_State* L);

	owfScript();

	bool loadFile(std::string&& path);
	bool loadString(const std::string& name, const std::string& code);

	bool tick();
	int tick(int nargs);

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

	bool isBlockingOutgoingMessage(const std::string_view& msg) const noexcept
	{
		for (const auto& prefix : blocked_outgoing_chat_prefixes)
		{
			if (msg.starts_with(prefix))
			{
				return true;
			}
		}
		return false;
	}

	bool handlesWebsocketMessage(const std::string& msg) const noexcept
	{
		for (const auto& prefix : websocket_message_prefixes)
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

	const bool* findSubscribedScriptTrigger(uint32_t hash) const noexcept
	{
		if (auto e = subscribed_script_triggers.find(hash); e != subscribed_script_triggers.end())
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

inline soup::RecursiveMutex running_scripts_mtx;
inline std::vector<owfScript*> running_scripts;
inline owfScript* bgscript = nullptr;
