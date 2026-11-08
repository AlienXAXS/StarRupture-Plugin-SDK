# StarRupture Plugin SDK

Everything you need to build plugins for the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) — without forking or building the modloader itself.

## What's in this repo

| Path | Description |
|---|---|
| `include/plugin_interface.h` | The full plugin API — interfaces, callbacks, enums |
| `include/plugin_network_helpers.h` | Typed helpers for sending and receiving network packets |
| `include/plugin_map.h` | Helpers for game functions, members and globals from the loader's version map (v72) |
| `include/plugin_version_map.h` | `plugin_map.h` plus the plugin's own rows from a generated `version_map.generated.h` |
| `include/object_ref.h` | `ObjectRef<T>`: a UObject reference that is safe to keep past the tick it was read on |
| `PluginGameBuild.props` | Defines `PLUGIN_GAME_BUILT_CL`, the game build a plugin was built for; import it from a plugin's `Shared.props` |
| `ExamplePlugin/` | Minimal starter plugin — copy, rename, and go |
| `StarRupture SDK/` | Dumper-7 generated UE5 SDK headers (Client + Server + Generic) |
| `Shared.props` | MSBuild properties for SDK paths and build configs |
| `PluginDevelopment.md` | Full API reference and developer guide |

## Quick start

1. **Download `dwmapi.dll`** from the [latest release](../../releases/latest) — pick Client or Server depending on your target
2. **Clone this repo**
3. **Copy `ExamplePlugin/` to a new folder**, rename it, and update the project name/GUID in the `.vcxproj`
4. **Add your project to `StarRupture-Plugin-SDK.sln`** (or create your own `.sln`)
5. **Build** using Visual Studio 2022 with the `Client Release` or `Server Release` configuration
6. Drop your `.dll` into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

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

Save over a selected saved preset asks "You're saving over ... Continue?" first. Save with nothing selected adds a new entry ("Custom", "Custom 2", ...). Built-in presets are never written to the file. The store is thread-safe, so a hotkey can read a preset with `Store::Get` while the panel is open.

Players' files are protected: loading never writes, the first rewrite keeps a `.bak` copy beside the file, and a file with a line the store cannot read is left alone (the row says so instead of saving). The tests are in `Tests/PluginPresetsTests/` and build without the game. To also check a real presets file, pass its path as the first argument; the file is only read.

## Interface versioning

This SDK tracks the current `PLUGIN_INTERFACE_VERSION_MAX` from the modloader. The `PluginInfo` struct your plugin returns must set `interfaceVersion` to `PLUGIN_INTERFACE_VERSION` (defined in `plugin_interface.h`) or the modloader will refuse to load it.

The SDK repo is updated alongside each modloader release.
