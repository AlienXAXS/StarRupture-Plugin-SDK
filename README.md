# StarRupture Plugin SDK

Everything you need to build plugins for the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) — without forking or building the modloader itself.

## What's in this repo

| Path | Description |
|---|---|
| `include/plugin_interface.h` | The full plugin API — interfaces, callbacks, enums |
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

## Runtime DLL

`dwmapi.dll` is the modloader itself. It is **not** built from this repo. Pre-built binaries are attached to each [release](../../releases/latest).

Do **not** fork or build the main modloader repo just to develop a plugin — this SDK repo is the intended starting point.

## Interface versioning

This SDK tracks the current `PLUGIN_INTERFACE_VERSION_MAX` from the modloader. The `PluginInfo` struct your plugin returns must set `interfaceVersion` to `PLUGIN_INTERFACE_VERSION` (defined in `plugin_interface.h`) or the modloader will refuse to load it.

The SDK repo is updated automatically each time a new modloader release is published.
