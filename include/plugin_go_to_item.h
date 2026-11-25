#pragma once
// Go to item: track one or more ITEMS and show where each is produced. Every
// tracked item has all its sources (one source = one base core that produces
// it). The GoToItem plugin marks every source on the game's full map and draws a
// pinned marker on screen, on foot and in the drone. Callers (Codex, WAILA,
// Production Viewer) only use this header and call GoToItem::Get().
//
// The plugin that answers is GoToItem.dll. GoToItem::Get() looks for it first,
// then for the loader's own module, so the implementation can move into the
// loader later without any caller changing. The header holds no game offsets,
// sizes or patterns: everything version-specific stays in the implementing
// module and its version map keys.
//
// Conventions (same as the rest of plugin_interface.h):
//   - plain C types across the DLL boundary, no STL, no exceptions;
//   - every struct starts with `uint32_t size` = sizeof as the BUILDER of the
//     struct saw it; the other side reads only fields inside that size;
//   - append-only: a new version adds fields at the end of a struct or new
//     function pointers at the end of GoToItemApi; nothing moves or changes;
//   - the caller provides every output buffer; strings are copied and always
//     NUL-terminated (truncated if the buffer is small);
//   - items are named by UAuItemDataBase::UniqueItemName.
//
// Threads: every function is callable from ANY thread (a button click runs on
// the render thread). Changes are queued and applied on the game thread, where
// the UObjects are read; no UObject pointer crosses this API and no caller may
// keep one for it. Query functions read a snapshot refreshed once per game
// tick, so values are at most one tick old. Counts and lists are consistent
// with GetChangeCounter() read just before.
//
// Hot RELOAD: the loader unloads and reloads a plugin DLL without telling the
// other plugins, so the function table can vanish between two calls. Call
// GoToItem::Get() every time you use the API and never keep the pointer past
// the current callback. Get() looks the module up again and checks the version
// on every call, which is cheap.
//
// Needs interface 77 (the item icons use hooks->Images); a loader without
// images still gets every call here, the pins just draw the coloured circle.

#include <cstdint>
#include <cstddef>

#define GOTOITEM_API_VERSION 1

// Exported by the implementing module as an undecorated C function:
//   extern "C" __declspec(dllexport) const GoToItemApi* GoToItem_GetApi(uint32_t wantedVersion);
// Returns null when the module cannot serve wantedVersion or older. A module
// that knows a newer version still serves older callers the same table.
#define GOTOITEM_EXPORT_NAME "GoToItem_GetApi"

// ---------------------------------------------------------------------------
// Limits (enforced by the implementation; the numbers are in the API struct
// so callers read them instead of hard-coding)
// ---------------------------------------------------------------------------
#define GOTOITEM_MAX_TRACKED_ITEMS      7   // one colour each, see GoToItemApi::GetItemColor
#define GOTOITEM_MAX_SOURCES_PER_ITEM  16   // sources kept and marked on the map per item
#define GOTOITEM_MAX_SCREEN_PER_ITEM    3   // nearest sources drawn on screen per item
#define GOTOITEM_KEY_CHARS             96   // UniqueItemName buffer, incl. NUL
#define GOTOITEM_NAME_CHARS            64   // display names, incl. NUL

// ---------------------------------------------------------------------------
// Results. Plain int32_t across the boundary; GoToItemApi::ResultToString
// gives a short player-facing reason.
// ---------------------------------------------------------------------------
enum GoToResult : int32_t
{
	GOTO_OK                 = 0,
	GOTO_ALREADY_TRACKED    = 1,   // success: nothing changed
	GOTO_UNKNOWN_ITEM       = -1,  // name not a known item (empty, or not in the item catalog)
	GOTO_NO_LOCATION_YET    = -2,  // item known, no producing base known yet (CanTrack only; TrackItem accepts it)
	GOTO_FEATURE_OFF        = -3,  // the version map group for lookup is off on this game build
	GOTO_TRACKED_LIMIT      = -4,  // GOTOITEM_MAX_TRACKED_ITEMS reached (untrack one first)
	GOTO_NOT_TRACKED        = -5,
	GOTO_UNKNOWN_SOURCE     = -6,  // source key not in that item's list
	GOTO_NOT_READY          = -7,  // no world yet / not initialised
	GOTO_BAD_ARGUMENT       = -8,
};

