#pragma once
// Go to item: track one or more ITEMS and show where each is produced. Every
// tracked item has all its sources (one source = one base core that produces
// it). The GoToItem plugin marks every source on the game's full map and draws a
// pinned marker on screen, on foot and in the drone. Callers (Codex, WAILA,
// Production Viewer) only use this header: GoToItem::GetIfOn() answers "show my
// Go button?" and returns the table to call. Read "Threads and RELOAD" below
// first. A worked example follows the API struct.
//
// The plugin that answers is GoToItem.dll. GoToItem::Get() looks for it first,
// then for the loader's own module, so the implementation can move into the
// loader later without any caller changing. The header holds no game offsets,
// sizes or patterns: everything version-specific stays in the implementing
// module and its version object map keys.
//
// Conventions (same as the rest of plugin_interface.h):
//   - plain C types across the DLL boundary, no STL, no exceptions;
//   - every struct starts with `uint32_t size` = sizeof as the BUILDER of the
//     struct saw it; the other side reads only fields inside that size;
//   - append-only: a new version adds fields at the end of a struct or new
//     function pointers at the end of GoToItemApi; nothing moves or changes;
//   - a struct the CALLER passes in (GoToPlace) is read only up to its own `size`,
//     and `size` is its first field, so it can grow at the end without breaking a
//     caller built earlier; a size below the version-2 minimum is
//     GOTO_BAD_ARGUMENT. An array the caller receives (GoToTarget, GoToSource) is
//     written at the caller's element size as the stride, at most that many bytes
//     per element, so a caller built with an older, shorter struct still reads it
//     right; new fields go only at the end;
//   - the caller provides every output buffer; strings are copied and always
//     NUL-terminated (truncated if the buffer is small);
//   - items are named by an item key: UAuItemDataBase::UniqueItemName, or,
//     when that is empty or "None" (ammo, blueprint items), the item's class
//     name without the trailing "_C". Pass the key as GoToItem reports it, or
//     build it the same way; the raw UniqueItemName alone does not match those
//     items.
//
// Threads and RELOAD: use everything here from the GAME THREAD ONLY (your engine
// tick), Get() and GetIfOn() included.
//   - The loader can unload and reload GoToItem.dll without telling the other
//     plugins.
//   - ModLoader 1.28.0 frees no plugin DLL while a call the loader made into any
//     plugin is still running. So Get() followed by use of the table inside the
//     same loader callback (your tick, a loader event, a PostToGameThread task)
//     cannot hit a freed GoToItem.
//   - ModLoader v1.22.0 and later run RELOAD on the game thread, so on them too
//     game-thread code that takes the table and uses it right away cannot be
//     cut off. Older ones ran the overlay RELOAD button on the render thread,
//     so on them any call can be.
//   - Not safe on any ModLoader: a table kept between callbacks, and a call made
//     off the game thread, from a thread you start or a detour that runs there
//     (the loader does not see these calls, so it does not wait for them).
//   - A GoToItem the loader shows as "Shut down" has had its PluginShutdown but
//     is not freed yet: a call was still running when the wait to free it ran
//     out. The loader has removed what it had registered by then. A call that
//     was still running can register or post things afterwards, and those can
//     still reach it until it is freed. Get() can still return its table, and
//     what the table answers then is up to GoToItem. A later UNLOAD or RELOAD
//     can free it, so the rule above about one loader callback still applies.
//   - A UI or draw callback can run off the game thread, so never call from one,
//     on any ModLoader: a DLL that stays loaded does not make such a call safe.
//     Store the click, act on it in your tick, and have the tick publish (in an
//     atomic) whether the UI shows the button. See the worked example.
//   - Take the table again for each use. Never keep it, or any pointer it
//     returns (ResultToString's text too), after the callback you got it in
//     returns: after a RELOAD it points into a DLL that is gone. Copy text you
//     need to show later.
//   - A place key (TrackPlace) is never given out twice while the game runs, a
//     RELOAD of GoToItem included, so a key you kept can never close a newer
//     place. An implementation must keep this.
//
// No UObject pointer crosses this API and no caller may keep one for it. A
// change may take effect on the next game tick. Query values are at most one
// tick old, and counts and lists are consistent with GetChangeCounter() read
// just before.
//
// Version 2 adds targets: a target is a (kind, key) pair, and an item is one kind
// of it. The other kinds are things the game map shows (GOTO_KIND_MAP), ore nodes
// (GOTO_KIND_ORE) and a place a caller names (GOTO_KIND_PLACE). The v1 item calls
// keep working and act on kind GOTO_KIND_ITEM. Get() asks for version 1 and gets
// the newest table; check every v2 member with GOTOITEM_HAS before using it: an
// older GoToItem has a shorter table.
//
// When to hide Go:
//   - GetIfOn() is null: GoToItem is not installed, is not loaded (disabled,
//     unloaded, between the two halves of a RELOAD) or is turned off. Ask again
//     on a later tick: it can come back without a restart.
//   - For an item, CanTrack returns anything but GOTO_OK or GOTO_NO_LOCATION_YET.
//     GOTO_FEATURE_OFF means item lookup is off on this game build (TrackItem
//     returns it too).
//
// Turned off, what an implementation must do: every function that returns a
// result code, version 1 and 2, returns GOTO_OFF and changes nothing: CanTrack,
// TrackItem, UntrackItem, IsTracked, SelectSource, SetItemColor and every
// version-2 function. So IsTracked and IsTrackedTarget are NOT yes/no answers:
// GOTO_OFF is non-zero. Compare their value with 1. The other functions cannot
// report that GoToItem is off, so never use them to detect it. ClearTracked does
// nothing while off: it writes no config and keeps the tracked set. GetIfOn()
// asks IsTrackedTarget. A version-1 GoToItem has no off state.

