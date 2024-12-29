#pragma once

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
	LUAU_USERDATA = 8,
};

struct luau_TValue
{
	/* 0x00 */ luau_Value value;
	PAD(0x08, 0x0C) uint32_t type;

	[[nodiscard]] const char* getString() const noexcept
	{
		return reinterpret_cast<const char*>(value.as_uintptr + 0x18);
	}
};
static_assert(sizeof(luau_TValue) == 0x10);

struct luau_State;

struct luau_GlobalState
{
	PAD(0x000, 0x018) void* ud;
	PAD(0x020, 0xC10) void* error_longjump_data;
	PAD(0xC18, 0xC50) void(*panic_func)(luau_State* L, int status);
	PAD(0xC58, 0x1168);
};
static_assert(sizeof(luau_GlobalState) == 0x1168);

struct luau_State
{
	PAD(0, 0x08) luau_TValue* outtop;
	/* 0x10 */ luau_TValue* intop;
	/* 0x18 */ luau_GlobalState* global_state;
	PAD(0x20, 0x90);

	luau_TValue* getValue(int idx)
	{
		return idx < 0 ? &outtop[idx] : &intop[idx - 1];
	}
};
static_assert(sizeof(luau_State) == 0x90);

using luau_CFunction = int(*)(luau_State*);
using luau_Alloc = void*(*)(void* ud, void* ptr, size_t osize, size_t nsize);

inline void* luau_alloc_impl(void* ud, void* ptr, size_t osize, size_t nsize)
{
	if (nsize == 0)
	{
		free(ptr);
		return nullptr;
	}
	else
	{
		return realloc(ptr, nsize);
	}
}
