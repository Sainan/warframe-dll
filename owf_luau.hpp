#pragma once

#include "owf_structs.hpp"

union luau_Value
{
	uintptr_t as_uintptr;
	int as_bool;
	float as_float;
};

enum luau_Type
{
	LUAU_NIL = 0,
	LUAU_BOOL = 1,
	LUAU_LIGHTUSERDATA = 2,
	LUAU_NUMBER = 3,
	LUAU_STRING = 5,
	LUAU_TABLE = 6,
	LUAU_FUNCTION = 7,
	LUAU_USERDATA = 8,
};

struct luau_TValue
{
	/* 0x00 */ luau_Value value;
	PAD(0x08, 0x0C) uint32_t type;

	[[nodiscard]] char* getString() noexcept
	{
		return reinterpret_cast<char*>(value.as_uintptr + 0x18);
	}

	[[nodiscard]] Object* getObject() const noexcept
	{
		if (is_38_5_0_or_above)
		{
			return **(Object***)(value.as_uintptr + 0x18);
		}
		return ***(Object****)(value.as_uintptr + 0x18);
	}
};
static_assert(sizeof(luau_TValue) == 0x10);

struct luau_State;

// 38.0.x
struct luau_GlobalState_38_0_x
{
	PAD(0x000, 0x018) void* ud;
	PAD(0x020, 0xC10) void* error_longjump_data;
	PAD(0xC18, 0xC50) void(*panic_func)(luau_State* L, int status);
	PAD(0xC58, 0x1168);
};
static_assert(sizeof(luau_GlobalState_38_0_x) == 0x1168);

// 38.5.0
struct luau_GlobalState_38_5_0
{
	PAD(0x000, 0x018) void* ud;
	PAD(0x020, 0xCA8) void* error_longjump_data;
	PAD(0xCA8 + 8, 0xCE8) void(*panic_func)(luau_State* L, int status);
};

struct luau_State
{
	PAD(0, 0x08) luau_TValue* outtop;
	/* 0x10 */ luau_TValue* intop;
	/* 0x18 */ void* global_state;
	/* 0x20 */ void* ci;
	/* 0x28 */ luau_TValue* stack_last;
	/* 0x30 */ luau_TValue* stack;
	PAD(0x38, 0x90);

	[[nodiscard]] SOUP_PURE void*& global_state_error_longjump_data() noexcept
	{
		if (is_38_5_0_or_above)
		{
			return reinterpret_cast<luau_GlobalState_38_5_0*>(global_state)->error_longjump_data;
		}
		return reinterpret_cast<luau_GlobalState_38_0_x*>(global_state)->error_longjump_data;
	}

	[[nodiscard]] SOUP_PURE auto& global_state_panic_func() noexcept
	{
		if (is_38_5_0_or_above)
		{
			return reinterpret_cast<luau_GlobalState_38_5_0*>(global_state)->panic_func;
		}
		return reinterpret_cast<luau_GlobalState_38_0_x*>(global_state)->panic_func;
	}

	luau_TValue* getValue(int idx)
	{
		return idx < 0 ? &outtop[idx] : &intop[idx - 1];
	}
};
static_assert(sizeof(luau_State) == 0x90);

using luau_CFunction = int(*)(luau_State*);
using luau_Alloc = void*(*)(void* ud, void* ptr, size_t osize, size_t nsize);

struct luau_Closure
{
	PAD(0x00, 0x03) uint8_t isC;
	PAD(0x04, 0x18) luau_CFunction func;
};

/*inline void* luau_alloc_impl(void* ud, void* ptr, size_t osize, size_t nsize)
{
	if (nsize == 0)
	{
		soup::free(ptr);
		return nullptr;
	}
	else
	{
		return soup::realloc(ptr, nsize);
	}
}*/

/*using luau_newstate_t = luau_State*(*)(luau_Alloc f, void* ud, char);
inline luau_newstate_t luau_newstate = nullptr;*/

using luau_pushstring_t = const char*(*)(luau_State*, const char*);
inline luau_pushstring_t luau_pushstring = nullptr;

using luau_pushpointer_t = void*(*)(luau_State*, void*);
inline luau_pushpointer_t luau_pushpointer = nullptr;

using luau_pushobject_t = Object*(*)(luau_State*, Object*);
inline luau_pushobject_t luau_pushobject = nullptr;

using luau_gettable_t = int(*)(luau_State*, int idx);
inline luau_gettable_t luau_gettable = nullptr;

using luau_createtable_t = void(*)(luau_State*, int, int);
inline luau_createtable_t luau_createtable = nullptr;

using luau_settable_t = void(*)(luau_State*, int);
inline luau_settable_t luau_settable = nullptr;

using luauD_call_t = int(*)(luau_State* L, luau_TValue* func, int nresults);
inline luauD_call_t luauD_call = nullptr;

inline luau_State* luau_L = nullptr;
//inline Object*** luau_obj_buf[4];
inline std::string luau_error_msg;

struct SwigMethod
{
	uint32_t hash;
	luau_CFunction func;
};
static_assert(sizeof(SwigMethod) == 0x10);

struct SwigAttribute
{
	uint32_t hash;
	luau_CFunction getter;
	luau_CFunction setter;
};
static_assert(sizeof(SwigAttribute) == 0x18);

struct SwigTypeDesc
{
	/* 0x00 */ const char* name; // e.g. "Object"
	PAD(0x08, 0x10) luau_CFunction ctor;
	PAD(0x18, 0x20) SwigMethod* methods;
	/* 0x28 */ SwigAttribute* attributes;
	PAD(0x30, 0x38) const char** parent_ptr_name; // e.g. "Object *"

	luau_CFunction findMethod(uint32_t hash)
	{
		for (auto method = this->methods; method->hash != 0; ++method)
		{
			if (method->hash == hash)
			{
				return method->func;
			}
		}
		return nullptr;
	}

	luau_CFunction findGetter(uint32_t hash)
	{
		for (auto attr = this->attributes; attr->hash != 0; ++attr)
		{
			if (attr->hash == hash)
			{
				return attr->getter;
			}
		}
		return nullptr;
	}

	luau_CFunction findSetter(uint32_t hash)
	{
		for (auto attr = this->attributes; attr->hash != 0; ++attr)
		{
			if (attr->hash == hash)
			{
				return attr->setter;
			}
		}
		return nullptr;
	}
};
static_assert(sizeof(SwigTypeDesc) == 0x40);

struct SwigTypeField
{
	/* 0x00 */ const char* field_name; // e.g. "_p_Object"
	/* 0x08 */ const char* type_name; // e.g. "Object *"
	PAD(0x10, 0x18) SwigTypeDesc* type_desc;
};
static_assert(sizeof(SwigTypeField) == 0x20);

inline std::unordered_map<uint32_t, SwigTypeDesc*> swig_types;

struct SwigEnum
{
	PAD(0, 0x08) const char* name;
	/* 0x10 */ int32_t value;
	PAD(0x14, 0x38);
};
static_assert(sizeof(SwigEnum) == 0x38);

inline std::vector<SwigEnum*> swig_enums;
