#pragma once

#include <structing.hpp>

inline char build_label[16] = { 0 }; // e.g. "2024.12.14.10.37"
inline bool is_38_5_0_or_above = false;
inline bool is_37_0_0_or_above = false;
inline bool is_35_5_0_or_above = false;
inline bool is_31_5_0_or_above = false;
inline bool is_28_0_0_or_above = false;
inline bool is_26_1_0_or_above = false;
inline bool is_25_0_0_or_above = false;

inline char build_hash[22] = { 0 };

union GameString
{
	struct
	{
		char data[15];
		uint8_t inv_len;
	} shrt;
	struct
	{
		char* ptr;
		uint64_t metadata;
	} lng;

	[[nodiscard]] bool isLong() const noexcept { return shrt.inv_len == 0xFF; }
	[[nodiscard]] bool willFreeData() const noexcept { return isLong() && (lng.metadata & 0xFFFFFFF0000000ull) != 0xFFFFFFF0000000ull; }
	[[nodiscard]] char* getData() noexcept { return isLong() ? lng.ptr : shrt.data; }
	[[nodiscard]] size_t getSize() const noexcept { return isLong() ? (lng.metadata & 0xFFFFFFF) : (sizeof(shrt.data) - shrt.inv_len); }

	void setUnownedData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			lng.ptr = (char*)data;
			lng.metadata = 0xFF'FFFFFFF'0000000ull | (len & 0xFFFFFFF);
		}
		else
		{
			memcpy(shrt.data, data, len);
			shrt.data[len] = 0;
			shrt.inv_len = sizeof(shrt.data) - len;
		}
	}

	void setShortData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			len = sizeof(shrt.data);
		}
		memcpy(shrt.data, data, len);
		shrt.data[len] = 0;
		shrt.inv_len = sizeof(shrt.data) - len;
	}

	void setShortData(const std::string& str) noexcept
	{
		return setShortData(str.data(), str.size());
	}

	/*void clear() noexcept
	{
		shrt.data[0] = '\0';
		shrt.inv_len = sizeof(shrt.data);
	}*/
};
static_assert(sizeof(GameString) == 0x10);

#define uses_legacy_game_string !is_35_5_0_or_above

union LegacyGameString
{
	struct
	{
		char data[31];
		uint8_t inv_len;
	} shrt;
	struct
	{
		char* ptr;
		uint32_t len;
		uint32_t ownership;
	} lng;

	[[nodiscard]] bool isLong() const noexcept { return shrt.inv_len == 0xFF; }
	[[nodiscard]] bool willFreeData() const noexcept { return isLong() && lng.ownership != -1; }
	[[nodiscard]] char* getData() noexcept { return isLong() ? lng.ptr : shrt.data; }
	[[nodiscard]] size_t getSize() const noexcept { return isLong() ? lng.len : (sizeof(shrt.data) - shrt.inv_len); }

	void setUnownedData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			lng.ptr = (char*)data;
			lng.len = len;
			lng.ownership = -1;
		}
		else
		{
			memcpy(shrt.data, data, len);
			shrt.data[len] = 0;
			shrt.inv_len = sizeof(shrt.data) - len;
		}
	}

	void setShortData(const char* data, size_t len) noexcept
	{
		if (len > sizeof(shrt.data))
		{
			len = sizeof(shrt.data);
		}
		memcpy(shrt.data, data, len);
		shrt.data[len] = 0;
		shrt.inv_len = sizeof(shrt.data) - len;
	}

	void setShortData(const std::string& str) noexcept
	{
		return setShortData(str.data(), str.size());
	}

	/*void clear() noexcept
	{
		shrt.data[0] = '\0';
		shrt.inv_len = sizeof(shrt.data);
	}*/
};

// Update 37-38
struct ArgumentsU37
{
	PAD(0, 0x04) bool silent;
	PAD(0x05, 0x18) bool client;
	PAD(0x19, 0x140) bool got_debugSession;
	/* 0x148 */ GameString debugSession;
	/* 0x158 */ bool got_clientType;
	/* 0x160 */ GameString clientType;
	PAD(0x160 + sizeof(GameString), 0x189) bool got_graphicsDriver;
	/* 0x190 */ GameString graphicsDriver;
	PAD(0x1A0, 0x1AC) bool got_language;
	/* 0x1B0 */ GameString language;
	/* 0x1C0 */ bool got_cluster;
	/* 0x1C8 */ GameString cluster;
	/* 0x1D8 */ GameString relaunch;
};
static_assert(offsetof(ArgumentsU37, got_graphicsDriver) == 0x189);
static_assert(offsetof(ArgumentsU37, graphicsDriver) == 0x190);
static_assert(offsetof(ArgumentsU37, got_language) == 0x1AC);
static_assert(offsetof(ArgumentsU37, language) == 0x1B0);
static_assert(offsetof(ArgumentsU37, got_cluster) == 0x1C0);
static_assert(offsetof(ArgumentsU37, cluster) == 0x1C8);
static_assert(offsetof(ArgumentsU37, relaunch) == 0x1D8);