#include <cstdint>
#include <cstddef>

#define GOTOITEM_API_VERSION 2

// Exported by the implementing module as an undecorated C function:
//   extern "C" __declspec(dllexport) const GoToItemApi* GoToItem_GetApi(uint32_t wantedVersion);
// Returns null when the module cannot serve wantedVersion or older. A module
// that knows a newer version still serves older callers the same table.
#define GOTOITEM_EXPORT_NAME "GoToItem_GetApi"

// ---------------------------------------------------------------------------
// Limits (enforced by the implementation; the numbers are in the API struct
// so callers read them instead of hard-coding)
// ---------------------------------------------------------------------------
#define GOTOITEM_MAX_TRACKED_ITEMS      7   // the fewest a provider allows; colours repeat after 7
#define GOTOITEM_MAX_SOURCES_PER_ITEM  16   // most sources a list call returns per item
#define GOTOITEM_MAX_SCREEN_PER_ITEM    3   // nearest sources drawn on screen per item
#define GOTOITEM_KEY_CHARS             96   // UniqueItemName buffer, incl. NUL
#define GOTOITEM_NAME_CHARS            64   // display names, incl. NUL

// Version 2: the kind of a target (GoToTarget::kind and the `kind` argument of
// the v2 calls). The tracked limit above counts every kind together.
#define GOTO_KIND_ITEM   0u   // key = item key; the same entries as the v1 item calls
#define GOTO_KIND_MAP    1u   // key = a map entry kind, e.g. "poi.Cave", "building.BaseCore", "foundable.Drone", "pin", "player"
#define GOTO_KIND_ORE    2u   // key = "ore.<resource>"
#define GOTO_KIND_PLACE  3u   // key = the decimal place key TrackPlace gave back
// A place closes by itself when the player (the character, not the camera) comes
// within this many metres of it.
#define GOTO_PLACE_ARRIVE_METERS 10

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
	GOTO_FEATURE_OFF        = -3,  // item lookup is off on this game build (version map)
	GOTO_TRACKED_LIMIT      = -4,  // the provider's cap (GoToItemApi::maxTrackedItems) reached (untrack one first)
	GOTO_NOT_TRACKED        = -5,
	GOTO_UNKNOWN_SOURCE     = -6,  // no longer returned by SelectSource; kept so the numbers stay put
	GOTO_NOT_READY          = -7,  // no world yet / not initialised
	GOTO_BAD_ARGUMENT       = -8,
	// Version 2:
	GOTO_UNKNOWN_TARGET     = -9,  // the key is not valid for that kind, or the kind is unknown
	GOTO_OFF                = -10, // turned off right now (see "Turned off" at the top)
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

// Version 2. A place a caller wants marked: a fixed point, or an actor followed
// by its object reference (index and serial, as the loader's object lookups give
// them). `size` is first and the struct only grows at the end; GoToItem reads
// nothing past `size`.
struct GoToPlace
{
	uint32_t size;                              // sizeof(GoToPlace) as the builder saw it
	uint32_t flags;                             // 0 for now
	char     name[GOTOITEM_NAME_CHARS];         // e.g. "Fabricator v.2"
	char     iconItemKey[GOTOITEM_KEY_CHARS];   // optional item key for the icon; "" = a plain disc
	double   x, y, z;                           // world position, cm; used when actorIndex is -1, and as the first position
	int32_t  actorIndex, actorSerial;           // follow this actor; -1 = a fixed point
};

