# Read game values from the version map

A game update moves functions, shifts members and resizes structs. A plugin that bakes in an address or an offset breaks, or worse, writes to the wrong place. So a plugin never hard-codes one. It asks the loader by name.

The loader keeps the answers in its version object map, called the version map (or v map) from here on. It has a row per key and game build: function addresses, member offsets and struct sizes. Your plugin asks for a key, and gets the value for the build that is running. When the game updates, we add rows to the version map and your plugin keeps working. You do not rebuild it.

## What "multi-version" means

A multi-version plugin is one DLL that loads on every current game build. Two game builds can be live at once, so a plugin that is not multi-version breaks for part of the players. The builds the version map covers are the rows in the loader's `StarRupture-ModLoader-Core/hooks/game/version_map.generated.h`, each tagged with its CL number.

A plugin is multi-version when it reads every per-build value from the loader. It then says so with `PLUGIN_MAP_MIGRATED`.

## What `PLUGIN_MAP_MIGRATED` promises

Put `PLUGIN_GAME_BUILT_CL` and `PLUGIN_MAP_MIGRATED` in `PluginInfo`. ExamplePlugin leaves the target, game build and flag at their defaults, so add them:

```cpp
static PluginInfo s_pluginInfo = {
    "MyPlugin", "1.0.0", "Your Name", "What this plugin does",
    PLUGIN_INTERFACE_VERSION, PLUGIN_TARGET_CLIENT,
    PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED
};
```

The flag promises that the plugin reads no game function address, member offset or struct size from a value compiled in. The loader does not check the promise. What it does with the flag:

- On the build the plugin was compiled for, it loads as usual.
- On any other build it knows, it loads only a plugin with the flag. Without it, the Plugins tab shows "Incompatible: needs an update to run on CL-...", and the plugin still loads on its own build.
- A plugin with the flag and no game build (`PLUGIN_GAME_BUILT_CL` is 0) is refused on every build.

The SDK's `tools/rule3-check.mjs` scans your sources for direct reads of game members, which is the usual way a promise gets broken. Run it before you set the flag. It needs Node.js:

```
node tools/rule3-check.mjs --plugin MyPlugin --sdk "StarRupture SDK\Client\SDK" --source MyPlugin
```

## The calls

Include `plugin_map.h` after `plugin_interface.h` and the game headers. Ask for everything once, in `OnPluginLoadHooks`.

| You need | Call |
|---|---|
| A game function's address | `PluginMap::Function(scanner, self, "Class::Function", nullptr)`, or `FunctionAs<Fn>` for a typed pointer |
| A game global | `PluginMap::Global<T>(scanner, self, "GWorld")` |
| An address inside a function of your own | `PluginMap::Site(...)`; it needs a pattern of its own, and a row only after we add it |
| A member's offset | `SR_FIELD(Type, Class, Member, 0)`, read with `.In(object)` |
| A bool bitfield member | `SR_BITFIELD(Class, Member)`, read with `.Get(object)` and `.Set(object, value)` |
| A struct's size | `SR_SIZE(Class)` (live size, for stepping through an array) or `SR_SIZE_CHECK(Class)` (a changed size refuses the plugin, or turns off only its feature if the key is in a feature, for a struct you copy) |
| A member whose size differs between builds | `SR_FIELD_ANY_SIZE`, then read `resolvedSize` |
| A function parameter | `SR_PARAM(Class, Function, Param)` and `SR_PARAMS_SIZE(Class, Function)`, used with `PluginMap::ParamsBlockOf` |

Then `PluginMap::ResolveFields(scanner, self, field1, field2, ...)` asks for all the member and size rows. Three more rules:

1. Never write `object->Member` for a game member. Write `field.In(object)`. The first reads the offset of the build you compiled on.
2. Call `PluginSdkRuntime::Init(self->hooks)` as the first statement of `PluginInit`, and use `PluginSdkRuntime::GetWorld()` in place of `SDK::UWorld::GetWorld()`.
3. A key that cannot be resolved refuses the plugin. To lose only one part of it, group the keys that part needs in a `PluginMap::Feature` and declare it with `PluginMap::DeclareFeatures` before the calls. A miss then turns that feature off and the plugin still loads. The form is `static PluginMap::Feature s_x{ "id", "Title a player reads", { "Key", ... }, { "Settings.*" } };`. `s_example` in `ExamplePlugin/plugin.cpp` is a working one.

Most members are reflected, so the loader reads their offset from the running game and a row is only a cross-check. A member that reflection does not describe (a plain C++ type) is called native. It needs a row for every build, and the flag `PLUGIN_FIELD_NATIVE`.

## Choosing a size or layout by build

When a member or struct changes between the two live builds, do not assume the new one. Ask for it with `SR_FIELD_ANY_SIZE` (or `SR_NATIVE_ANY_SIZE` for a native member) and read the size the loader reports in `resolvedSize`. Handle the sizes you know, such as a float on one build and a double on the next, and treat any other size as the member not being there.

## Older loaders

The loader refuses a plugin for version reasons only when the plugin cannot run on it. Interface changes are append-only, so a plugin that uses nothing newer loads on an older loader. When your plugin uses a call an older loader lacks, put its key in a feature. On that loader the feature turns off and the plugin loads. `plugin_map.h` names the minimum loader beside each call that needs one, for example loader 1.23.2 for `SR_PARAM`, `SR_PARAMS_SIZE` and `SR_NATIVE_ANY_SIZE`. Where a plugin cannot work at all, make its message name that minimum ModLoader version.

## Worked example: ConfirmHotkey

ConfirmHotkey reads one member of a game widget. These lines are from the `gss/plugin-work` branch of [msantoro12/StarRupture-Plugin-ConfirmHotkey](https://github.com/msantoro12/StarRupture-Plugin-ConfirmHotkey); the upstream `main` does not read its members through the version map yet. Its declaration, in `ModCore.cpp`:

```cpp
static auto s_claimButton = SR_FIELD(decltype(SDK::UCrUW_Analyzer::ClaimButton), UCrUW_Analyzer, ClaimButton, 0);
```

It asks for it once in `OnPluginLoadHooks`:

```cpp
return PluginMap::ResolveFields(scanner, self, s_claimButton, s_overlayButton);
```

It reads it with the helper that returns null when the member did not resolve:

```cpp
if (auto* claimButton = PluginMap::ValueOr(s_claimButton, widget))
```

Its `PluginInfo` ends with `PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED`, and `PluginInit` starts with `PluginSdkRuntime::Init(self->hooks)`. The key `UCrUW_Analyzer::ClaimButton` is reflected, so it needs no row from us.

## Checklist

1. `PLUGIN_GAME_BUILT_CL` is not 0, and `PLUGIN_MAP_MIGRATED` is set.
2. Every game function address comes from `PluginMap::Function`, `FunctionAs`, `Global` or `Site`.
3. Every game member is read through `SR_FIELD` or `SR_BITFIELD`, and the rule 3 check passes.
4. `PluginSdkRuntime::Init` is the first statement of `PluginInit`.
5. `ResolveFields` asks for every field and size in `OnPluginLoadHooks`.
6. You loaded the plugin on at least two game builds and read `ModLoader\Logs\ModLoader.log`: each key has a `[Map]` line.

If a key has no row, page 3 says how to add one.