struct ArgumentsU36
{
	PAD(0, 0x171) bool got_graphicsDriver;
	/* 0x178 */ GameString graphicsDriver;
	PAD(0x178 + sizeof(GameString), 0x194) bool got_language;
	/* 0x198 */ GameString language;
	PAD(0x198 + sizeof(GameString), 0x1A8) bool got_cluster;
	/* 0x1B0 */ GameString cluster;
};
static_assert(offsetof(ArgumentsU36, got_graphicsDriver) == 0x171);
static_assert(offsetof(ArgumentsU36, graphicsDriver) == 0x178);
static_assert(offsetof(ArgumentsU36, got_language) == 0x194);
static_assert(offsetof(ArgumentsU36, language) == 0x198);
static_assert(offsetof(ArgumentsU36, got_cluster) == 0x1A8);
static_assert(offsetof(ArgumentsU36, cluster) == 0x1B0);

struct LegacyArguments
{
	PAD(0, 0x263) bool got_graphicsDriver;
	/* 0x268 */ LegacyGameString graphicsDriver;
	PAD(0x268 + sizeof(LegacyGameString), 0x294) bool got_language;
	/* 0x298 */ LegacyGameString language;
	PAD(0x298 + sizeof(LegacyGameString), 0x2B8) bool got_cluster;
	/* 0x2C0 */ LegacyGameString cluster;
};
static_assert(offsetof(LegacyArguments, got_graphicsDriver) == 0x263);
static_assert(offsetof(LegacyArguments, graphicsDriver) == 0x268);
static_assert(offsetof(LegacyArguments, got_language) == 0x294);
static_assert(offsetof(LegacyArguments, language) == 0x298);
static_assert(offsetof(LegacyArguments, got_cluster) == 0x2B8);
static_assert(offsetof(LegacyArguments, cluster) == 0x2C0);

struct LegacyArgumentsU30
{
	PAD(0, 0x263) bool got_graphicsDriver;
	/* 0x268 */ LegacyGameString graphicsDriver;
	PAD(0x268 + sizeof(LegacyGameString), 0x28A) bool got_language;
	/* 0x290 */ LegacyGameString language;
	/* 0x2B0 */ bool got_cluster;
	/* 0x2B8 */ LegacyGameString cluster;
};
static_assert(offsetof(LegacyArgumentsU30, got_graphicsDriver) == 0x263);
static_assert(offsetof(LegacyArgumentsU30, graphicsDriver) == 0x268);
static_assert(offsetof(LegacyArgumentsU30, got_language) == 0x28A);
static_assert(offsetof(LegacyArgumentsU30, language) == 0x290);
static_assert(offsetof(LegacyArgumentsU30, got_cluster) == 0x2B0);
static_assert(offsetof(LegacyArgumentsU30, cluster) == 0x2B8);

// 2020.03.24.20.24
struct LegacyArgumentsU27
{
	inline static bool got_graphicsDriver;
	inline static GameString graphicsDriver;

	PAD(0, 0x24B) bool got_language;
	/* 0x250 */ LegacyGameString language;
	/* 0x270 */ bool got_cluster;
	/* 0x278 */ LegacyGameString cluster;
};
static_assert(offsetof(LegacyArgumentsU27, got_language) == 0x24B);
static_assert(offsetof(LegacyArgumentsU27, language) == 0x250);
static_assert(offsetof(LegacyArgumentsU27, got_cluster) == 0x270);
static_assert(offsetof(LegacyArgumentsU27, cluster) == 0x278);

// 2019.05.22.23.12, 2019.09.09.12.43, 2019.10.31.22.42
struct LegacyArgumentsU25
{
	inline static bool got_graphicsDriver;
	inline static GameString graphicsDriver;

	PAD(0, 0x249) bool got_language;
	/* 0x250 */ LegacyGameString language;
	/* 0x270 */ bool got_cluster;
	/* 0x278 */ LegacyGameString cluster;
};
static_assert(offsetof(LegacyArgumentsU25, got_language) == 0x249);
static_assert(offsetof(LegacyArgumentsU25, language) == 0x250);
static_assert(offsetof(LegacyArgumentsU25, got_cluster) == 0x270);
static_assert(offsetof(LegacyArgumentsU25, cluster) == 0x278);

// 2019.04.04.21.31
struct LegacyArgumentsU24
{
	inline static bool got_graphicsDriver;
	inline static GameString graphicsDriver;

