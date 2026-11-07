# From fingerprinting to mapping

Your plugin finds game code with byte patterns and reads game data at fixed offsets and sizes.
This guide moves each of those onto the loader's version map, one step at a time. For the full
API, see [PluginDevelopment.md](PluginDevelopment.md).

## 1. Why

A game update moves functions, shifts members and resizes structs. A pattern that no longer
matches, or an offset that now points at something else, breaks the plugin or corrupts memory.
The loader's version map holds a row per game build for each key, so your plugin asks for a
value by name and gets the one for the build that is running. Your pattern stays as a fallback.

## 2. Turn each hard-coded value into a key

| You have | Key | Written as |
|---|---|---|
| A pattern for a game function | `Class::Function` | `PluginMap::Function` / `FunctionAs` |
| A pattern for a game global | the global's name, e.g. `GWorld` | `PluginMap::Global` |
| A pattern for a spot inside a function (call-site anchor) | `<PluginName>/<hookName>` | `PluginMap::Site` |
| A member offset | `Class::Member` | `SR_FIELD`, `SR_BITFIELD` |
| A struct size | `Class` | `SR_SIZE`, `SR_SIZE_CHECK` |
| A function parameter | `Class_Function::Param` | `SR_PARAM` |
| A function's parameter block size | `Class_Function` | `SR_PARAMS_SIZE` |

Class and function names are the Dumper-7 spelling. For `SR_PARAM`, leave off the class prefix
letter (`CrEnviroWaveSubsystem`, not `UCrEnviroWaveSubsystem`).

**Function, native function or call-site anchor.** Keep your pattern as the fallback. A site
(an address inside a function, or a call you follow with `PLUGIN_SCAN_FLAG_FOLLOW_REL32`) needs
its own pattern, and changing that pattern or its offsets starts it over with no rows.

```cpp
g_beginPlay = PluginMap::FunctionAs<void (*)(SDK::UWorld*)>(
    scanner, self, "UWorld::BeginPlay", "40 53 48 83 EC 20 ...");
g_anchor = PluginMap::Site(scanner, self, "ReadSettings", "E8 ?? ?? ?? ?? 48 8B D8 ...",
    PLUGIN_SCAN_IN_FUNCTION, PLUGIN_SCAN_FLAG_FOLLOW_REL32, 0, 1);
```

**Member offset.** Most members are reflected: the loader reads the offset from the running game.

```cpp
static auto g_claim = SR_FIELD(decltype(SDK::UCrUW_Analyzer::ClaimButton),
                               UCrUW_Analyzer, ClaimButton, 0);
// read:  if (auto* b = g_claim.In(widget)) ...
```

A native member (plain C++ type, not reflected) has no reflection to fall back on, so it needs a
row for every game build. If its size also changes between builds, use `SR_NATIVE_ANY_SIZE`
(type, class, member, compiled offset) and read `resolvedSize`. For a reflected member whose
size changes, use `SR_FIELD_ANY_SIZE`, and treat a size you do not handle as "not there".

**Struct size.** Pick by what the code does with the struct:
- It only steps through an array of it: `SR_SIZE(Struct)`, then `PluginMap::ElementAt`. The live
  size is used, so a resize costs nothing.
- It copies the struct, takes it from a return value, or hands a buffer of it to the game:
  `SR_SIZE_CHECK(Struct)`. The plugin (or its feature) goes off if the size changed.
- A struct that keeps changing when you only need a few members: copy those members into a
  struct your plugin owns, and read each from the game with `SR_FIELD`.

**Function parameters.** Do not use `sizeof(Params::X)` or `offsetof` on them. Ask for the size
and for each parameter you touch, then build the block:

```cpp
static auto g_size = SR_PARAMS_SIZE(CrEnviroWaveSubsystem, GetCurrentStageSettings);
static auto g_ret  = SR_PARAM(CrEnviroWaveSubsystem, GetCurrentStageSettings, ReturnValue);

auto block = PluginMap::ParamsBlockOf(g_size, { &g_ret });
if (!block) return;                       // did not resolve: leave the feature off
PluginMap::Call(wave, function, block);
const uint8_t* result = block.Param("ReturnValue");
```

This works for native functions and for Blueprint functions (class names ending `_C`; only
parameters count, not locals). A Blueprint class is not loaded when plugins start, so its rows
stay pending and the feature stays off until you call `PluginMap::ResolveLateFields` from the
game thread (for example once per tick, until it returns true). List the key in a declared
feature (section 5): a pending key that no feature lists refuses the plugin.

## 3. Get the CI row for each key

The pipeline writes a row per game build only for keys it knows about. For an outside plugin
that means a pull request; nothing is picked up automatically.

- **Function or global:** add your plugin's rows for the build you tested to the loader's
  `hooks/game/version_map.generated.h`, and add your plugin to the `plugins` input of
  starrupture-ci's `sr-client-check.yml`. CI then scans it on every new build.
- **Native member, or the size of a plain C++ struct:** add a line to starrupture-ci's
  `ci/native-keys.txt`, plus a row in `kFieldRows` for the build you tested.

```
MiniMap UTexture::SRGB optional
MyPlugin FMyStruct
```

A line is `<plugin> <Class::Member>`, or `<plugin> <ClassName>` for a struct size. After the key it
may give `<offset> <size> <mask>` in hex (only when the PDB's size is not the one your plugin was
built with), then `optional` if the plugin runs without that key. The header of the file has the
full format.

Reflected members, structs the game reflects, and parameters (`SR_PARAM`, `SR_PARAMS_SIZE`) are
read live from the game and need no line and no row.

