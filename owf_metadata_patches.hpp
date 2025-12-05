#pragma once

#include <unordered_map>

#include <Mutex.hpp>
#include <Regex.hpp>

#define METADATA_PATCHES true

#if METADATA_PATCHES && SOUP_BITS == 64
struct MetadataPatch
{
	std::string prefix;
	std::vector<std::pair<std::string, std::string>> replacements;
	std::vector<std::pair<soup::Regex, std::string>> substitutions;
	std::vector<std::pair<std::string, std::string>> query_assignments;

	std::string final_data;
	bool is_implicit = false;
	bool applied = false;
};
inline soup::Mutex metadata_patches_mtx;
inline std::unordered_map<uint32_t, MetadataPatch> metadata_patches;
#endif
