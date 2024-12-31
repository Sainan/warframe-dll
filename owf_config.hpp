#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline std::string server_host;
inline uint16_t http_port;
inline uint16_t https_port;
inline std::string fallback_language;
inline std::string fallback_graphicsDriver;
inline std::string fallback_cluster;
inline bool high_damage_numbers_patch;
inline bool skip_mission_start_timer;
inline float fov_override;
inline bool simulacrum_blacklisted;
inline bool simulacrum_whitelisted;
inline bool enable_http_interface;
inline bool disable_nrs_connection;
inline bool autologin;
inline std::string autologin_email;
inline std::string autologin_password;
inline std::vector<std::string> auto_start_scripts;
inline std::string forced_profile_dir;