For builds from before your key existed, CI has no row. A function key can still resolve (section
5); a native member cannot.

## 4. Read the value through the API

| Call | Gives you | Needs |
|---|---|---|
| `PluginMap::Function`, `FunctionAs`, `Global`, `Site` | an address | interface v72 |
| `SR_FIELD`, `SR_BITFIELD`, `SR_SIZE_CHECK` | an offset / a size check | v72 |
| `SR_SIZE` | the live struct size, in `resolvedSize` | v72 |
| `SR_FIELD_ANY_SIZE` | offset and live member size | v74 |
| `SR_NATIVE_ANY_SIZE` | the same for a native member | the first loader release after v1.23.1 |
| `SR_PARAMS_SIZE`, `SR_PARAM`, `PluginMap::ParamsBlockOf` | live parameter layout | the first loader release after v1.23.1 |
| the same on a Blueprint class | pending, then live | the first loader release after v1.23.1 (`PLUGIN_FIELD_LATE`) |
| `PluginMap::Feature`, `DeclareFeatures` | one feature off, not the plugin | v73 |

Interface v72 to v75 all ship in loader v1.23.0. An older loader refuses a plugin that needs a
newer interface. It ignores `PLUGIN_FIELD_LATE`, so a Blueprint class stays refused there.

Resolve everything once, in `OnPluginLoadHooks`:

```cpp
PluginMap::ResolveFields(scanner, self, g_claim, g_size, g_ret);
```

## 5. When a row is missing

The loader tries, in order:
- **Function:** the row for the running build decides on its own. With no such row, the loader
  rescans the pattern of the latest earlier build's row on this build. That result, your pattern
  and your own rows must all resolve to the same address; two different addresses fail the key.
- **Reflected member:** the row, then the game's own reflection.
- **Native member:** the row for the running build only. No row means refusal.

`ModLoader.log` has one `[Map]` line per key:

```
[Map] MyPlugin: UWorld::BeginPlay: loader map, CL-128786 row
[Map] MyPlugin: UCrCraftingComponent::FinishCrafting: no row for CL-129100; built pattern, CL-128786 row agree
[Map] MyPlugin: MyClass::MyMember: refused: the CL-128786 row says 0x6D8/0x8, the game says 0x6E0/0x8
```

A key that does not resolve refuses the whole plugin. To lose only one feature, declare it
first, before the resolves it covers:

```cpp
static PluginMap::Feature s_laser{ "mining-laser", "Mining laser tuning",
    { "ADroneLaser::Beam" }, { "Drone.LaserScale" } };
PluginMap::DeclareFeatures(scanner, self, s_laser);
// later:  if (s_laser.On()) { ... }
```

## 6. Declare `PLUGIN_MAP_MIGRATED`

Set it in `PluginInfo` once every game function, member and size your plugin uses comes from the
map. It promises the plugin reads no per-build game data from compiled-in values.

```cpp
static PluginInfo s_pluginInfo = { "MyPlugin", "1.0.0", "You", "What it does",
    PLUGIN_INTERFACE_VERSION, PLUGIN_TARGET_CLIENT,
    PLUGIN_GAME_BUILT_CL,       // the game build your Game-SDK was made for
    PLUGIN_MAP_MIGRATED };
```

Also call `PluginSdkRuntime::Init(self->hooks)` as the first statement of `PluginInit`, and use
`PluginSdkRuntime::GetWorld()` instead of `SDK::UWorld::GetWorld()`.

On a game build other than the one it was built for, the loader runs a plugin that has the flag.
Without it the plugin is listed as Incompatible and does not load there:

```
Needs an update to run on CL-xxxx: it does not declare PLUGIN_MAP_MIGRATED and names no game build, ...
```

If the plugin names its build but lacks the flag, the text says "it does not declare
PLUGIN_MAP_MIGRATED, so it would read this game's data where CL-yyyy had it".

## 7. Checklist

1. `PLUGIN_GAME_BUILT_CL` is set (`PluginGameBuild.props`).
2. Every pattern is a `PluginMap` call with a key.
3. Every game member you read is `SR_FIELD` / `SR_BITFIELD` (or an `ANY_SIZE` form).
4. Every struct you copy, return or buffer has `SR_SIZE_CHECK`; arrays are stepped with `SR_SIZE`.
5. No `sizeof(Params::...)` or `offsetof` on game data is left.
6. `ResolveFields` runs in `OnPluginLoadHooks`; optional parts are declared features.
7. `PluginSdkRuntime::Init` is first in `PluginInit`.
8. `node tools/rule3-check.mjs` is clean.
9. Check `ModLoader.log` on your own build and on another one.
10. Then set `PLUGIN_MAP_MIGRATED` and send the pull requests from step 3.

## 8. Worked example: ConfirmHotkey

ConfirmHotkey read two members of game widgets directly. One of them, before:

```cpp
if (widget->ClaimButton)
    widget->ClaimButton->ButtonClicked();
```

After:

```cpp
static auto s_claimButton = SR_FIELD(decltype(SDK::UCrUW_Analyzer::ClaimButton),
                                     UCrUW_Analyzer, ClaimButton, 0);

// OnPluginLoadHooks (a new export), once:
PluginMap::ResolveFields(scanner, self, s_claimButton);

// the read:
if (auto* claimButton = PluginMap::ValueOr(s_claimButton, widget))
    claimButton->ButtonClicked();
```

It also calls `PluginSdkRuntime::Init(self->hooks)` first in `PluginInit`, and lists
`OnPluginLoadHooks` in its `.def` file. The key `UCrUW_Analyzer::ClaimButton` is reflected, so it
needs no CI row.
