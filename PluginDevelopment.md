# Plugin Development Guide -- StarRupture ModLoader

> End-user documentation is in [README.md](README.md) and [How To Use](How%20To%20Use.md).

> **Building a plugin?** Use the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) repo — it contains the headers, UE5 SDK, ExamplePlugin template, and pre-built `dwmapi.dll` releases. You do **not** need to fork or build the modloader itself.

This guide covers building the mod loader from source, creating new plugins, the full plugin API, and the interface version changelog.

---

## Table of Contents

1. [Building from Source](#building-from-source)
2. [Project Structure](#project-structure)
3. [Creating a Plugin](#creating-a-plugin)
4. [Plugin Lifecycle](#plugin-lifecycle)
5. [Plugin Manifest & Auto-Update](#plugin-manifest--auto-update)
6. [API Reference](#api-reference)
   - [IPluginSelf](#ipluginself)
   - [IPluginLogger](#ipluginlogger)
   - [IPluginConfig](#ipluginconfig)
   - [IPluginScanner](#ipluginscanner)
   - [IPluginHooks -- Sub-Interfaces](#ipluginhooks----sub-interfaces)
     - [hooks->Engine (IPluginEngineEvents)](#hooksengine)
     - [hooks->World (IPluginWorldEvents)](#hooksworld)
     - [hooks->Players (IPluginPlayerEvents)](#hookplayers)
     - [hooks->Actors (IPluginActorEvents)](#hooksactors)
     - [hooks->Spawner (IPluginSpawnerHooks)](#hooksspawner)
     - [hooks->Hooks (IPluginHookUtils)](#hookshooks)
     - [hooks->Memory (IPluginMemoryUtils)](#hooksmemory)
     - [hooks->Input (IPluginInputEvents) -- Client only](#hooksinput)
     - [hooks->UI (IPluginUIEvents) -- Client only](#hooksui)
     - [hooks->HUD (IPluginHUDEvents) -- Client only](#hookshud)
     - [hooks->Network (IPluginNetworkChannel) -- Server + Client](#hooksnetwork)
     - [hooks->NativePointers (IPluginNativePointers)](#hooksnativepointers)
     - [hooks->HttpServer (IPluginHttpServer) -- Server only](#hookshttpserver)
7. [Interface Version Changelog](#interface-version-changelog)
8. [Troubleshooting](#troubleshooting)

---

## Building from Source

### Prerequisites

- Visual Studio 2022 with the C++ desktop workload (C++20 required)
- Windows SDK 10.0+
- A copy of the StarRupture Unreal Engine SDK (Dumper-7 generated) placed at `StarRupture SDK/`

### Build Configurations

The solution provides six configurations (all x64):

| Configuration    | Output DLL       | SDK path                         | Defines                      |
|------------------|------------------|----------------------------------|------------------------------|
| Debug            | dwmapi.dll       | StarRupture SDK/SDK/             | (none)                       |
| Release          | dwmapi.dll       | StarRupture SDK/SDK/             | (none)                       |
| Client Debug     | dwmapi.dll       | StarRupture SDK/SDK/             | MODLOADER_CLIENT_BUILD       |
| Client Release   | dwmapi.dll       | StarRupture SDK/SDK/             | MODLOADER_CLIENT_BUILD       |
| Server Debug     | dwmapi.dll       | StarRupture SDK/Server/SDK/      | MODLOADER_SERVER_BUILD       |
| Server Release   | dwmapi.dll       | StarRupture SDK/Server/SDK/      | MODLOADER_SERVER_BUILD       |

- **Client builds** include the full client SDK and enable client-only hooks (Input, UI, HUD).
- **Server builds** use the server-only SDK and disable all client-only hooks (Input, UI, HUD are nullptr).
- **Generic builds** (Debug/Release) use the common SDK subset and also disable client-only hooks.

### Steps

1. Clone the repository.
2. Place the Dumper-7 generated SDK under `StarRupture SDK/`.
3. Open `StarRupture-ModLoader.sln`.
4. Select the desired configuration (Client/Server/Generic x Debug/Release).
5. Build the solution. Output DLLs are placed in `build/[Configuration]/`.

---

## Project Structure

```
StarRupture-ModLoader/
+-- Version_Mod_Loader/          # Core mod loader (ships as dwmapi.dll)
|   +-- logging/logger.cpp       # Multi-level logging system
|   +-- memory_scanner/scanner.cpp  # IDA-style pattern scanning engine
|   +-- hooks/hooks_interface.cpp   # Sub-interface wiring (g_pluginHooks)
|   +-- config/config_manager.cpp   # INI config with schema validation
|   +-- plugins/plugin_manager.cpp  # Plugin discovery and lifecycle
|   +-- plugins/plugin_interface.h  # Plugin API header (copy this into your plugin)
|   +-- hooks/game/               # Game-specific hook groups
|       +-- scan_patterns.h       # Central IDA-pattern registry (all builds)
|       +-- engine_init/          # UGameEngine::Init callback
|       +-- engine_shutdown/      # Engine shutdown callback
|       +-- engine_tick/          # UGameEngine::Tick per-frame callback
|       +-- world_begin_play/     # UWorld::BeginPlay callback
|       +-- any_world_begin_play/ # Any world BeginPlay (with name) callback
|       +-- experience_load/      # UCrExperienceManagerComponent::OnExperienceLoadComplete
|       +-- save_loaded/          # UCrMassSaveSubsystem::OnSaveLoaded
|       +-- actor_begin_play/     # AActor::BeginPlay callback
|       +-- player_joined/        # ACrGameModeBase::PostLogin callback
|       +-- player_left/          # ACrGameModeBase::Logout callback
|       +-- spawner_hooks/        # ActivateSpawner/DeactivateSpawner/DoSpawning Before+After
|       +-- input_hooks/          # UE key input (client only)
|       +-- hud_post_render/      # AHUD::PostRender (client only)
|   +-- UI/imgui_backend.cpp      # DX12 ImGui overlay (client only)
|
+-- StarRupture SDK/              # Dumper-7 generated Unreal Engine SDK
|
+-- ExamplePlugin/                # Starter template for new plugins
+-- KeepTicking_Plugin/           # Anti-AFK keep-alive for servers
+-- RailJunctionFixer/            # Runtime UStruct inheritance patcher
+-- ServerUtility/                # RCON, Steam Query, and CLI settings
+-- Compass_Plugin/               # Minimap compass overlay (client)
```

---

## Creating a Plugin

Use `ExamplePlugin` as your starting point. It is a minimal, working plugin that demonstrates the required structure without modifying gameplay.

### Quick Start

1. **Copy** the `ExamplePlugin` folder and rename it (e.g. `MyPlugin`).
2. **Rename** `ExamplePlugin.vcxproj` to `MyPlugin.vcxproj`.
3. **Edit** the `.vcxproj` and replace the `<ProjectGuid>` value with a new GUID.
4. **Update** `s_pluginInfo` in `plugin.cpp` with your plugin's name, version, author, and description.
5. **Add** the project to the solution: right-click solution > Add > Existing Project.
6. **Build** -- the DLL is placed in `bin\x64\[Configuration]\plugins\`.
7. **Install** -- copy the DLL to `<game_dir>\Plugins\`.

### Minimal Plugin (plugin.cpp)

```cpp
#include "plugin.h"
#include "plugin_helpers.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

static PluginInfo s_pluginInfo = {
    "MyPlugin",
    "1.0.0",
    "Your Name",
    "What this plugin does",
    PLUGIN_INTERFACE_VERSION
};

extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo()
{
    return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self; // pointer is stable for the plugin's lifetime

    LOG_INFO("Plugin initializing...");
    // Register callbacks, install hooks, etc.
    return true;
}

__declspec(dllexport) void PluginShutdown()
{
    LOG_INFO("Plugin shutting down...");
    // Unregister callbacks, remove hooks, free resources.
    g_self = nullptr;
}

} // extern "C"
```

### Config System (plugin_config.h pattern)

The config schema system auto-generates an INI file with defaults on first load:

```cpp
// plugin_config.h
static const ConfigEntry CONFIG_ENTRIES[] = {
    { "General", "Enabled",      ConfigValueType::Boolean, "true",  "Enable the plugin" },
    { "General", "PlayerName",   ConfigValueType::String,  "Alice", "Display name"      },
    { "Settings","RefreshRate",  ConfigValueType::Integer, "60",    "Ticks per second"  },
    { "Settings","ScaleFactor",  ConfigValueType::Float,   "1.0",   "UI scale"          },
};
static const ConfigSchema SCHEMA = { CONFIG_ENTRIES, 4 };
```

In `PluginInit`, call `self->config->InitializeFromSchema(self, &SCHEMA)`. The loader creates `Plugins/config/MyPlugin.ini` with all defaults if it does not yet exist.

Read values with type-safe helpers:

```cpp
bool    enabled = self->config->ReadBool  (self, "General",  "Enabled",     true);
int     rate    = self->config->ReadInt   (self, "Settings", "RefreshRate",  60);
float   scale   = self->config->ReadFloat (self, "Settings", "ScaleFactor",  1.0f);
char    name[64];
self->config->ReadString(self, "General", "PlayerName", name, sizeof(name), "Alice");
```

---

## Plugin Lifecycle

```
Mod loader scans Plugins/ directory
    --> loads MyPlugin.dll
    --> calls GetPluginInfo()    -- reads name, version, interfaceVersion
    --> version check: must be in [PLUGIN_INTERFACE_VERSION_MIN, MAX]
    --> calls PluginInit(self)   -- IPluginSelf* bundles name, version, logger, config, scanner, hooks
        -- store self pointer (stable for the plugin's lifetime)
        -- register callbacks
        -- install hooks
        -- return true (success) or false (failure, plugin is unloaded)
    --> plugin is active
    --> per-frame: engine tick callbacks fire
    --> on game events: world/player/actor callbacks fire
Game closes (or plugin hot-unloaded):
    --> calls PluginShutdown()
        -- unregister all callbacks
        -- remove all hooks
        -- free resources
        -- clear self pointer
```

---

## Plugin Manifest & Auto-Update

The modloader ships a two-pass auto-update system that runs at startup, before any plugins are loaded. Understanding it lets you ship self-updating plugins that never need users to manually replace DLL files.

### Why a manifest sidecar?

When the modloader discovers plugins it loads each DLL into the process. To check whether a newer version is available it would have to load the DLL, call `GetPluginInfo`, then potentially unload and replace it — which is impossible while the process holds a lock on the file.

A **sidecar** is a small `.json` file placed next to the plugin DLL with the same base name (e.g. `Plugins\MyPlugin.json`). The modloader reads the sidecar before loading any DLLs. Because it is plain JSON, the updater can fetch a remote manifest, compare versions, and atomically replace the DLL on disk — all without ever loading the old binary.

### Sidecar format

```json
{
  "manifest_url": "https://example.com/MyPlugin/releases/latest/download/MyPlugin-client-manifest.json"
}
```

The sidecar contains a single field: the URL where the plugin's **remote update manifest** is hosted. This URL is typically a GitHub Releases `latest/download` permalink so it always points at the newest release.

### Remote update manifest format

Hosted at the URL stored in the sidecar, fetched fresh on every boot:

```json
{
  "plugin_name":           "MyPlugin",
  "version":               "1.2.0",
  "interface_version_min": 19,
  "interface_version_max": 22,
  "download_url":          "https://example.com/releases/download/v1.2.0/MyPlugin-Client.dll"
}
```

| Field | Description |
|-------|-------------|
| `plugin_name` | Display name used in log messages |
| `version` | Version string compared against `update_state.ini` to decide whether to download |
| `interface_version_min` | Lowest interface version this build supports |
| `interface_version_max` | Highest interface version this build supports |
| `download_url` | Direct URL to the DLL to download if an update is needed |

If `interface_version_min`/`max` are present and do not overlap `[PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX]`, the update is skipped entirely — the loader will not install a plugin it cannot run.

### How assets are generated by CI

The reusable workflow [.github/workflows/plugin-release.yml](.github/workflows/plugin-release.yml) handles everything automatically when a plugin uses it:

1. **Build** — MSBuild compiles the plugin DLL with `/p:ModLoaderBuildTag=<semver>` so the version tag is baked into `PluginInfo.version`.
2. **Generate manifest** — a `MyPlugin-client-manifest.json` file is written from the plugin name, computed semver tag, interface version range (read directly from `plugin_interface.h`), and the GitHub release download URL.
3. **Generate ZIP** — a `Plugins\` folder is staged containing:
   - `MyPlugin.dll` — the compiled plugin
   - `MyPlugin.json` — the sidecar pointing at the `latest/download` manifest URL
   - `MyPlugin.pdb` — PDB if present
4. **Create GitHub Release** — three assets are uploaded: the bare DLL, the manifest JSON, and the ZIP. The ZIP is the intended install package for users.

A user who installs via the ZIP gets both the DLL **and** its sidecar. From that point on the modloader handles all future updates silently.

### How the auto-update system works at runtime

`RunAutoUpdate()` runs two independent passes before `LoadAllPlugins()`:

**Pass 1 — Central manifest (modloader self-update check)**

Reads `modloader.ini` for the central manifest URL (or uses the compiled-in `AUTOUPDATE_MANIFEST_URL` baked in by CI). Fetches the JSON, reads `build_tag`, and compares it against the value stored in `Plugins\update_state.ini`. If a newer tag is found the user is notified (UI overlay on client builds). The modloader does **not** self-update — it only tells the user.

**Pass 2 — Per-plugin sidecar pass**

Scans `Plugins\*.json`. For each sidecar that has a matching `.dll`:
1. Fetches the `manifest_url`.
2. Compares the remote `version` against `[PluginVersions]` in `update_state.ini`.
3. If the versions differ and the interface version range is compatible, downloads the DLL to a `.tmp` file then atomically renames it over the old DLL.
4. Writes the new version to `update_state.ini` so the next boot is a no-op.

```
Plugins/
  MyPlugin.dll        ← binary (replaced atomically on update)
  MyPlugin.json       ← sidecar: { "manifest_url": "https://..." }
  update_state.ini    ← auto-generated state file (not user-editable)
    [AutoUpdate]
    BuildTag=v1.0.0
    [PluginVersions]
    MyPlugin.dll=1.2.0
```

Both passes respect `[AutoUpdate] Enabled=0` in `modloader.ini`. Network and parse errors are always non-fatal — the existing DLL is never removed unless the replacement download is fully written and successfully renamed.

### Configuration

`modloader.ini` (next to `dwmapi.dll`):

```ini
[AutoUpdate]
Enabled=1          ; set to 0 to disable all update checks
ManifestUrl=       ; override the compiled-in central manifest URL (optional)
```

### Shipping a self-updating plugin

To opt into auto-updates for your plugin:

1. Use the reusable workflow: add `.github/workflows/release.yml` to your plugin repo that calls `AlienXAXS/StarRupture-Plugin-SDK/.github/workflows/plugin-release.yml@main`.
2. The workflow generates and publishes the manifest and sidecar automatically — no manual steps.
3. Tell users to install from the ZIP. The sidecar inside it wires up future updates automatically.

If you self-host outside of GitHub, generate the manifest and sidecar yourself following the formats above and host the manifest at a stable URL (use a `latest` permalink, not a versioned one, so users never need to update the sidecar).

---

## API Reference

The `IPluginSelf*` pointer passed to `PluginInit` is stable for the plugin's lifetime (from `PluginInit` return until `PluginShutdown` returns). Store it in a static and use it for all API calls.

---

### IPluginSelf

`IPluginSelf` is the single entry point your plugin receives. It bundles identity and all sub-interfaces:

| Field | Type | Description |
|-------|------|-------------|
| `name` | `const char*` | Plugin name from `PluginInfo` |
| `version` | `const char*` | Plugin version from `PluginInfo` |
| `logger` | `IPluginLogger*` | Logging interface |
| `config` | `IPluginConfig*` | Config read/write interface |
| `scanner` | `IPluginScanner*` | Memory pattern scanner |
| `hooks` | `IPluginHooks*` | All hook and event sub-interfaces |

Store the pointer once in `PluginInit` and use it everywhere:

```cpp
static IPluginSelf* g_self = nullptr;

bool PluginInit(IPluginSelf* self)
{
    g_self = self;
    self->logger->Info(self, "Hello from %s v%s", self->name, self->version);
    self->config->InitializeFromSchema(self, &SCHEMA);
    self->hooks->Engine->RegisterOnInit(&OnEngineInit);
    return true;
}
```

The `plugin_helpers.h` from the SDK exposes convenience wrappers:

```cpp
IPluginSelf* GetSelf();

inline IPluginHooks*   GetHooks()   { auto* s = GetSelf(); return s ? s->hooks   : nullptr; }
inline IPluginConfig*  GetConfig()  { auto* s = GetSelf(); return s ? s->config  : nullptr; }
inline IPluginScanner* GetScanner() { auto* s = GetSelf(); return s ? s->scanner : nullptr; }
```

---

### IPluginLogger

Use the convenience macros from `plugin_helpers.h`. These log to `Plugins/logs/modloader.log`.

```cpp
LOG_TRACE("Very detailed trace: addr=0x%llX", addr);
LOG_DEBUG("Diagnostic: value=%d", value);
LOG_INFO ("General info");
LOG_WARN ("Something unexpected: %s", msg);
LOG_ERROR("Fatal condition encountered");
```

Macro definitions in `plugin_helpers.h`:

```cpp
#define LOG_INFO(fmt, ...) if (auto s = GetSelf()) s->logger->Info(s, fmt, ##__VA_ARGS__)
```

---

### IPluginConfig

All config functions take `const IPluginSelf* self` as their first argument. The plugin name is derived from `self->name` — no string literals required.

```cpp
// Schema-based initialization (recommended -- auto-creates INI with defaults)
self->config->InitializeFromSchema(self, &SCHEMA);

// Read
bool   b = self->config->ReadBool  (self, "Section", "Key", defaultBool);
int    i = self->config->ReadInt   (self, "Section", "Key", defaultInt);
float  f = self->config->ReadFloat (self, "Section", "Key", defaultFloat);
char buf[256];
self->config->ReadString(self, "Section", "Key", buf, sizeof(buf), "default");

// Write
self->config->WriteBool  (self, "Section", "Key", true);
self->config->WriteInt   (self, "Section", "Key", 42);
self->config->WriteFloat (self, "Section", "Key", 3.14f);
self->config->WriteString(self, "Section", "Key", "value");

// Repair a config file (adds missing keys with defaults, preserves existing values)
self->config->ValidateConfig(self, &SCHEMA);
```

Config files are stored in `Plugins/config/<PluginName>.ini`.

---

### IPluginScanner

Uses IDA-style byte patterns where `??` is a wildcard byte.

```cpp
// Find the first match in the main game executable
uintptr_t addr = scanner->FindPatternInMainModule("48 89 5C 24 ?? 57 48 83 EC 20");

// Find first match in a specific module
uintptr_t addr = scanner->FindPatternInModule(GetModuleHandle("module.dll"), "48 8B C4 ...");

// Find all matches (caller-buffer API, safe across DLL boundaries)
int count = scanner->FindAllPatternsInMainModule("E8 ?? ?? ?? ??", nullptr, 0); // query count
uintptr_t results[32];
scanner->FindAllPatternsInMainModule("E8 ?? ?? ?? ??", results, 32);

// Find a unique match from a list of candidates (useful for multi-version support)
const char* patterns[] = { "48 8B C4 48 89 58 ??", "48 89 5C 24 ?? 48 89 6C" };
int matched = 0;
uintptr_t addr = scanner->FindUniquePattern(patterns, 2, &matched);

// XRef scanning -- find all call sites / pointer slots referencing an address
PluginXRef xrefs[64];
int n = scanner->FindXrefsToAddressInMainModule(targetAddr, xrefs, 64);
for (int i = 0; i < n; i++) {
    // xrefs[i].address   -- address of the instruction or pointer slot
    // xrefs[i].isRelative -- true = relative CALL/JMP, false = absolute pointer
}
```

---

### IPluginHooks -- Sub-Interfaces

Starting with v14 the interface is entirely sub-interface based. Access every feature via a named group pointer:

```
hooks->Engine    -- engine lifecycle (init / shutdown / tick)
hooks->World     -- world and level events
hooks->Players   -- player join / leave
hooks->Actors    -- actor lifecycle
hooks->Spawner   -- enemy spawner Before/After hooks
hooks->Hooks     -- low-level hook install / remove / query
hooks->Memory    -- memory patch / nop / read / alloc
hooks->Input     -- keybind subscriptions      (CLIENT ONLY -- null on server)
hooks->UI        -- ImGui panels and widgets   (CLIENT ONLY -- null on server)
hooks->HUD       -- AHUD PostRender callbacks  (CLIENT ONLY -- null on server)
```

**Client-only sub-interfaces (`Input`, `UI`, `HUD`) are always `nullptr` on server and generic builds.** Always null-check before use:

```cpp
if (hooks->HUD) {
    hooks->HUD->RegisterOnPostRender(&MyCallback);
}
```

---

#### hooks->Engine

`IPluginEngineEvents` -- engine lifecycle subscriptions.

```cpp
// Called once when UGameEngine::Init completes
void OnEngineInit() { LOG_INFO("Engine ready"); }
hooks->Engine->RegisterOnInit(&OnEngineInit);
hooks->Engine->UnregisterOnInit(&OnEngineInit);  // call in PluginShutdown

// Called when the engine is shutting down
void OnEngineShutdown() { /* flush state */ }
hooks->Engine->RegisterOnShutdown(&OnEngineShutdown);
hooks->Engine->UnregisterOnShutdown(&OnEngineShutdown);

// Called every game-thread tick (UGameEngine::Tick)
void OnTick(float delta) { /* per-frame logic */ }
hooks->Engine->RegisterOnTick(&OnTick);
hooks->Engine->UnregisterOnTick(&OnTick);

// Resolved address of CoreUObject::StaticLoadObject (0 if not found). All builds.
uintptr_t addr = hooks->Engine->GetStaticLoadObjectAddress();
```

---

#### hooks->World

`IPluginWorldEvents` -- world and level lifecycle subscriptions.

```cpp
// Called when ChimeraMain world begins play (main game world only)
void OnWorldBeginPlay(SDK::UWorld* world) { /* ... */ }
hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
hooks->World->UnregisterOnWorldBeginPlay(&OnWorldBeginPlay);

// Called when ANY world begins play; worldName is a plain C string
void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName) {
    LOG_INFO("World: %s", worldName);
}
hooks->World->RegisterOnAnyWorldBeginPlay(&OnAnyWorldBeginPlay);
hooks->World->UnregisterOnAnyWorldBeginPlay(&OnAnyWorldBeginPlay);

// Called when UCrMassSaveSubsystem::OnSaveLoaded fires (save fully loaded)
void OnSaveLoaded() { /* read save-dependent state */ }
hooks->World->RegisterOnSaveLoaded(&OnSaveLoaded);
hooks->World->UnregisterOnSaveLoaded(&OnSaveLoaded);

// Called when UCrExperienceManagerComponent::OnExperienceLoadComplete fires
void OnExperienceLoadComplete() { /* game mode fully loaded */ }
hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
```

---

#### hooks->Players

`IPluginPlayerEvents` -- player join and leave events (server-side).

```cpp
// Called when ACrGameModeBase::PostLogin fires (player fully connected)
void OnPlayerJoined(void* playerController) {
    // Cast to SDK::ACrPlayerController* if needed
}
hooks->Players->RegisterOnPlayerJoined(&OnPlayerJoined);
hooks->Players->UnregisterOnPlayerJoined(&OnPlayerJoined);

// Called when ACrGameModeBase::Logout fires (player about to disconnect)
// The controller is still valid at callback time.
void OnPlayerLeft(void* exitingController) { /* ... */ }
hooks->Players->RegisterOnPlayerLeft(&OnPlayerLeft);
hooks->Players->UnregisterOnPlayerLeft(&OnPlayerLeft);
```

---

#### hooks->Actors

`IPluginActorEvents` -- actor lifecycle events.

```cpp
// Called when any AActor::BeginPlay fires
void OnActorBeginPlay(void* actor) {
    // Cast to SDK::AActor* or a more derived type as needed
}
hooks->Actors->RegisterOnActorBeginPlay(&OnActorBeginPlay);
hooks->Actors->UnregisterOnActorBeginPlay(&OnActorBeginPlay);
```

---

#### hooks->Spawner

`IPluginSpawnerHooks` -- Before/After hooks for enemy spawner functions.

**Before callbacks return `bool`. Returning `true` cancels the operation and suppresses the original call and all After callbacks.**

```cpp
// AAbstractMassEnemySpawner::ActivateSpawner(bool bDisableAggroLock)
bool OnBeforeActivate(void* spawner, bool bDisableAggroLock) {
    return false; // return true to cancel
}
void OnAfterActivate(void* spawner, bool bDisableAggroLock) { /* fires if not cancelled */ }
hooks->Spawner->RegisterOnBeforeActivate(&OnBeforeActivate);
hooks->Spawner->RegisterOnAfterActivate(&OnAfterActivate);
hooks->Spawner->UnregisterOnBeforeActivate(&OnBeforeActivate);
hooks->Spawner->UnregisterOnAfterActivate(&OnAfterActivate);

// AAbstractMassEnemySpawner::DeactivateSpawner(bool bPermanently)
bool OnBeforeDeactivate(void* spawner, bool bPermanently) { return false; }
void OnAfterDeactivate(void* spawner, bool bPermanently)  { }
hooks->Spawner->RegisterOnBeforeDeactivate(&OnBeforeDeactivate);
hooks->Spawner->RegisterOnAfterDeactivate(&OnAfterDeactivate);
hooks->Spawner->UnregisterOnBeforeDeactivate(&OnBeforeDeactivate);
hooks->Spawner->UnregisterOnAfterDeactivate(&OnAfterDeactivate);

// AMassSpawner::DoSpawning()
bool OnBeforeDoSpawning(void* spawner) { return false; }
void OnAfterDoSpawning(void* spawner)  { }
hooks->Spawner->RegisterOnBeforeDoSpawning(&OnBeforeDoSpawning);
hooks->Spawner->RegisterOnAfterDoSpawning(&OnAfterDoSpawning);
hooks->Spawner->UnregisterOnBeforeDoSpawning(&OnBeforeDoSpawning);
hooks->Spawner->UnregisterOnAfterDoSpawning(&OnAfterDoSpawning);
```

---

#### hooks->Hooks

`IPluginHookUtils` -- low-level function hook install, remove, and query.

```cpp
typedef void(__fastcall* MyFunc_t)(void* thisPtr, int param);
static MyFunc_t  g_original = nullptr;
static HookHandle g_hook    = nullptr;

void __fastcall MyDetour(void* thisPtr, int param) {
    LOG_DEBUG("Called: param=%d", param);
    if (g_original) g_original(thisPtr, param);
}

// Install in PluginInit
g_hook = hooks->Hooks->Install(targetAddress, (void*)&MyDetour, (void**)&g_original);

// Query
bool active = hooks->Hooks->IsInstalled(g_hook);

// Remove in PluginShutdown
hooks->Hooks->Remove(g_hook);
g_hook     = nullptr;
g_original = nullptr;
```

---

#### hooks->Memory

`IPluginMemoryUtils` -- memory patch, nop, read, and engine allocator.

```cpp
// Patch bytes at an address
uint8_t patch[] = { 0x90, 0x90 };
hooks->Memory->Patch(address, patch, 2);

// NOP a range
hooks->Memory->Nop(address, 5);

// Read memory safely
uint8_t buf[16];
hooks->Memory->Read(address, buf, sizeof(buf));

// Engine allocator (FMemory::Malloc / Free) -- use for FString / engine-owned memory
if (hooks->Memory->IsAllocatorAvailable()) {
    void* mem = hooks->Memory->Alloc(256, 16); // count=256 bytes, alignment=16
    // write engine-compatible data into mem
    hooks->Memory->Free(mem);
}
```

---

#### hooks->Input

`IPluginInputEvents` -- keybind subscriptions. **Client builds only. Always null-check.**

```cpp
if (!hooks->Input) return; // not a client build

// By enum (EModKey)
void OnF1Pressed(EModKey key, EModKeyEvent event) {
    LOG_INFO("F1 pressed");
}
hooks->Input->RegisterKeybind(EModKey::F1, EModKeyEvent::Pressed, &OnF1Pressed);
hooks->Input->UnregisterKeybind(EModKey::F1, EModKeyEvent::Pressed, &OnF1Pressed);

// By UE key name string (case-insensitive)
hooks->Input->RegisterKeybindByName("LeftShift", EModKeyEvent::Released, &OnShiftReleased);
hooks->Input->UnregisterKeybindByName("LeftShift", EModKeyEvent::Released, &OnShiftReleased);
```

Available enum values include F1-F12, A-Z, digit keys, navigation, modifier keys, numpad, and mouse buttons. See `EModKey` in `plugin_interface.h` for the full list.

---

#### hooks->UI

`IPluginUIEvents` -- ImGui panel and widget registration. **Client builds only. Always null-check.**

All ImGui drawing is done through the `IModLoaderImGui` function table passed to your render callback. Do not call `ImGui::` directly from plugins.

**Panels** appear as buttons in the ModLoader Config tab and open a floating window when clicked:

```cpp
static PanelHandle g_panel = nullptr;

void RenderMyPanel(IModLoaderImGui* imgui) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Value: %d", g_someValue);
    imgui->Text(buf);
    if (imgui->Button("Do thing")) { /* ... */ }
}

// Register in PluginInit
if (hooks->UI) {
    static PluginPanelDesc desc = { "My Plugin", "My Plugin Panel", &RenderMyPanel };
    g_panel = hooks->UI->RegisterPanel(&desc);
}

// Programmatically open/close
if (g_panel) {
    hooks->UI->SetPanelOpen(g_panel);
    hooks->UI->SetPanelClose(g_panel);
}

// Unregister in PluginShutdown
if (hooks->UI && g_panel) {
    hooks->UI->UnregisterPanel(g_panel);
    g_panel = nullptr;
}
```

**Widgets** are always-on ImGui windows rendered every frame regardless of whether the modloader window is open:

```cpp
static WidgetHandle g_widget = nullptr;

void RenderMyWidget(IModLoaderImGui* imgui) {
    imgui->Text("Always visible overlay");
}

// Register in PluginInit
if (hooks->UI) {
    static PluginWidgetDesc wdesc = { "MyPlugin_Widget", &RenderMyWidget };
    g_widget = hooks->UI->RegisterWidget(&wdesc);
}

// Show/hide (e.g. in response to a keybind)
if (hooks->UI && g_widget) {
    hooks->UI->SetWidgetVisible(g_widget, true);
    hooks->UI->SetWidgetVisible(g_widget, false);
}

// Unregister in PluginShutdown
if (hooks->UI && g_widget) {
    hooks->UI->UnregisterWidget(g_widget);
    g_widget = nullptr;
}
```

**Config change notifications** let your plugin react when the user edits values in the modloader UI:

```cpp
void OnConfigChanged(const char* section, const char* key, const char* newValue) {
    LOG_INFO("Config changed: [%s] %s = %s", section, key, newValue);
}
if (hooks->UI) {
    hooks->UI->RegisterOnConfigChanged(&OnConfigChanged);
}
// In PluginShutdown:
if (hooks->UI) {
    hooks->UI->UnregisterOnConfigChanged(&OnConfigChanged);
}
```

---

#### hooks->HUD

`IPluginHUDEvents` -- HUD PostRender callbacks and HUD-related function addresses. **Client builds only. Always null-check.**

```cpp
if (!hooks->HUD) return;

// Per-frame callback fired after AHUD::PostRender
// The engine draws its own HUD before your callback fires.
void OnHUDPostRender(void* hud) {
    // Cast hud to SDK::AHUD* if you have the client SDK headers
    // Draw overlays here using the SDK or raw canvas calls
}
hooks->HUD->RegisterOnPostRender(&OnHUDPostRender);
hooks->HUD->UnregisterOnPostRender(&OnHUDPostRender); // call in PluginShutdown

// Resolved address of UCrMapManuSubsystem::GatherPlayersData, or 0 if not found.
// Cast and call with the subsystem instance to force a player data refresh.
uintptr_t gatherAddr = hooks->HUD->GetGatherPlayersDataAddress();
```

---

#### hooks->Network

`IPluginNetworkChannel` -- typed packet send/receive between client and server. Available on **both client and server builds**. Always `nullptr` on generic (non-client, non-server) builds.

Use the typed helpers from `plugin_network_helpers.h` rather than calling `IPluginNetworkChannel` directly.

**Define a packet:**

```cpp
#pragma pack(push, 1)
struct MyPacket {
    float x, y, z;
    int32_t flags;
};
#pragma pack(pop)
```

**Server -- send to clients:**

```cpp
#include "plugin_network_helpers.h"

// Send to all connected clients
MyPacket pkt = { 1.0f, 2.0f, 3.0f, 42 };
Network::SendPacketToAllClients(g_self->hooks, g_self, pkt);

// Send to a specific player controller
Network::SendPacketToPlayer(g_self->hooks, g_self, playerController, pkt);

// Receive on server (register in PluginInit)
Network::OnServerReceive<MyPacket>(g_self->hooks, g_self,
    [](void* playerController, const MyPacket& p) {
        // handle packet from a client
    });
```

**Client -- send to server:**

```cpp
// Send to server
Network::SendPacketToServer(g_self->hooks, g_self, pkt);

// Receive on client (register in PluginInit)
Network::OnReceive<MyPacket>(g_self->hooks, g_self,
    [](const MyPacket& p) {
        // handle packet from the server
    });
```

**Null-check on generic builds:**

```cpp
if (g_self->hooks->Network) {
    Network::SendPacketToAllClients(g_self->hooks, g_self, pkt);
}
```

Packets are routed by plugin name (from `self->name`) and type tag, so different plugins cannot accidentally receive each other's packets.

---

#### hooks->NativePointers

`IPluginNativePointers` -- resolved addresses of every function the modloader hooks internally (v21). Useful for installing your own hook on top of the same site, or for verifying a pattern scan result against a known address.

All functions return `0` if the address was not resolved (pattern not found, or not applicable to the current build).

```cpp
auto* np = hooks->NativePointers;

uintptr_t tickAddr    = np->EngineTick();
uintptr_t wbpAddr     = np->WorldBeginPlay();
uintptr_t actorAddr   = np->ActorBeginPlay();
uintptr_t hudAddr     = np->HUDPostRender(); // 0 on server/generic
```

Full list of available accessors:

| Method | Description |
|--------|-------------|
| `EngineLoopInit()` | FEngineLoop::Init |
| `GameEngineInit()` | UGameEngine::Init |
| `EngineLoopExit()` | FEngineLoop::Exit |
| `EnginePreExit()` | EnginePreExit |
| `EngineTick()` | UGameEngine::Tick |
| `WorldBeginPlay()` | UWorld::BeginPlay |
| `WorldEndPlay()` | UWorld::EndPlay |
| `SaveLoaded()` | UCrMassSaveSubsystem::OnSaveLoaded |
| `ExperienceLoadComplete()` | UCrExperienceManagerComponent::OnExperienceLoadComplete |
| `ActorBeginPlay()` | AActor::BeginPlay |
| `PlayerJoined()` | ACrGameModeBase::PostLogin |
| `PlayerLeft()` | ACrGameModeBase::Logout |
| `SpawnerActivate()` | AAbstractMassEnemySpawner::ActivateSpawner |
| `SpawnerDeactivate()` | AAbstractMassEnemySpawner::DeactivateSpawner |
| `SpawnerDoSpawning()` | AMassSpawner::DoSpawning |
| `HUDPostRender()` | AHUD::PostRender -- client only, 0 on server/generic |
| `ClientMessageExec()` | Client message exec -- client only, 0 on server/generic |

---

#### hooks->HttpServer

`IPluginHttpServer` -- HTTP intercept interface (v22). **Server builds only. Always `nullptr` on client and generic builds.** Always null-check before use.

The modloader embeds a lightweight HTTP server. `hooks->HttpServer` lets your plugin mount routes under `/<pluginName>/...` (case-insensitive). Three route types are available:

**Static file routes** serve files from disk:

```cpp
// URL: /<self->name>/ui/...
// Files served from: <exe_dir>\Plugins\<self->name>\ui\
if (hooks->HttpServer) {
    hooks->HttpServer->AddRoute(self, "ui");
}
// In PluginShutdown:
if (hooks->HttpServer) {
    hooks->HttpServer->RemoveRoute(self, "ui");
}
```

**Raw-response routes** let your plugin handle any URL prefix and write any response body:

```cpp
void MyApiHandler(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
    // req->url, req->method, req->body, req->bodyLen are available
    static const char* body = "{\"status\":\"ok\"}";
    resp->statusCode   = 200;
    resp->contentType  = "application/json";
    resp->body         = body;
    resp->bodyLen      = strlen(body);
}

// URL matched: /<self->name>/api/...
if (hooks->HttpServer) {
    hooks->HttpServer->AddRawRoute(self, "api", &MyApiHandler);
}
// In PluginShutdown:
if (hooks->HttpServer) {
    hooks->HttpServer->RemoveRawRoute(self, "api");
}
```

**Raw-request filters** inspect every incoming request before any route fires. Return `HttpRequestAction::Deny` to send a `403 Forbidden` and stop all further processing; return `HttpRequestAction::Approve` to continue:

```cpp
HttpRequestAction MyFilter(const PluginHttpRequest* req)
{
    if (strstr(req->url, "/admin") && !IsAuthorized(req))
        return HttpRequestAction::Deny;
    return HttpRequestAction::Approve;
}

if (hooks->HttpServer) {
    hooks->HttpServer->RegisterOnRawRequest(&MyFilter);
}
// In PluginShutdown:
if (hooks->HttpServer) {
    hooks->HttpServer->UnregisterOnRawRequest(&MyFilter);
}
```

Processing order for every incoming request:
1. Raw-request filters — first `Deny` sends 403 and stops everything.
2. Raw-response routes — plugin-owned handler.
3. Static-file routes — served from disk.
4. Pass-through — engine's built-in handler (404 for unknown paths).

---

## Interface Version Changelog

The loader accepts plugins whose `interfaceVersion` is in `[PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX]`. All interface structs are append-only so older plugins still load without recompilation as long as they are within the supported range.

| Version | MIN bumped? | Changes |
|---------|-------------|---------|
| v1      | yes         | Initial flat `IPluginHooks`: `RegisterEngineInitCallback` |
| v2      | no          | Added `RegisterEngineShutdownCallback` / `Unregister` to `IPluginHooks` |
| v3      | no          | Replaced `std::vector` returns in `IPluginScanner` with caller-buffer API to fix cross-DLL heap corruption |
| v4      | no          | Added `FindXrefsToAddress`, `FindXrefsToAddressInModule`, `FindXrefsToAddressInMainModule` to `IPluginScanner` |
| v5      | no          | Added `EngineAlloc`, `EngineFree`, `IsEngineAllocatorAvailable` to `IPluginHooks` for safe FString/engine memory manipulation |
| v6      | no          | Added `RegisterAnyWorldBeginPlayCallback` / `Unregister` to `IPluginHooks` |
| v7      | no          | Added `RegisterSaveLoadedCallback` / `Unregister` to `IPluginHooks` |
| v8      | no          | Added `RegisterExperienceLoadCompleteCallback` / `Unregister` to `IPluginHooks` |
| v9      | no          | Added `RegisterEngineTickCallback` / `Unregister` to `IPluginHooks` |
| v10     | no          | Added `RegisterActorBeginPlayCallback` / `Unregister` to `IPluginHooks` |
| v11     | no          | Added `RegisterPlayerJoinedCallback` / `Unregister` to `IPluginHooks` |
| v12     | no          | Added `RegisterPlayerLeftCallback` / `Unregister` to `IPluginHooks` |
| v13     | --          | (skipped) |
| v14     | **yes**     | **ABI break.** Replaced all flat callbacks with typed sub-interface pointers. `IPluginHooks` now contains only group pointers (`Spawner`, `Hooks`, `Memory`, `Engine`, `World`, `Players`, `Actors`). Access via `hooks->Engine->RegisterOnInit(...)`. Added 10 named callback typedefs. MIN bumped to 14. |
| v15     | no          | Added `EModKey`, `EModKeyEvent`, `PluginKeybindCallback`, `IPluginInputEvents` and `hooks->Input` (client only, nullptr on server/generic). Also folded: `IModLoaderImGui` function table, `PluginImGuiRenderCallback`, `PluginPanelDesc`, `PanelHandle`, `PluginConfigChangedCallback`, `IPluginUIEvents` and `hooks->UI` (client only). `RegisterPanel` returns a `PanelHandle`; `SetPanelOpen`/`SetPanelClose` take a handle instead of a title string. |
| v16     | no          | Added `PluginWidgetDesc`, `WidgetHandle`, `RegisterWidget`, `UnregisterWidget`, `SetWidgetVisible` to `IPluginUIEvents` for always-on overlay windows. Added `PluginHUDPostRenderCallback`, `IPluginHUDEvents`, `hooks->HUD` (client only, nullptr on server/generic) with `RegisterOnPostRender` and `GetGatherPlayersDataAddress`. Extended `IPluginEngineEvents` with `GetStaticLoadObjectAddress()` (all builds). Byte patterns for `AHUD_PostRender`, `StaticLoadObject`, and `GatherPlayersData` moved from `Compass_Plugin` into the modloader's `scan_patterns.h`. |
| v17     | no          | Added `IPluginNetworkChannel` and `hooks->Network` (server + client builds; nullptr on generic). Typed packet routing between client and server by plugin name + type tag. Added `plugin_network_helpers.h` with `SendPacketToAllClients`, `SendPacketToPlayer`, `SendPacketToServer`, `OnReceive`, `OnServerReceive` template helpers. |
| v18     | no          | Extended `IPluginNetworkChannel` with `SendPacketToServer`, `RegisterServerMessageHandler`, `UnregisterServerMessageHandler`, `ExcludeFromBroadcast`, `UnexcludeFromBroadcast`. Added `PostToGameThread` to `IPluginEngineEvents` for safely dispatching work back to the game thread from any thread. |
| v19     | **yes**     | **ABI break.** Introduced `IPluginSelf` — a per-plugin identity and service bundle (`name`, `version`, `logger`, `config`, `scanner`, `hooks`). `PluginInit` signature changed from four separate pointers to `PluginInit(IPluginSelf* self)`. Removed `const char* pluginName` from all `IPluginLogger`, `IPluginConfig`, and `IPluginNetworkChannel` functions — replaced with `const IPluginSelf* self`. MIN bumped to 19. |
| v20     | no          | Added `RegisterOnBeforeWorldEndPlay` / `UnregisterOnBeforeWorldEndPlay` and `RegisterOnAfterWorldEndPlay` / `UnregisterOnAfterWorldEndPlay` to `IPluginWorldEvents`. Callback type: `PluginWorldEndPlayCallback(UWorld* world, const char* worldName)`. |
| v21     | no          | Added `IPluginNativePointers` and `hooks->NativePointers` (all builds). Exposes the resolved addresses of every function the modloader hooks internally so plugins can install secondary hooks or validate pattern scan results without re-scanning. |
| v22     | no          | Added `IPluginHttpServer` and `hooks->HttpServer` (server only; nullptr on client/generic). Provides static-file routes (`AddRoute`/`RemoveRoute`), raw-request filter hooks (`RegisterOnRawRequest`/`UnregisterOnRawRequest`), and raw-response routes (`AddRawRoute`/`RemoveRawRoute`). URL scheme: `/<pluginName>/<routeName>/...` (case-insensitive). Static files served from `<exe_dir>\Plugins\<pluginName>\<folderName>\`. |

The current `PLUGIN_INTERFACE_VERSION_MIN` is **19** and `PLUGIN_INTERFACE_VERSION_MAX` is **22**.

---

## Troubleshooting

### Plugin Not Loading

- Verify the DLL is in `Plugins/` and exports all three functions (`GetPluginInfo`, `PluginInit`, `PluginShutdown`).
- Check that `interfaceVersion` in `PluginInfo` is within `[MIN, MAX]`. Use the `PLUGIN_INTERFACE_VERSION` macro.
- Check `Plugins/logs/modloader.log` for initialization errors.
- Ensure the DLL was built for the matching configuration (client/server).

### Pattern Not Found

- Verify the pattern against a disassembler (IDA, Ghidra, x64dbg).
- Game updates change binary layouts -- patterns may need updating after a patch.
- Use `FindAllPatternsInMainModule` to check for zero or multiple matches.
- Check that client-only patterns are not scanned in a server build.

### Hook Crashes

- Ensure the calling convention is correct (`__fastcall` for most UE5 member functions on x64).
- Verify the function signature matches exactly (parameter count and types).
- Always call the original function pointer from the detour (unless intentionally blocking).
- Wrap hook bodies in SEH (`__try` / `__except`) for crash isolation.

### Client-Only Interface is Null

- `hooks->Input`, `hooks->UI`, and `hooks->HUD` are always `nullptr` on server and generic builds.
- `hooks->Network` is always `nullptr` on generic builds (available on both client and server).
- Always null-check before using these pointers.
- If your plugin targets both client and server, guard client-only code with a null check, not with `#ifdef`.

### Config Not Saving

- Ensure `Plugins/config/` exists (created automatically on first run).
- Check file write permissions in the game directory.

### MSVC Static Local Initialization Error (C2065)

- MSVC can misparse `static T var = ComplexExpression()` inside a namespace function.
- Promote the static to module scope with a separate `bool g_scanned = false` sentinel and check it manually.

---

## Contributing

1. Fork the repository.
2. Create a feature branch from `main`.
3. Follow existing code style: tabs for indentation, existing naming conventions (`g_` for module statics, `s_` for function statics).
4. Add logging to all significant code paths.
5. Test with Debug and Release builds for both Client and Server configurations.
6. Submit a pull request.

---

## License

This project is provided as-is for educational purposes.
