# StarRupture Plugin SDK

Everything you need to build plugins for the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) — without forking or building the modloader itself.

Making one DLL run on every game build? Start with [MultiVersionGuide.md](MultiVersionGuide.md).

## What's in this repo

| Path | Description |
|---|---|
| `include/plugin_interface.h` | The full plugin API — interfaces, callbacks, enums |
| `include/plugin_network_helpers.h` | Typed helpers for sending and receiving network packets |
| `include/plugin_map.h` | Helpers for game functions, members and globals from the loader's version map (v72) |
| `include/plugin_version_map.h` | `plugin_map.h` plus the plugin's own rows from a generated `version_map.generated.h` |
| `include/object_ref.h` | `ObjectRef<T>`: a UObject reference that is safe to keep past the tick it was read on |
| `include/item_icons.h` | `ItemIcons::Loader`: loads item icons the first time they are drawn, a few per tick (`icon_requests.h` is its request queue) |
| `include/plugin_images.h` | `PluginImages::Image`: draws an image from the loader's image library, or reserves its space while it loads (interface 77, loader 1.24.0 or newer) |
| `include/plugin_waypoint.h` | `PluginWaypoint::Waypoint`: a small window with an arrow and a distance that points the player at a target; `Math::` has the bearing and distance arithmetic alone |
| `PluginGameBuild.props` | Defines `PLUGIN_GAME_BUILT_CL` from the build_cl.txt of the Game-SDK headers a plugin compiles against (a stale pin only warns); import it from a plugin's `Shared.props` |
| `ExamplePlugin/` | Minimal starter plugin — copy, rename, and go |
| `StarRupture SDK/` | Dumper-7 generated UE5 SDK headers (Client + Server + Generic) |
| `Shared.props` | MSBuild properties for SDK paths and build configs |
| `MultiVersionGuide.md` | One page: make a plugin run on every game build, and get rows added to the version map |
| `PluginDevelopment.md` | Full API reference and developer guide |

## Quick start

1. **Download `dwmapi.dll`** from the [latest release](../../releases/latest) — pick Client or Server depending on your target
2. **Clone this repo** with `git clone --recursive`, so the `StarRupture SDK` submodule comes with it
3. **Copy `ExamplePlugin/` to a new folder**, rename it, and update the project name/GUID in the `.vcxproj`
4. **Add your project to `StarRupture-Plugin-SDK.sln`** (or create your own `.sln`)
5. **Build** using Visual Studio 2022 with the `Client Release` or `Server Release` configuration
6. Drop your `.dll` into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

Plugins in their own repos find this SDK as a sibling folder, so no paths need setting:

```
AnyFolder\
  StarRupture-Plugin-SDK\      (git clone --recursive)
  StarRupture-Plugin-<Name>\
```