// Version 2. One tracked target of any kind (an item as well).
struct GoToTarget
{
	uint32_t size;                              // sizeof(GoToTarget) as the builder saw it
	uint32_t kind;                              // GOTO_KIND_*
	char     key[GOTOITEM_KEY_CHARS];
	char     displayName[GOTOITEM_NAME_CHARS];
	uint32_t colorRgba;                         // 0xAARRGGBB, opaque
	uint64_t selectedSourceKey;                 // 0 = follow the nearest location
	float    nearestMeters;                     // < 0 = no location known yet
	uint32_t sourceCount;                       // locations known now
};

static_assert(sizeof(GoToSource) == 112, "GoToSource v1 layout");
static_assert(offsetof(GoToSource, x) == 16, "GoToSource v1 layout");
static_assert(offsetof(GoToSource, name) == 48, "GoToSource v1 layout");
static_assert(sizeof(GoToTrackedItem) == 192, "GoToTrackedItem v1 layout");
static_assert(offsetof(GoToTrackedItem, itemKey) == 32, "GoToTrackedItem v1 layout");
static_assert(sizeof(GoToPlace) == 200, "GoToPlace v2 layout");
static_assert(offsetof(GoToPlace, x) == 168, "GoToPlace v2 layout");
static_assert(offsetof(GoToPlace, actorIndex) == 192, "GoToPlace v2 layout");
static_assert(sizeof(GoToTarget) == 192, "GoToTarget v2 layout");
static_assert(offsetof(GoToTarget, colorRgba) == 168, "GoToTarget v2 layout");
static_assert(offsetof(GoToTarget, selectedSourceKey) == 176, "GoToTarget v2 layout");

// ---------------------------------------------------------------------------
// The API. Append-only: new function pointers go at the END; a caller checks
// `size` before using one newer than the header it was built with.
// ---------------------------------------------------------------------------
struct GoToItemApi
{
	uint32_t size;                  // sizeof(GoToItemApi) as the implementation built it
	uint32_t version;               // GOTOITEM_API_VERSION the implementation serves
	uint32_t maxTrackedItems;       // the live cap, at least GOTOITEM_MAX_TRACKED_ITEMS
	uint32_t maxSourcesPerItem;     // = GOTOITEM_MAX_SOURCES_PER_ITEM in the implementation

	// --- change detection ---------------------------------------------------
	// Climbs by one whenever the tracked set, any item's sources, a pick or a
	// colour changes. Poll it in your engine tick, re-read lists only when it
	// moved and hand your UI a copy (no per-frame copying, no file I/O).
	uint32_t (*GetChangeCounter)();

	// --- can this item be located? (the Go button's look and tooltip) -------
	// GOTO_OK when at least one source is known now (a joined multiplayer client
	// also gets it for a known item it has no sources for yet); otherwise the
	// reason: GOTO_NO_LOCATION_YET while no producing base is known,
	// GOTO_FEATURE_OFF when item lookup is off on this game build, GOTO_OFF while
	// turned off, GOTO_NOT_READY with no world yet, GOTO_UNKNOWN_ITEM or
	// GOTO_BAD_ARGUMENT for a bad key. Show Go on GOTO_OK or
	// GOTO_NO_LOCATION_YET only: TrackItem does not require GOTO_OK.
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
	// Remove all items. Does nothing while turned off.
	void    (*ClearTracked)();
	// 1 if tracked, 0 if not, GOTO_OFF while turned off. NOT a yes/no answer:
	// test IsTracked(key) == 1, because any other non-zero value is not "tracked".
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
	// A key that is not in the item's list yet (a base not seen so far, or a
	// joined client whose list has not arrived) is accepted (GOTO_OK), kept, and
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
	// Short player-facing reason for a GoToResult. The text lives in GoToItem.dll:
	// use or copy it right away, never keep the pointer (a RELOAD frees it).
	const char* (*ResultToString)(int32_t result);

	// --- version 2: targets ------------------------------------------------------
	// Check each member with GOTOITEM_HAS before calling it. `kind` is GOTO_KIND_*;
	// ITEM goes to the same entry as the v1 item calls. Every call returns GOTO_OFF
	// while GoToItem is turned off, and GOTO_UNKNOWN_TARGET for a key the kind does
	// not have (a MAP or ORE key is accepted while the first scan has not finished).
	// The other results are those of TrackItem and friends.

