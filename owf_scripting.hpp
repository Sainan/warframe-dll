#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <lua.h> // lua_State

#include "owf_overlay.hpp"
#include "owf_web.hpp"

inline soup::Mutex script_log_mtx;
inline std::string script_log;

inline uintptr_t ChatRedux_table = 0;

inline std::string active_input_filter;
inline bool active_input_filter_allows_hotkeys;

#define OWF_SET_GLOBAL(L, name) { ObfusString os(name); lua_setglobal(L, os.c_str()); }
#define OWF_SET_GLOBAL_INT(L, name, value) lua_pushinteger(L, value); OWF_SET_GLOBAL(L, name);
#define OWF_EXPOSE_INT_CONSTANT(L, e) OWF_SET_GLOBAL_INT(L, #e, e);

enum owfScriptEventType : uint8_t
{
	OWF_EVT_SUBMIT_CHAT_MESSAGE = 1,
	OWF_EVT_OUTGOING_CHAT_MESSAGE = 2,
	OWF_EVT_CUSTOM_ROUTE_REQUEST = 3,
	OWF_EVT_CUSTOM_ROUTE_SERVED = 4,
	OWF_EVT_CALLBACK = 5,
	OWF_EVT_WEBSOCKET_MESSAGE = 6,
	OWF_EVT_SCRIPT_MESSAGE = 7,
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
		uint64_t intdata;
		std::string data;

		Event(owfScriptEventType type, std::string data)
			: type(type), data(std::move(data))
		{
		}

		Event(owfScriptEventType type, uint64_t intdata, std::string data)
			: type(type), intdata(intdata), data(std::move(data))
		{
		}
	};
	std::unordered_map<std::string, bool> subscribed_chat_prefixes;
	std::unordered_set<std::string> subscribed_outgoing_chat_prefixes;
	std::unordered_set<std::string> websocket_message_prefixes;
	std::unordered_set<std::string> channels;
	std::unordered_map<uint32_t, CustomRouteResponse> static_custom_routes;
	std::unordered_set<uint32_t> dynamic_custom_routes;
	std::unordered_set<std::string> callbacks;
	//std::unordered_map<uint32_t, bool> subscribed_script_triggers;
	std::deque<Event> events;

	static void init();

	static void logNl(std::string msg);
	static void log(std::string msg);

	static void openLibs(lua_State* L);

	owfScript();

	bool loadFile(std::string&& path);
	bool loadString(const std::string& name, const std::string& code);

	bool tick();
	int tick(int nargs);

	lua_Integer getHotfixVersion() const;

	const bool* findChatSendSubscription(const std::string& msg) const noexcept
	{
		for (const auto& e : subscribed_chat_prefixes)
		{
			if (msg.starts_with(e.first))
			{
				return &e.second;
			}
		}
		return nullptr;
	}

	bool isSubscribedToOutgoingMessage(const std::string_view& msg) const noexcept
	{
		for (const auto& prefix : subscribed_outgoing_chat_prefixes)
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

	const CustomRouteResponse* findStaticCustomRoute(uint32_t hash) const noexcept
	{
		if (auto e = static_custom_routes.find(hash); e != static_custom_routes.end())
		{
			return &e->second;
		}
		return nullptr;
	}

	bool handlesRouteDynamically(uint32_t hash) const noexcept
	{
		if (auto e = dynamic_custom_routes.find(hash); e != dynamic_custom_routes.end())
		{
			return true;
		}
		return false;
	}

	/*const bool* findSubscribedScriptTrigger(uint32_t hash) const noexcept
	{
		if (auto e = subscribed_script_triggers.find(hash); e != subscribed_script_triggers.end())
		{
			return &e->second;
		}
		return nullptr;
	}*/

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

inline owfScript* get_script_by_name(const std::string& name)
{
	for (const auto& scr : running_scripts)
	{
		if (scr->name == name)
		{
			return scr;
		}
	}
	return nullptr;
}

extern owfScript* get_script_by_instance_id(size_t instance_id);