// ---------------------------------------------------------------------------
// Data structs
// ---------------------------------------------------------------------------

// One place an item is produced (a base core). 112 bytes.
struct GoToSource
{
	uint32_t size;               // sizeof(GoToSource) as the builder saw it
	uint32_t flags;              // GOTO_SOURCE_* bits
	// The base core's key: its Mass entity handle packed as (SerialNumber << 32)
	// | Index, the same value Production Viewer calls baseKey, so a per-base
	// button there passes it straight to SelectSource. Stable while the base
	// lives in a session; keys do not survive a save load (picks are cleared
	// then). 0 is never a key.
	uint64_t sourceKey;
	double   x, y, z;            // world position, game units (cm)
	float    distanceMeters;     // from the camera / view target; < 0 when unknown
	float    outputPerMinute;    // this source's output of the item; < 0 when unknown
	char     name[GOTOITEM_NAME_CHARS]; // base name, may be empty
};
enum : uint32_t
{
	GOTO_SOURCE_NEAREST  = 1u << 0,   // currently the nearest source of its item
	GOTO_SOURCE_SELECTED = 1u << 1,   // the item's picked source (see SelectSource)
	GOTO_SOURCE_REMOTE   = 1u << 2,   // position came from the server (multiplayer client)
};

// One tracked item. 192 bytes.
struct GoToTrackedItem
{
	uint32_t size;               // sizeof(GoToTrackedItem) as the builder saw it
	uint32_t colorRgba;          // 0xAARRGGBB, fully opaque; same value GetItemColor returns
	uint32_t sourceCount;        // sources known now (not capped by the caller's buffer)
	uint32_t flags;              // GOTO_ITEM_* bits
	uint64_t selectedSourceKey;  // 0 = no pick, follow the nearest
	float    nearestMeters;      // distance to the nearest source; < 0 when unknown
	uint32_t reserved0;
	char     itemKey[GOTOITEM_KEY_CHARS];
	char     displayName[GOTOITEM_NAME_CHARS];
};
enum : uint32_t
{
	GOTO_ITEM_HAS_PICK = 1u << 0,   // selectedSourceKey is set
};

static_assert(sizeof(GoToSource) == 112, "GoToSource v1 layout");
static_assert(offsetof(GoToSource, x) == 16, "GoToSource v1 layout");
static_assert(offsetof(GoToSource, name) == 48, "GoToSource v1 layout");
static_assert(sizeof(GoToTrackedItem) == 192, "GoToTrackedItem v1 layout");
static_assert(offsetof(GoToTrackedItem, itemKey) == 32, "GoToTrackedItem v1 layout");

// ---------------------------------------------------------------------------
// The API. Append-only: new function pointers go at the END; a caller checks
// `size` before using one newer than the header it was built with.
// ---------------------------------------------------------------------------
struct GoToItemApi
{
	uint32_t size;                  // sizeof(GoToItemApi) as the implementation built it
	uint32_t version;               // GOTOITEM_API_VERSION the implementation serves
	uint32_t maxTrackedItems;       // = GOTOITEM_MAX_TRACKED_ITEMS in the implementation
	uint32_t maxSourcesPerItem;     // = GOTOITEM_MAX_SOURCES_PER_ITEM in the implementation

	// --- change detection ---------------------------------------------------
	// Climbs by one whenever the tracked set, any item's sources, a pick or a
	// colour changes. A UI polls this each frame and re-reads lists only when
	// it moved (no per-frame copying, no file I/O).
	uint32_t (*GetChangeCounter)();

	// --- can this item be located? (the Go button's look and tooltip) -------
	// GOTO_OK when at least one source is known now; otherwise the reason
	// (GOTO_NO_LOCATION_YET while no producing base is known). Only for how the
	// button looks: TrackItem does not require it.
	int32_t (*CanTrack)(const char* itemKey);

	// --- tracked set ----------------------------------------------------------
	// Add an item to the set. Any known item is accepted, also one with no source
	// yet (after a game load nothing is known until the factories finish a craft
	// cycle): it waits in the set, and its pins and map marks appear once a
	// source is known. All its sources are marked on the full map and pinned on
	// screen. New items default to "nearest source" mode. GOTO_UNKNOWN_ITEM for
	// an empty key or, once the item catalog is loaded, a key not in it.
	int32_t (*TrackItem)(const char* itemKey);
	// Remove one item (its map marks and screen pins disappear). Result
	// GOTO_NOT_TRACKED if it was not tracked.
	int32_t (*UntrackItem)(const char* itemKey);
	// Remove all items.
	void    (*ClearTracked)();
	// 1 if tracked, else 0.
	int32_t (*IsTracked)(const char* itemKey);

