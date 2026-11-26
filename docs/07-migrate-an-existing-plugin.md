# Migrate an existing plugin

You have a plugin that is already released. This page moves it to the current SDK and to the version object map (the version map, or v map, from here on), so one DLL loads on every current game build.

You keep your own build files, your own versioning, your own release workflow and your own Nexus description. Nothing here replaces them. You add a few lines to the code and one line to your `Shared.props`.

[Page 2](02-read-game-values-from-the-version-map.md) explains the calls and shows a one-member example. This page is the order to do it in on a plugin you already have, plus what page 2 skips.

## Steps

1. Update the SDK next to your plugin, as in [page 1](01-build-against-the-sdk.md) (pin it to a release tag, with its submodule). Keep your own `Shared.props`.
2. Include `plugin_map.h` after `plugin_interface.h` and the game headers, in each file that asks for a game value. Include `plugin_version_map.h` only if your plugin carries rows of its own (page 2).
3. In your `Shared.props`, import `PluginGameBuild.props`, after the lines that set `GameSDKRoot`, `StarRuptureSDKConfigDir` and `PluginSDKInclude`:

   ```xml
   <Import Project="$(PluginSDKInclude)\..\PluginGameBuild.props" Condition="Exists('$(PluginSDKInclude)\..\PluginGameBuild.props')" />
   ```

   It reads the game build from the headers you compile against and defines `PLUGIN_GAME_BUILT_CL`. If you kept a hand-written `PluginGameBuiltCL` line, delete it. The headers win, and a pin that differs prints warning `PLUGINCL004`. None of these warnings fails a build.
4. Add `OnPluginLoadHooks`, the only place a plugin asks for game values. If you list your exports in a `.def` file, add it there too.
5. Replace every hard-coded offset, struct size and pattern with a request by name, made in `OnPluginLoadHooks`:
   - a member: `SR_FIELD(Type, Class, Member, 0)`, then `PluginMap::ResolveFields(scanner, self, field, ...)`;
   - a function or global: `PluginMap::Function`, `FunctionAs` or `Global`. Your old pattern can stay as the fallback argument.
6. Replace every direct read with the helper: `field.In(object)`, or `PluginMap::ValueOr(field, object)`, which gives null when the member did not resolve. Run `tools/rule3-check.mjs` first (page 2) to find the reads you missed.
7. Make `PluginSdkRuntime::Init(self->hooks)` the first statement of `PluginInit`, before any early return.
8. Add `PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED` as the last two members of `PluginInfo`.
9. Declare your interface level. Set `PLUGIN_NEEDS_INTERFACE=<n>` in the preprocessor definitions of every configuration, and fill `interfaceVersion` with `PLUGIN_DECLARED_INTERFACE`. Raise `<n>` only when the compiler says a member you use needs a higher level. Use the same value in every file, or you get a link error (`LNK2038`). A newer member that you want only when the loader has it goes behind `PLUGIN_OPTIONAL_INTERFACE=<n>`, with `PLUGIN_EXPORT_HOST_INFO()` in one `.cpp` file (leaving it out is a link error). Ask for it through its `PluginOpt` accessor, which returns `nullptr` on a loader without it. If you use a `.def` file, check that the build still links with the macro in place. The table of the first loader release per level is in the SDK `README.md`.
10. If one part of your plugin can be off while the rest runs, declare a feature with at least one key, before the requests it covers:

    ```cpp
    static PluginMap::Feature s_x{ "id", "Title a player reads", { "Class::Member" }, { "Section.*" } };
    PluginMap::DeclareFeatures(scanner, self, s_x);
    ```

    Check `s_x.On()` where the feature acts. A key that is in no feature is core.
11. Build with your own build and your own version label, as before (`/p:ModLoaderBuildTag=<version>`). Install the DLL, press RELOAD or restart, and read `ModLoader\Logs\ModLoader.log`. Test on at least two game builds.

## What a missing row does

