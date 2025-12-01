#pragma once

#include <atomic>
#include <string>

#include <SharedPtr.hpp>
#include <Socket.hpp>
#include <Task.hpp>

extern void start_builtin_http_server();
extern void owf_broadcast_message(std::string&& msg, uint32_t recipient = 0);

struct CustomRouteResponse
{
	std::string mime;
	std::string content;
};

struct owfScriptRouteTask final : public soup::Task
{
	soup::SharedPtr<soup::Worker> s;
	const size_t script_instance_id;
	std::atomic<CustomRouteResponse*> response = nullptr;

	owfScriptRouteTask(soup::Socket& _s, size_t script_instance_id);

	void onTick() final;
};
