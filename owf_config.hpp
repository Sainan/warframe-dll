#pragma once

#include <cstdint>
#include <string>
#include <vector>


// Command line arguments
inline std::string fallback_language;
inline std::string fallback_graphicsDriver;
inline std::string fallback_cluster;

// Server connection
inline std::string server_host;
inline uint16_t http_port;
inline uint16_t https_port;
inline bool autologin;
inline std::string autologin_email;
inline std::string autologin_password;

// Patches
inline bool high_damage_numbers_patch;
inline bool simulacrum_blacklisted;
inline bool simulacrum_whitelisted;
inline bool pause_always_stops_time;
inline bool disable_nrs_connection;

// Features
inline bool ee_log_in_console;
inline bool skip_mission_start_timer;
inline bool logout_on_request_failure;
inline float fov_override;
inline std::string forced_profile_dir;
inline std::vector<std::string> auto_start_scripts;
inline bool dont_resolve_labels;
inline bool save_all_metadata;
inline bool write_all_metadata_reads_to_console;
inline bool write_all_metadata_reads_to_ee_log;
inline bool write_patched_metadata_reads_to_console;
inline bool write_patched_metadata_reads_to_ee_log;


// Effective arguments (not in client_config.json)
inline std::string lang_code;
inline std::string webui_lang_code;
inline std::string graphics_driver;