A key that does not resolve on the running build refuses the whole plugin: `PluginInit` is never called. If a declared feature lists the key, only that feature is off, the plugin loads, and the log names the miss. A reflected member comes from a row or, failing that, from the running game. A native member needs a row for the build that is running. Your code must never fall back to a compiled-in offset: `ValueOr` gives null instead.

If a key has no row, [page 3](03-add-a-value-to-the-version-map.md) says how to ask for one. [Page 6](06-fix-a-plugin-that-will-not-load.md) lists the log lines you can see and what each means.

## What you do not change

- Your `.sln` and `.vcxproj`, apart from the preprocessor definition in step 9 if you declare a level.
- Your versioning: your `version` file, your tags and your `ModLoaderBuildTag` label.
- Your release workflow and your release assets. If your workflow writes a minimum interface into an update manifest, check that it uses the level your plugin declares (`PLUGIN_NEEDS_INTERFACE`), not the SDK's newest.
- Your Nexus description.

## Worked example: ConfirmHotkey

[Page 2](02-read-game-values-from-the-version-map.md) shows the member read of ConfirmHotkey. This example covers what that page skips: the project side, the export, and the interface level. The code is on the `gss/plugin-work` branch of [msantoro12/StarRupture-Plugin-ConfirmHotkey](https://github.com/msantoro12/StarRupture-Plugin-ConfirmHotkey/tree/gss/plugin-work). ConfirmHotkey keeps its own `.sln`, `.vcxproj`, `version` file, release workflow and README. Commit `544da55` ("Read the two button members from the loader map") is the migration: `ConfirmHotkey.def`, `ModCore.cpp`, `ModCore.h`, `Shared.props` and `plugin.cpp`, 55 lines added and 6 removed. It adds no build file and no workflow.

The project side, in `Shared.props`, is the import from step 3. ConfirmHotkey also has an older hand-kept `PluginGameBuiltCL` line beside it. It is no longer needed; the headers decide.

The include, in `ModCore.cpp`, after the game headers:

```cpp
#include "plugin_map.h"
```

Two members are requested once and read through the helper. Before, a direct read:

```cpp
if (widget->ClaimButton)
    widget->ClaimButton->ButtonClicked();
```

After:

```cpp
if (auto* claimButton = PluginMap::ValueOr(s_claimButton, widget)) claimButton->ButtonClicked();
```

The new export, which the loader calls before `PluginInit`, in `plugin.cpp`. Because ConfirmHotkey lists its exports in a `.def`, `OnPluginLoadHooks` is added to `ConfirmHotkey.def` as well:

```cpp
void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner) { ModCore::ResolveFields(self, scanner); }
```

The SDK globals, first in `PluginInit`:

```cpp
PluginSdkRuntime::Init(self->hooks);
```

The `PluginInfo` tail changes in `544da55`:

```cpp
PLUGIN_INTERFACE_VERSION,
PLUGIN_TARGET_CLIENT,
PLUGIN_GAME_BUILT_CL,       // added
PLUGIN_MAP_MIGRATED         // added
```

Commit `7a377e0` then declares the interface level. In `Shared.props`, `PLUGIN_NEEDS_INTERFACE=73` joins the preprocessor definitions of the Client configurations, and `PluginInfo` uses `PLUGIN_DECLARED_INTERFACE` in place of `PLUGIN_INTERFACE_VERSION`. That is two files and two lines.

ConfirmHotkey has no patterns or functions, only member reads, and it declares no feature, so both keys stay core. A plugin with a part that can be off would add step 10.

## Checklist

1. `PluginGameBuild.props` is imported, and any hand-kept `PluginGameBuiltCL` line is gone.
2. No game offset, size or pattern is compiled in. The rule 3 check passes.
3. `OnPluginLoadHooks` asks for every key; `PluginSdkRuntime::Init` is first in `PluginInit`.
4. `PluginInfo` ends with `PLUGIN_GAME_BUILT_CL, PLUGIN_MAP_MIGRATED`, and uses `PLUGIN_DECLARED_INTERFACE`.
5. Your build files, versioning, release workflow and Nexus description are as they were.
6. The log shows your plugin as initialized on two game builds.