To use another location, pass `/p:PluginSDKInclude=<path to include>` and `/p:GameSDKRoot=<path to StarRupture SDK>` to MSBuild; they override the defaults. Details are in [PluginDevelopment.md](PluginDevelopment.md#where-the-sdks-live).

See [PluginDevelopment.md](PluginDevelopment.md) for the complete API reference, hook list, config system, pattern scanner, and ImGui integration.

## Build configurations

| Configuration | Target | Defines |
|---|---|---|
| `Client Debug` / `Client Release` | Game client | `MODLOADER_CLIENT_BUILD` |
| `Server Debug` / `Server Release` | Dedicated server | `MODLOADER_SERVER_BUILD` |
| `Debug` / `Release` | Generic (no SDK-specific APIs) | — |

Client builds have access to Input, UI panel, and HUD hooks. These interfaces are `nullptr` on server builds, so guard any client-only code with `#if defined(MODLOADER_CLIENT_BUILD)`.

Network channel (`hooks->Network`) is available on both client and server builds; it is `nullptr` on generic builds. Use `plugin_network_helpers.h` for typed packet send/receive rather than calling `IPluginNetworkChannel` directly.

## Runtime DLL

`dwmapi.dll` is the modloader itself. It is **not** built from this repo. Pre-built binaries are attached to each [release](../../releases/latest).

Do **not** fork or build the main modloader repo just to develop a plugin — this SDK repo is the intended starting point.

## Plugin structure (v19+)

`PluginInit` now receives a single `IPluginSelf*` instead of four separate pointers:

```cpp
extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self; // store — pointer is stable for the plugin's lifetime

    self->logger->Info(self, "Hello from %s", self->name);
    self->config->InitializeFromSchema(self, &MY_SCHEMA);
    self->hooks->Engine->RegisterOnInit(OnEngineInit);
    return true;
}
```

`IPluginSelf` bundles everything your plugin needs:

| Field | Type | Description |
|---|---|---|
| `name` | `const char*` | Plugin name from `PluginInfo` |
| `version` | `const char*` | Plugin version from `PluginInfo` |
| `logger` | `IPluginLogger*` | Logging interface |
| `config` | `IPluginConfig*` | Config read/write interface |
| `hooks` | `IPluginHooks*` | All hook and event sub-interfaces. `nullptr` during `OnPluginLoadHooks` |

There is no `scanner` member. Pattern scanning lives in the optional `OnPluginLoadHooks` export,
which receives its own `IPluginHookScanner` and is the only place a plugin can scan -- see
[PluginDevelopment.md](PluginDevelopment.md#pattern-scanning----onpluginloadhooks--ipluginhookscanner).

The `ExamplePlugin/` files demonstrate the full pattern.

## Keeping a UObject across ticks

A raw `UObject*` held past the tick that fetched it can dangle: the object may be garbage collected and its memory handed to a new object, and a write through the old pointer corrupts whatever lives there now. `include/object_ref.h` is a header-only `ObjectRef<T>` for this. It records the object's `GObjects` slot and class next to the pointer, and `Get()` returns the pointer only while the slot still holds that object, of that class, not marked destroyed or garbage. (If a new object of the same class later takes the same address and slot, `Get()` returns it: it is live and safe to use, but it is not the one you recorded.) It needs no loader support, so it works with any loader version.

```cpp
#include <object_ref.h>

ObjectRef<SDK::UAudioComponent> m_audio;   // member, kept across ticks

m_audio.Set(component);                    // component must be live right now
if (SDK::UAudioComponent* c = m_audio.Get())   // nullptr once it is gone or its slot reused
    c->Stop();
```

`Reset()` clears it, and references are small, trivially copyable, and compare with `==`. Call `Get()` on the game thread (the `Tick` callback, not a render callback), since that is the thread that frees objects. It includes `Basic.hpp` and `CoreUObject_classes.hpp` from the game SDK. The tests in `Tests/ObjectRefTests/` build without the game, against stand-ins for those headers.

## Saved presets

`include/plugin_presets.h` gives a plugin a "Saved Presets" row: a dropdown of the player's saved presets with Save, Rename and Delete. It is header-only and uses only `IModLoaderImGui`. Presets are kept in `<plugin dir>\config\<Plugin>-Presets.ini`, in groups, so one file can serve several panels.

1. Keep one `PluginPresets::Store` for the plugin and load it once, with the path of the file. Pass a logger if you want the store's messages in your log.
2. For each group, keep a `PluginPresets::RowState` and call `RenderSavedRow` from your panel.
3. Give it four small callables: read the live values, apply a loaded preset, say whether a name is one of your own built-in presets, and suggest a name for a new copy.

```cpp
#include <plugin_presets.h>

static PluginPresets::Store s_presets([](const char* line) { MyLog(line); });
static PluginPresets::RowState s_speedRow;

// once, at startup
s_presets.LoadFile(presetsPath);

// in the panel
PluginPresets::Field fields[2];
PluginPresets::RenderSavedRow(ui, s_presets, "speed", "Speed", fields, 2,
    [](PluginPresets::Field* out) { out[0] = { "speed", ReadSpeed() }; out[1] = { "boost", ReadBoost() }; },
    [](const PluginPresets::Field* f, int n) { ApplySpeed(f[0].value, f[1].value); },
    [](const char* name) { return IsBuiltInSpeedName(name); },
    [](char* out, int cap) { snprintf(out, cap, "Custom"); },
    s_speedRow);
```

Save over a selected saved preset asks "You're saving over ... Continue?" first; it replaces the values the live settings supply and keeps any stored key they do not. Save with nothing selected adds a new entry ("Custom", "Custom 2", ...). The dropdown's "(none)" item clears the selection, and `RowState::ClearSelection()` does the same from code: call it when your own built-in preset buttons or hotkeys apply a built-in, so the row stops showing the old selection (and the next Save adds a new entry unless the values now equal a saved preset). When nothing is selected and the live values equal a saved preset (after a restart, say), the row selects the first such preset, so Save over it works at once; picking "(none)" turns this off until a preset is applied. Built-in presets are never written to the file. The store is thread-safe, so a hotkey can read a preset with `Store::Get` while the panel is open.

Players' files are protected: loading never writes, the first rewrite keeps a `.bak` copy beside the file, and a file with a line the store cannot read, or one that exists but could not be read at load (locked, for example), is left alone for the session (the row says so instead of saving). The tests are in `Tests/PluginPresetsTests/` and build without the game. To also check a real presets file, pass its path as the first argument; the file is only read.

## Waypoint: point the player at something

`include/plugin_waypoint.h` shows a small window with a label, an X button, an arrow that turns with the camera and the distance to a target (`250m`, or `1.2km` past a kilometre). It closes itself when the player is within 10 m of the target, or when the target has been gone for 120 ticks (about 2 seconds at 60 fps). It is header-only and needs no loader support and no version-map rows of its own. It draws with `IModLoaderImGui` and reads the player and camera through Game-SDK calls, so set up the SDK globals first, as `MultiVersionGuide.md` says.

1. Keep one `PluginWaypoint::Waypoint` for the plugin (a static or a member).
2. Call `Tick(world)` from your engine tick. This is the only call that reads the game, so it must run on the game thread.
3. Register a widget whose callback calls `Render(ui)`. When `Render` returns false there is nothing to show, so hide the widget. Do the same when `Active()` turns false in your tick.
4. To start, show the widget and call `Set` with a function that finds the target (below), or `SetActor` with an `ObjectRef<SDK::AActor>` built on the game thread. `Set` and `SetActor` are safe from a button click.
5. Call `Clear()` to stop.

```cpp
#include <plugin_waypoint.h>

static PluginWaypoint::Waypoint s_waypoint;

// Called on the game thread every tick while the waypoint is active. Look the
// target up again each time; do not keep a UObject* between calls.
static bool FindHome(void* /*user*/, SDK::UWorld* world, PluginWaypoint::Resolved& out)
{
    SDK::FVector pos;
    if (!MyLookUpHome(world, &pos))     // your own lookup
        return false;                   // gone this tick
    out.x = pos.X; out.y = pos.Y; out.z = pos.Z;
    out.label = MyHomeName();           // optional; leave empty to keep the label given to Set
    return true;
}

// engine tick:      s_waypoint.Tick(world);
// widget callback:  if (!s_waypoint.Render(ui)) { /* hide the widget */ }
// button click:     s_waypoint.Set(&FindHome, nullptr, "Home"); /* show the widget */
```

The arrow follows the camera's yaw, and the distance and the close check are measured from the camera's view target (the character on foot; the drone or vehicle while you are in it). The arrow size and colour, the 10 m close distance and the grace period before a lost target closes the window are the fields of `PluginWaypoint::Options`, passed to the `Waypoint` constructor. Two waypoints in one window need an `imgui->PushIDInt` around each `Render`, because the X button's id is the same.

A plugin that draws its own marker can use `PluginWaypoint::Math` alone. It takes plain doubles and needs no game types. `RelativeBearingDeg(dx, dy, yawDeg)` gives the angle from where the camera faces to the target: 0 ahead, +90 right, -90 left, +-180 behind. `Distance3dMeters(dx, dy, dz)` turns a world-unit offset (centimetres) into metres. For example, a map that has a world-to-map transform and the target's world x and y:

```cpp
const double dx = targetX - playerX;
const double dy = targetY - playerY;
const double arrow = PluginWaypoint::Math::RelativeBearingDeg(dx, dy, cameraYawDegrees);
// For a map that turns with the player, draw the arrow `arrow` degrees
// clockwise from "up". A map that keeps north at the top needs the target's
// bearing from north instead, not this. If the
// transform's WorldToScreen puts the target outside the map rectangle, draw
// the arrow at the rectangle's edge instead of on the target.
```

The tests for `Math::` are in `Tests/WaypointTests/` and build without the game.

## Item stack sizes

`include/item_stack_limits.h` is a header-only helper to apply and restore item stack sizes. It raises `MaxStack` on the items you give it (never below an item's own vanilla stack, never on items that do not stack, capped at 9999) and puts every value back to vanilla on request. Both members are reflected, so the loader reads their offsets from the running game's own reflection data and a build with no version map row still works; `Resolve` is false when the loader cannot give either member at the compiled size. The "does not stack" value is read by name from the game's enum, and if that lookup misses nothing is raised. It needs plugin interface 73 (`plugin_map.h`, which it includes, calls the v73 `DeclareFeature`) and the Game-SDK's `AuItems_classes.hpp`. Call it from `Tick`: it holds the items as `ObjectRef` and checks each before a write. Usage is in the header's comment; the tests in `Tests/ItemStackLimitsTests/` build without the game.

## Interface versioning

Your plugin declares the interface level it needs. A ModLoader refuses a plugin for its interface level only when that level is higher than the newest one the ModLoader knows, so the lower you can honestly declare, the more ModLoaders load your plugin, even when you build against the newest SDK.

1. Set `PLUGIN_NEEDS_INTERFACE` in your project's preprocessor definitions (every configuration) to the lowest level your plugin uses. Unset, it is 75.
2. Fill `PluginInfo` with `PLUGIN_DECLARED_INTERFACE`, not a number.
3. Raise the level only when the compiler tells you to. An error such as `'Images': is not a member of 'IPluginHooks'` means that member needs a newer ModLoader; the comment on it in `plugin_interface.h` names the level. Members are hidden, and so are `PluginReload`, `PLUGIN_RELOAD_*`, `PluginLoadSetting` and `PluginUnloadSetting` below 73. Other flags and constants are not (`PLUGIN_FIELD_ANY_SIZE` and `PluginCond_*` came in at 74, `PLUGIN_FIELD_LATE` and `PluginWindowFlags_MenuMouseWhereUncovered` at 75), so use one only when your level covers it. An older loader refuses the request or ignores the flag, and that feature stays off.

Use the same value in every file you build into the plugin. Two values in one DLL are a link error (`LNK2038`, `PluginNeedsInterface`).

Example: a plugin that draws library images (`IPluginHooks::Images`, since level 77).

```xml
<PreprocessorDefinitions>PLUGIN_NEEDS_INTERFACE=77;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

```cpp
static PluginInfo s_pluginInfo = {
    "MyPlugin", "1.0.0", "Your Name", "What this plugin does",
    PLUGIN_DECLARED_INTERFACE, PLUGIN_TARGET_CLIENT,
    PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED
};
```

The plugin is built on the newest SDK but declares 77, so it loads on ModLoader v1.24.0 and later. Without the define the compiler stops at `hooks->Images`. `include/plugin_images.h` does not stop you: a plugin that includes it needs `PLUGIN_NEEDS_INTERFACE=77`, or `PLUGIN_OPTIONAL_INTERFACE=77` as in the next section (the images then draw only on ModLoader v1.28.0 and later), or the images draw nothing. A plugin that touches nothing above 75 needs no define at all and loads on v1.23.0 and later.

| Interface level | First ModLoader that loads it |
|---|---|
| 72 to 75 (75 is the default) | v1.23.0 |
| 76 to 77 (77 is the image library) | v1.24.0 |
| 78 | v1.25.0 |
| 79 | v1.27.0 |
| 80 | v1.28.0 |

### A newer member only when the loader has it

A plugin that needs 73 can still use a member added later, such as `IPluginHooks::Registry` (80), on a loader that has it. Say so with a second define, and the loader says which level it has.

1. Keep `PLUGIN_NEEDS_INTERFACE` at the oldest loader the plugin runs on, and set `PLUGIN_OPTIONAL_INTERFACE` (every configuration, same value in every file) to the newest level it may use. Unset, it is the need.
2. Put `PLUGIN_EXPORT_HOST_INFO()` once in one `.cpp` file of the plugin. It exports `PluginSetHostInfo`, which a loader with interface 80 calls once, after `GetPluginInfo` and before `OnPluginLoadHooks` and `PluginInit`, with its own level. Leave it out and the plugin does not link (`LNK2001`, `PluginSetHostInfo`), so the accessors can never silently stay off.
3. Reach the member through its accessor, not through the raw table, and handle `nullptr`:

```xml
<PreprocessorDefinitions>PLUGIN_NEEDS_INTERFACE=73;PLUGIN_OPTIONAL_INTERFACE=80;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

```cpp
PLUGIN_EXPORT_HOST_INFO()

void MyTick(const IPluginSelf* self)
{
    if (IPluginRegistry* reg = PluginOptRegistry(self->hooks))
    {
        // the loader is 80 or newer: use *reg
    }
    // otherwise the loader is older: this feature is off, the rest of the plugin works
}
```

The plugin declares 73, so it loads on every loader from 73 up. A loader below 80 never calls the export, so `PluginHostLevel()` stays at 73, every accessor returns `nullptr` and the feature is simply off, even for a member that older loader has (an accessor for level 77 returns `nullptr` on v1.24.0 to v1.27.0). `PluginHostHas(n)` asks whether the loader is known to have level `n`; the level you declared always counts.

| Accessor | Member | Level | Works from |
|---|---|---|---|
| `PluginOptImages(hooks)` | `IPluginHooks::Images` | 77 | ModLoader 1.28.0 |
| `PluginOptDL_AddTexturedTriangles(imgui)` | `IModLoaderImGui::DL_AddTexturedTriangles` | 78 | ModLoader 1.28.0 |
| `PluginOptHostRules(hooks)` | `IPluginHooks::HostRules` | 79 | ModLoader 1.28.0 |
| `PluginOptRegistry(hooks)` | `IPluginHooks::Registry` | 80 | ModLoader 1.28.0 |
| `PluginOptServerAddress(network)` | `IPluginNetworkChannel::GetServerAddress` | 80 | ModLoader 1.28.0 |

`PluginImages::Image` (`plugin_images.h`) uses `PluginOptImages` by itself when `PLUGIN_OPTIONAL_INTERFACE` is 77 or higher and the need is below 77.

An accessor exists only when `PLUGIN_OPTIONAL_INTERFACE` is at least its level, so naming one you did not ask for is a compile error. The raw member stays hidden below the need, as in the section above.

The SDK repo is updated alongside each modloader release.
