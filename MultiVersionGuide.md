# Make your plugin work on every game build

Follow these steps and your plugin is one DLL that runs on every game build the loader knows. You do not rebuild it when the game updates. The loader keeps the per-build game data (function addresses, member offsets, struct sizes) in its version object map, called the version map below. Your plugin asks it for each value by name, instead of baking in the value from the build you compiled on.

## Steps

1. **Set your game build.** Import `PluginGameBuild.props` from your plugin's `Shared.props`. It defines `PLUGIN_GAME_BUILT_CL` from the Game-SDK headers you compile against.
2. **Ask for every game member by name.** Declare each one with `SR_FIELD`, and request them all in `OnPluginLoadHooks` with `PluginMap::ResolveFields`.
3. **Read members with `.In(object)`.** Never write `object->Member`: that reads the offset of the build you compiled on.
4. **Ask for every game function by key.** Use `PluginMap::Function` or `FunctionAs`, in `OnPluginLoadHooks`.
5. **Declare the plugin migrated.** Put `PLUGIN_GAME_BUILT_CL` and `PLUGIN_MAP_MIGRATED` in `PluginInfo`. Only a migrated plugin may run on a game build it was not compiled for.
6. **Take the SDK globals first.** Call `PluginSdkRuntime::Init(self->hooks)` as the first statement of `PluginInit`, and use `PluginSdkRuntime::GetWorld()` instead of `SDK::UWorld::GetWorld()`.

The last argument of `SR_FIELD` is the flags argument. `0` means no flag: the loader answers with the running build's offset from the version map, or from the game's reflection when the map has no row. Add a flag only when you must: `PLUGIN_FIELD_NATIVE` for a member the game's reflection does not describe, `PLUGIN_FIELD_CHECK` for a struct you copy by value.

Reflected members are read from the running game on the client and on a dedicated server. The loader also carries version map rows for the dedicated server builds it knows (currently CL 127004: functions, globals, member offsets and struct sizes). They are served only when the running server exe matches that build exactly. A server plugin asks for the same keys as a client plugin and reads them from the map. On a server build the loader has no rows for, a member with a row of your own for the running build uses that row, and a member without one is read by reflection ([Game-class fields](PluginDevelopment.md#game-class-fields-resolvefield)). A member the game's reflection does not describe needs your own row there; [ask us](#ask-us-for-rows) if you need one.

## Worked example

`Shared.props`, after `GameSDKRoot`, `StarRuptureSDKConfigDir` and `PluginSDKInclude` are set:

```xml
<Import Project="$(PluginSDKInclude)\..\PluginGameBuild.props"
        Condition="Exists('$(PluginSDKInclude)\..\PluginGameBuild.props')" />
```

`plugin.cpp`:

```cpp
#include <Engine_classes.hpp>
#include <Chimera_classes.hpp>   // ACrCrafter, UCrCraftingComponent
#include <plugin_map.h>   // after plugin_interface.h and the Game-SDK headers

// PLUGIN_MAP_PATTERN keeps this pattern for the build that reads your plugin's keys. A shipped
// build passes nullptr, so the version map answers, and a key it lacks fails.
static constexpr const char* kBeginPlayPattern = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ??";

static void (*g_beginPlay)(SDK::UWorld*) = nullptr;
static auto g_craftComponent = SR_FIELD(SDK::UCrCraftingComponent*, ACrCrafter, CraftComponent, 0);

static PluginInfo s_pluginInfo = {
    "MyPlugin", "1.0.0", "Your Name", "What this plugin does",
    PLUGIN_DECLARED_INTERFACE, PLUGIN_TARGET_CLIENT,
    PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED
};

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo() { return &s_pluginInfo; }

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scan)
{
    g_beginPlay = PluginMap::FunctionAs<void (*)(SDK::UWorld*)>(scan, self, "UWorld::BeginPlay",
        PLUGIN_MAP_PATTERN(kBeginPlayPattern));
    PluginMap::ResolveFields(scan, self, g_craftComponent);
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    PluginSdkRuntime::Init(self->hooks);   // first, before any early return
    return true;
}

void UseComponent(SDK::UCrCraftingComponent* component);   // your code

// Later, on the game thread:
void OnCrafter(SDK::ACrCrafter* crafter)
{
    if (auto* component = g_craftComponent.In(crafter))    // null when not resolved
        UseComponent(*component);
    SDK::UWorld* world = PluginSdkRuntime::GetWorld();      // not SDK::UWorld::GetWorld()
}
```

On a game build your plugin was not compiled for, `g_craftComponent` holds the offset of that build and `g_beginPlay` holds that build's address. If a value cannot be found, the plugin is refused, unless a feature lists the key and only that feature turns off ([Features](PluginDevelopment.md#features----declarefeature)).

## Checklist

1. `PLUGIN_GAME_BUILT_CL` is not 0, and `PLUGIN_MAP_MIGRATED` is set.
2. Every game function address comes from `PluginMap::Function`, `FunctionAs`, `Global` or `Site`.
3. Every game member is read through `SR_FIELD`, `SR_BITFIELD` or `PluginLazyField`, and the rule 3 check passes.
4. `PluginSdkRuntime::Init(self->hooks)` is the first statement of `PluginInit`, and nothing calls `SDK::UWorld::GetWorld()`.
5. `PluginMap::ResolveFields` asks for every field and size row in `OnPluginLoadHooks`.

## Ask us for rows

Send us the `Class::Member` list and the function names your plugin uses, and we publish the rows in the version map before your release. The version map goes out on its own, so no loader release is needed. To send the list, open an issue on this repository.

## More detail

- [Map keys](PluginDevelopment.md#map-keys-plugin_scan_flag_map_key): how the loader picks a row for a function or global.
- [Game-class fields](PluginDevelopment.md#game-class-fields-resolvefield): the flags and what each one does.
- [SDK helpers](PluginDevelopment.md#sdk-helpers-plugin_maph-plugin_version_maph): `plugin_map.h` and every helper in it.
- [Your game build](PluginDevelopment.md#your-game-build-plugin_game_built_cl): what `PluginGameBuild.props` does.
- [Struct sizes](PluginDevelopment.md#struct-sizes-live-or-checked): `SR_SIZE` and `SR_SIZE_CHECK`.
- [GetWorld](PluginDevelopment.md#getworld-the-viewports-world): why `PluginSdkRuntime::GetWorld()` replaces `SDK::UWorld::GetWorld()`.
- [Features](PluginDevelopment.md#features----declarefeature): keep a plugin running when one value is missing. The migration checklist ends that section.
- [Rule 3 check](PluginDevelopment.md#rule-3-check----no-direct-reads-of-game-members): finds direct reads of game members in your sources.
- [Getting a value added to the shared map](PluginDevelopment.md#getting-a-value-added-to-the-shared-map): carrying your own rows meanwhile.