	// PLACE through TrackTarget is GOTO_UNKNOWN_TARGET: a place comes only from
	// TrackPlace.
	int32_t (*TrackTarget)(uint32_t kind, const char* key);
	int32_t (*UntrackTarget)(uint32_t kind, const char* key);
	// 1 tracked, 0 not, or a negative result (GOTO_OFF, GOTO_UNKNOWN_TARGET).
	// Test for == 1, not for non-zero.
	int32_t (*IsTrackedTarget)(uint32_t kind, const char* key);
	// Every tracked target of every kind, in tracking order. Like ListTracked,
	// `elementSize` is the caller's sizeof(GoToTarget): each element is written at
	// that stride and cut to it. At most `capacity` elements are written; *outCount
	// is how many exist in all (it may exceed capacity). out may be null with
	// capacity 0 to just count.
	int32_t (*ListTargets)(GoToTarget* out, uint32_t elementSize, uint32_t capacity, uint32_t* outCount);
	// The locations of one target, nearest first, at most maxSourcesPerItem. Same
	// convention (elementSize = the caller's sizeof(GoToSource)). Also for a target
	// that is not tracked, where GoToItem can know it.
	int32_t (*ListTargetSources)(uint32_t kind, const char* key, GoToSource* out, uint32_t elementSize, uint32_t capacity, uint32_t* outCount);
	// Like SelectSource, for any kind. sourceKey 0 = follow the nearest.
	int32_t (*SelectTargetSource)(uint32_t kind, const char* key, uint64_t sourceKey);
	// Mark one place. Reads `place` up to place->size (GOTO_BAD_ARGUMENT when that
	// is below the version-2 size). *outKey is the place's key: its decimal text is
	// the key of a GOTO_KIND_PLACE target, and it is never reused (see "Threads
	// and RELOAD"). It counts toward the tracked limit. The place closes by itself
	// on arrival (GOTO_PLACE_ARRIVE_METERS), when its actor is gone, on
	// UntrackPlace and when a save loads; it is never saved.
	int32_t (*TrackPlace)(const GoToPlace* place, uint64_t* outKey);
	int32_t (*UntrackPlace)(uint64_t placeKey);

	// Appended in later versions go below this line.
};

static_assert(offsetof(GoToItemApi, ResultToString) == 104, "GoToItemApi v1 layout");
static_assert(offsetof(GoToItemApi, TrackTarget) == 112, "GoToItemApi v2 layout");
static_assert(offsetof(GoToItemApi, UntrackPlace) == 168, "GoToItemApi v2 layout");

// Worked example: a Go button for one item. The tick does all the calling; the
// UI callback only reads and writes atomics.
//
//   static std::atomic<bool> s_showGo{false};
//   static std::atomic<bool> s_goClicked{false};
//
//   void OnTick(float)                           // engine tick: game thread
//   {
//       bool show = false;
//       if (const GoToItemApi* api = GoToItem::GetIfOn())
//       {
//           // your item's key; call this only while your panel is shown
//           const int32_t can = api->CanTrack(kItemKey);
//           show = can == GOTO_OK || can == GOTO_NO_LOCATION_YET;
//           if (s_goClicked.exchange(false) && show)
//               api->TrackItem(kItemKey);
//       }
//       else
//           s_goClicked = false;
//       s_showGo = show;
//   }
//
//   void DrawGo(IModLoaderImGui* ui)             // UI callback: no GoToItem calls
//   {
//       if (s_showGo && ui->Button("Go"))
//           s_goClicked = true;
//   }
//
// For a version-2 call, add GOTOITEM_HAS(api, TrackPlace) (or the member you
// use) to the test. To keep one marked place at a time, call UntrackPlace with
// the key you kept before the next TrackPlace, and set place.size =
// sizeof(GoToPlace).

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
	// the lookup, or cannot serve API version 1. Asks for version 1, the
	// oldest this wrapper can use, so a caller built against a newer header
	// still gets the table from an older GoToItem.dll. Check members added
	// after version 1 with GOTOITEM_HAS before calling them. Looks the module
	// up on every call: do not cache the result. Game thread only (see "Threads
	// and RELOAD" above). Null means GoToItem is not there. To decide whether to
	// show Go, use GetIfOn() below.
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
			const GoToItemApi* api = fn(1);
			if (api && api->size >= offsetof(GoToItemApi, ResultToString) + sizeof(void*))
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

	// Get(), or null while GoToItem is turned off: the one test for "show Go?".
	// Game thread only, like Get(). A version-1 table is always returned.
	inline const GoToItemApi* GetIfOn()
	{
		const GoToItemApi* api = Get();
		if (Has(api, offsetof(GoToItemApi, IsTrackedTarget), sizeof(api->IsTrackedTarget)) &&
			api->IsTrackedTarget(GOTO_KIND_ITEM, "-") == GOTO_OFF)
			return nullptr;
		return api;
	}
}

#define GOTOITEM_HAS(api, member) \
	GoToItem::Has((api), offsetof(GoToItemApi, member), sizeof(((GoToItemApi*)nullptr)->member))
#endif // _WIN32