	// Copies up to `max` tracked items into `out` (stride = elementSize, the
	// caller's sizeof(GoToTrackedItem)); returns how many tracked items exist
	// in all, which may exceed `max`. out may be null with max 0 to just count.
	uint32_t (*ListTracked)(GoToTrackedItem* out, uint32_t elementSize, uint32_t max);

	// Sources of an item, nearest first (so index 0 is the nearest while no
	// pick is set), capped at maxSourcesPerItem. Same copy and return
	// convention as ListTracked. Works for an item that is not tracked too
	// (feeds a picker before the user commits).
	uint32_t (*ListSources)(const char* itemKey, GoToSource* out, uint32_t elementSize, uint32_t max);

	// --- nearest + picker -----------------------------------------------------
	// sourceKey == 0 returns the item to "nearest" mode. A non-zero key pins the
	// primary target to that source; every source is still marked on the map.
	// GOTO_UNKNOWN_SOURCE if the key is not in the item's list. The exception is
	// a remote client (a joined game), whose list comes from the server and may
	// not have arrived yet: a key not listed yet is accepted (GOTO_OK), held, and
	// picked once its source appears; until then the item shows its nearest.
	int32_t (*SelectSource)(const char* itemKey, uint64_t sourceKey);

	// --- colour ----------------------------------------------------------------
	// The item's circle colour, 0xAARRGGBB opaque. Stable per item: derived
	// from a hash of the key, skipping colours another tracked item holds, so
	// the same item gets the same colour whenever the set allows. 0 when the
	// item is not tracked and has no override.
	uint32_t (*GetItemColor)(const char* itemKey);
	// Optional user override (a colour pick in a settings page), stored per item
	// key independent of tracking: it is kept when the item is untracked and
	// applies again when it is tracked again. Only rgba == 0 clears it. Works for
	// an untracked item too; GOTO_UNKNOWN_ITEM as for TrackItem. Persisting it is
	// the implementation's job (written on change, never per frame).
	int32_t  (*SetItemColor)(const char* itemKey, uint32_t colorRgba);

	// --- text ------------------------------------------------------------------
	// Short player-facing reason for a GoToResult, valid forever (static).
	const char* (*ResultToString)(int32_t result);

	// Appended in later versions go below this line.
};

// ---------------------------------------------------------------------------
// Client wrapper. Header-only.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace GoToItem
{
	typedef const GoToItemApi* (*GetApiFn)(uint32_t wantedVersion);

	// The function table, or null when GoToItem is not loaded, does not export
	// the lookup, or cannot serve GOTOITEM_API_VERSION. Looks the module up on
	// every call (see "Hot RELOAD" above): do not cache the result across
	// callbacks. Null means: hide the Go button, and say in its tooltip that the
	// GoToItem plugin is needed.
	inline const GoToItemApi* Get()
	{
		static const char* const kModules[] = { "GoToItem.dll", "StarRupture-ModLoader-Core.dll" };
		for (const char* name : kModules)
		{
			HMODULE module = GetModuleHandleA(name);
			if (!module)
				continue;
			GetApiFn fn = reinterpret_cast<GetApiFn>(GetProcAddress(module, GOTOITEM_EXPORT_NAME));
			if (!fn)
				continue;
			const GoToItemApi* api = fn(GOTOITEM_API_VERSION);
			if (api && api->version >= 1 && api->size >= offsetof(GoToItemApi, ResultToString) + sizeof(void*))
				return api;
		}
		return nullptr;
	}

	// True when `api` is new enough to hold the function pointer at `memberOffset`
	// (use GOTOITEM_HAS(api, Member) below before calling anything added after v1).
	inline bool Has(const GoToItemApi* api, size_t memberOffset, size_t memberSize)
	{
		return api && api->size >= memberOffset + memberSize;
	}
}

#define GOTOITEM_HAS(api, member) \
	GoToItem::Has((api), offsetof(GoToItemApi, member), sizeof(((GoToItemApi*)nullptr)->member))
#endif // _WIN32
