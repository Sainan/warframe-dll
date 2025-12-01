#pragma once

#include <atomic>
#include <string>

#include <fwd.hpp>
#include <SharedPtr.hpp>
#include <Socket.hpp>
#include <Task.hpp>

#include "owf_web.hpp"

#define BOOTSTRAPPER_TITLE "OpenWF Bootstrapper v0.12.1"

extern bool set_server_tunables(const char* data, size_t size, bool delta = false);
extern bool owf_command(const std::string& in, soup::JsonObject& out);
extern void owf_broadcast_message(std::string&& msg, uint32_t recipient = 0);

struct owfScriptRouteTask final : public soup::Task
{
	soup::SharedPtr<soup::Worker> s;
	const size_t script_instance_id;
	std::atomic<CustomRouteResponse*> response = nullptr;

	owfScriptRouteTask(soup::Socket& _s, size_t script_instance_id);

	void onTick() final;
};