	PAD(0, 0x223) bool got_language;
	/* 0x228 */ LegacyGameString language;
	/* 0x248 */ bool got_cluster;
	/* 0x250 */ LegacyGameString cluster;
};
static_assert(offsetof(LegacyArgumentsU24, got_language) == 0x223);
static_assert(offsetof(LegacyArgumentsU24, language) == 0x228);
static_assert(offsetof(LegacyArgumentsU24, got_cluster) == 0x248);
static_assert(offsetof(LegacyArgumentsU24, cluster) ==  0x250);

// Objects

struct ObjectTypeName
{
	uint32_t path_handle;
	uint32_t name_handle;
};

struct ObjectType
{
	PAD(0x00, 0x10) uint32_t* path_handle;
	PAD(0x18, 0x2C) uint32_t name_handle;

	uint32_t getPathHandle() const noexcept
	{
		return path_handle ? *path_handle : 0;
	}
};

struct Object
{
	/* 0x00 */ void* vftable;
	/* 0x08 */ ObjectType* type;
	/* 0x10 */ Object** self_pointer;
	/* 0x18 */ uint32_t id;
	/* 0x1C */ uint32_t path_handle;
};

struct WeaponEx : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x970) Object*(*GetActiveImpactBehavior)(WeaponEx*, void*);
	};

	INIT_PAD(Object, 0x8A0) void* unk_impact_behavior_data;

	Object* GetActiveImpactBehavior() { return reinterpret_cast<Vftable*>(vftable)->GetActiveImpactBehavior(this, unk_impact_behavior_data); }
};

struct LotusInventoryController : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x250) void(*RemoveItem)(LotusInventoryController*, uint8_t slot, bool); // from BaseInventoryController
		PAD(0x258, 0x2C8) Object*(*GetWeaponInHand)(LotusInventoryController*, uint32_t hand); // from BaseInventoryController
		PAD(0x2D0, 0x8E0) Object*(*GetActivePowerSuit)(LotusInventoryController*);
	};

	void RemoveItem(uint8_t slot, bool b) { return reinterpret_cast<Vftable*>(vftable)->RemoveItem(this, slot, b); }
	Object* GetWeaponInHand(uint32_t hand) { return reinterpret_cast<Vftable*>(vftable)->GetWeaponInHand(this, hand); }
	Object* GetActivePowerSuit() { return reinterpret_cast<Vftable*>(vftable)->GetActivePowerSuit(this); }
};

struct BaseEntity : public Object
{
};

struct Entity : public BaseEntity
{
	INIT_PAD(BaseEntity, 0x48) float mov_dir_x;
	/* 0x4C */ float mov_dir_y;
	/* 0x50 */ float mov_dir_z;
	PAD(0x54, 0x70) float pos_x;
	/* 0x74 */ float pos_y;
	/* 0x78 */ float pos_z;
	PAD(0x7C, 0xA0) float rot_x;
	/* 0xA4 */ float rot_y;
	/* 0xA8 */ float rot_z;
	PAD(0xAC, 0xD0) float body_pos_x;
	/* 0xD4 */ float body_pos_y;
	/* 0xD8 */ float body_pos_z;
	PAD(0x0DC, 0x0F0) float vel_x;
	/* 0xF4 */ float vel_y;
	/* 0xF8 */ float vel_z;
	PAD(0x0FC, 0x100) float pos2_x;
	/* 0x104 */ float pos2_y;
	/* 0x108 */ float pos2_z;
	PAD(0x10C, 0x110) float vis_x;
	/* 0x114 */ float vis_y;
	/* 0x118 */ float vis_z;
};

struct UnkControlsArg
{
};

struct BaseAvatar : public Entity
{
	struct Vftable
	{
		PAD(0, 0x610) void(*disableJumping)(BaseAvatar*, UnkControlsArg*);
		/* 0x618 */ void(*enableJumping)(BaseAvatar*, UnkControlsArg*);
		PAD(0x620, 0x8C8) Object*(*getDamageController)(BaseAvatar*);
		PAD(0x8D0, 0x8E8) Object*(*getInputController)(BaseAvatar*);
		PAD(0x8F0, 0x8F8) LotusInventoryController*(*getInventoryController)(BaseAvatar*);
		PAD(0x900, 0xC40) void(*Suicide)(BaseAvatar*);
	};
	static_assert(sizeof(Vftable) == 0xC40 + 8);

	Object* getDamageController() { return reinterpret_cast<Vftable*>(vftable)->getDamageController(this); }
	LotusInventoryController* getInventoryController() { return reinterpret_cast<Vftable*>(vftable)->getInventoryController(this); }
};

struct Avatar : public BaseAvatar
{
	// 38.0.x
	// INIT_PAD(BaseAvatar, 0x500) float head_pos_x;
	// /* 0x504 */ float head_pos_y;
	// /* 0x508 */ float head_pos_z;
	// PAD(0x50C, 0x511) bool followed_by_camera;
	// PAD(0x512, 0x679) uint8_t movement_flags; // 2 = sprinting, 4 = crouching, 5 = sliding
	// PAD(0x67A, 0x6A0) bool render_above_everything;

