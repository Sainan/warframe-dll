#pragma once

#include <string>

#include <fwd.hpp>

extern const char* g_bootstrapper_title;

extern void memoise_server_tunables();
extern bool owf_command(const std::string& in, soup::JsonObject& out);
extern void owf_broadcast_message(std::string&& msg, uint32_t recipient = 0);
