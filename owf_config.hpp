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
inline bool pause_always_stops_time;
inline bool disable_nrs_connection;
inline bool autologin;
inline std::string autologin_email;
inline std::string autologin_password;
inline std::vector<std::string> auto_start_scripts;
inline std::string forced_profile_dir;
inline bool ee_log_in_console;

inline std::string lang_code;
inline std::string webui_lang_code;

// Tunables
inline bool prohibit_skip_mission_start_timer = false;
inline bool prohibit_fov_override = false;
inline bool prohibit_freecam = false;
inline bool prohibit_teleport = false;
inline bool prohibit_scripts = false;