	[[nodiscard]] SOUP_PURE bool& followed_by_camera() noexcept
	{
		if (is_38_5_0_or_above)
		{
			return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x4C1);
		}
		return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x511);
	}
};

struct LotusAvatar : public Avatar
{
	INIT_PAD(Avatar, 0x6D8) uint32_t relationship_group; // if equal between two avatars, they are friendlies (IsAvatarFriendly; ee0bc178)
};

struct Player : public Object
{
	INIT_PAD(Object, 0x038) GameString name;
	PAD(0x048, 0x068) GameString name_with_platform_suffix;
	PAD(0x078, 0x090) GameString clan_name;
	PAD(0x0A0, 0x148) Avatar** avatar;
	PAD(0x150, 0x158) bool controlling_camera;
	PAD(0x159, 0x1A0) GameString mm_value;
	PAD(0x1B0, 0x1C0) GameString account_id;
	PAD(0x1D0, 0x13E0) UnkControlsArg unk_controls_arg;

	[[nodiscard]] Avatar* getAvatar() const noexcept { return *avatar; }
};
static_assert(offsetof(Player, controlling_camera) == 0x158);

struct Camera : public Entity
{
};

struct LotusGameRules : public Object
{
};

// Most of what we access here is actually on RegionMgrImpl
struct RegionMgr : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x3B0) Camera*(*GetGameCamera)(RegionMgr*);
		PAD(0x3B8, 0x3F8) Player*(*GetLocalPlayer)(RegionMgr*);
		/* 0x400 */ Avatar*(*GetLocalPlayerAvatar)(RegionMgr*);
	};

	Camera* GetGameCamera() { return reinterpret_cast<Vftable*>(vftable)->GetGameCamera(this); }
	Player* GetLocalPlayer() { return reinterpret_cast<Vftable*>(vftable)->GetLocalPlayer(this); }
	Avatar* GetLocalPlayerAvatar() { return reinterpret_cast<Vftable*>(vftable)->GetLocalPlayerAvatar(this); }

	INIT_PAD(Object, 0x208) Player*** local_player;
	PAD(0x210, 0x218) LotusGameRules** game_rules;
	PAD(0x220, 0x2C8) Camera** game_camera;
};
static_assert(sizeof(RegionMgr) == 0x2C8 + 8);

struct StringPoolBucket
{
	char* data;
	size_t unk;
};
inline StringPoolBucket** string_pool;
inline const char* resolve_string_handle(uint32_t handle)
{
	return &(*string_pool)[handle & 0xffff].data[handle >> 16];
}

struct ScriptInstance
{
	/* 0x00 */ void* vftable;
	/* 0x08 */ ObjectType* script_type;
	PAD(0x10, 0x20) uint32_t func_name_handle;
};

struct TextureLayer
{
	/* 0x00 */ void* data;
	/* 0x08 */ uint32_t depthPitch; // data size
	/* 0x0C */ uint32_t rowPitch;
	PAD(0x10, 0x18);
};
static_assert(sizeof(TextureLayer) == 0x18);

struct Texture : public Object
{
	struct Vftable
	{
		PAD(0x000, 0x1B8) uint32_t(*getArraySize)(Texture*);
	};

	auto getArraySize() { return reinterpret_cast<Vftable*>(vftable)->getArraySize(this); }

	INIT_PAD(Object, 0x60) uint8_t miplevels;
	PAD(0x61, 0x68) uint16_t width;
	/* 0x6A */ uint16_t height;
};

struct DxTexture
{
	PAD(0x00, 0x90) Texture** object;
	/* 0x98 */ uint32_t total_size_bytes;
	PAD(0x9C, 0xA8) TextureLayer* layers;
};
static_assert(sizeof(DxTexture) == 0xB0);

struct CacheReader
{
	struct Vtbl
	{
		PAD(0, 0x58) void(*read)(CacheReader*, void* data, uint32_t size);
	};

	Vtbl* vtbl;
};


// Added in 38.5.0
struct EncryptedString
{
	struct AppendData
	{
		/* 0x00 */ const char* data;
		/* 0x08 */ uint32_t size;
	};

	/* 0x00 */ AppendData* app;
	PAD(0x08, 0x58) GameString out_buf;
};
static_assert(offsetof(EncryptedString, out_buf) == 0x58);


inline RegionMgr* regionmgr = nullptr;
//inline LotusGameRules* gamerules;
inline Object* flashmgr = nullptr;
inline Object* gamedata = nullptr;
inline Object* profilemgr = nullptr;
inline Object* gClient = nullptr;
inline void* matchingservice = nullptr;
