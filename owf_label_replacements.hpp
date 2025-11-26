#pragma once

#define LABEL_REPLACEMENTS true

#include <unordered_map>

#include <alloc.hpp>
#include <fnv.hpp>
#include <Mutex.hpp>

struct PermanentString
{
	size_t size;
	char data[1];
};
inline std::unordered_map<uint64_t, PermanentString*> permanent_strings; // mutexed by label_replacements_mtx
inline std::atomic<size_t> fossilised_memory = 0;
inline PermanentString* fossilise_string(const char* data, size_t size)
{
	const auto hash = soup::fnv1a_64(data, size);
	if (auto e = permanent_strings.find(hash); e != permanent_strings.end())
	{
		return e->second;
	}
	auto ps = (PermanentString*)soup::malloc(offsetof(PermanentString, data) + size + 1);
	ps->size = size;
	memcpy(ps->data, data, size);
	ps->data[size] = 0;
	permanent_strings.emplace(hash, ps);

	fossilised_memory += size + 1;

	return ps;
}

inline soup::Mutex label_replacements_mtx;

extern void load_label_replacements();
extern const char* do_label_replacements(const char* str_data, size_t str_size, const char* loctag_data, size_t loctag_size, size_t& out_size);
