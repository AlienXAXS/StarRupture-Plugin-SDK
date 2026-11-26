# Fix a plugin that will not load or a feature that is off

This page is for when your plugin does not appear as Loaded in the Plugins tab, or loads with part of it switched off. The loader writes down why. You find the line, read what it means, and do what it says.

Before you paste log lines anywhere to ask for help, paste only the lines about your plugin and its features. The full log can also hold a server's startup details and login details, which can contain passwords. Take those out first.

## Find your plugin's lines

The log is `ModLoader\Logs\ModLoader.log`, in the `Binaries\Win64` folder next to the game's exe. Older logs are kept beside it as `ModLoader-<date>_<time>.log`. Each line looks like this:

```
[HH:MM:SS.mmm] [LEVEL] [T:<thread id>] <text>
```

Search the file for your plugin's name. A plugin's session reads in this order:

| Line | What it is |
|---|---|
| `[ModLoader] Loading plugin: <file>.dll` | The loader starts reading your DLL. |
| `[ModLoader] Plugin info - Name: <n>, Version: <v>, Author: <a>, Interface: <N> (modloader expects [<min>, <max>]), game build <CL-x>` | Your plugin passed the version checks. It shows your interface level and the game build you built for. |
| `[ModLoader] [HookScan] Resolving patterns for: <plugin> v<ver>` | Your `OnPluginLoadHooks` runs now. The `[Map]` and `[HookScan]` lines for your plugin come after this one. |
| `[ModLoader] [HookScan] <plugin>: <N> pattern(s) resolved, no failures` | The clean case. It counts patterns and function keys, not member lookups. |
| `[ModLoader] Calling PluginInit for: <plugin> v<ver>`, then `Plugin initialized: <plugin>` | Your plugin is running. |

Lines tagged `[Map]` are about the version object map (the version map, or v map, from here on): one line per key that needed attention, and one per feature that is off. A key that resolves cleanly adds no line of its own.

The same text shows in the in-game window that lists failures, per plugin, as "loaded" or "NOT LOADED" with the number of features off.

## A key has no row for this game build

The loader never falls back to an offset compiled into your plugin for a member it cannot place. A key resolves from a row or from the running game, or it is refused. A refused key turns off the feature that lists it. If no feature lists it, the plugin is not loaded (see "The plugin is not loaded").

| Line | What it means | What to do |
|---|---|---|
| `[Map] <plugin>: <Class::Member>: refused: no row for CL-<build>, and a native member has no other source` | The member is native (the game does not describe it) and there is no row for this build. Nothing is read from a compiled offset. | Ask for a row; see below. |
| `[Map] <plugin>: <Class::Member>: refused: no row for CL-<build>, and the layout check found <Class> or a class it derives from changed on this build, so its built offset is not used` | The same, and the game changed that class on this build, so your compiled offset is not trusted either. | Ask for a row. |
| `[Map] <plugin>: <Class::Member>: refused: <Class> is not reflected, so its size needs a row for CL-<build>` | The game does not describe that class, so only a row can give its size or offset. | Ask for a row. |
| `[Map] <plugin>: <key>: refused: the code relies on <x>, this build has <y>` | The member exists but is a different size or layout from what you compiled for. | Handle both sizes by build (page 2, "Choosing a size or layout by build"), or ask for a row. |
| `[Map] <plugin>: <Class::Member>: reflected (no row for CL-<build>) (<offset>/<size>)` | Not a problem. The loader read the member from the running game and needed no row. | Nothing. |
| `[Map] <plugin>: <key>: <Class::Member>: not in this game version (CL-<build>)` | The version map says this build does not have the member. A feature that needs it is off. | Nothing, unless you expected the member. |
| `[Map] <plugin>: <key>: pending: <why>` | A class you need is not loaded yet. The loader tries again later. | Nothing. |

Three ways to fix a missing row:

1. Ask for the row. Open an issue on the [SDK repository](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/issues) with the key and what you know. [Page 3](03-add-a-value-to-the-version-map.md) says what to send. Once the row is published you need no new plugin release. Players restart the game to pick up the new version map.
2. Use plugin-owned rows while you wait. [Page 2](02-read-game-values-from-the-version-map.md) shows `plugin_version_map.h`, which builds rows into your plugin. Move them to the shared version map before you release.
3. Decide whether the value is a feature or a requirement. If the plugin works without it, group its keys in a feature (see "A feature is off"). The plugin then loads and only that part stays off. If it cannot work without it, leave the key as core: the plugin is refused on a build with no row, and says so in the log.

## A member, function or pattern is not found

| Line | What it means | What to do |
|---|---|---|
| `[Map] <plugin>: <Class::Member>: refused: reflection: <detail>` | The game's own description has no such class or member. The detail after `reflection:` says what was missing. | The member may have been renamed or removed on this build. Check the key. Put it in a feature so a miss only turns that feature off. |
| `[Map] <plugin>: <key>: refused: no reflection data to read a reflected field from` | The game's description was not ready yet, because the request ran too early. | Ask with the late flag so the loader retries. |
| `[Map] <plugin>: <key>: refused: asked for as a bitfield, but the game's member is not a bool` | Your request type does not match the member. | Fix the request. |
| `[Map] <plugin>: <key>: refused: the game's member is a bitfield bool; ask for it with PLUGIN_FIELD_BITFIELD` | Same. | Use `SR_BITFIELD` (page 2). |
| `[Map] <plugin>: <key>: its own pattern matches 0 times on this exe (stale for this build?); a row decides this key and is carrying it, but a loader without that row would not find the function. Update the pattern.` | Harmless today: a row finds the function. Your own pattern no longer matches, so a loader without that row would fail. | Update the pattern. |
| `[Map] <plugin>: <key>: the CL-<n> row's pattern is not in this exe; trying every other locator` | One row missed. The loader tries the other rows and your own pattern. | If the function is still found, nothing. Tell us the row is stale. |
| `[HookScan]   [required]` or `[HookScan]   [optional]` then `<key>`, with a line such as `pattern not found in <module>: <pattern>` | A function or pattern could not be found and no feature covers it. The plugin is not loaded. | Give it a new pattern or a row for this build, or put it in a feature. |

## A feature is off

A feature is a group of keys you declared, so one part of your plugin can be off while the rest runs.

| Line | What it means | What to do |
|---|---|---|
| `[Map] <plugin>: feature "<title>" is off: <key1, key2> (<reason>)` | That part is off. The missed keys are listed. The reason is one of: `the plugin reported a problem with something it needs`, `the plugin asked for something it needs in a way the loader could not use`, `this game build changed something it needs`. | Fix or add rows for the listed keys. The lines just above name each miss. |
| `[Map] <plugin>: feature "<title>" is waiting for a game class that is not loaded yet: <keys>` | Not off yet. It turns on when the class loads. | Nothing. |
| `[Map] <plugin>: feature "<title>" is not in this game version: <keys>` | This build does not have what the feature needs. | Nothing. It is intended. |

## DeclareFeature does not work

`DeclareFeature` (or `PluginMap::DeclareFeatures`) writes a log line for two reasons:

| Line | What it means | What to do |
|---|---|---|
| `[Map] <plugin>: DeclareFeature(<id>) refused: bad description` | The id or title is empty, there is no key or more than 256, or the config keys have no list. The id shows as `(no id)` when it is missing. The feature is not declared, so its keys stay core. | Give an id, a title and at least one key. |
| `[Map] <plugin>: DeclareFeature(<id>) refused: an empty key` | One entry in the keys list is null or empty. | Remove it. |
| `[HookScan] Plugin '<name>' called DeclareFeature outside OnPluginLoadHooks -- rejected. Pattern scanning is only available inside that event; resolve there and install from PluginInit.` | You called it from `PluginInit` or later. | Declare and resolve inside `OnPluginLoadHooks`. |

Some failures return false with no log line at all, so check the return value:

- the description pointer is null, or its `structSize` is too small;
- it was called outside `OnPluginLoadHooks`;
- the same `id` was declared twice in one load.

## The plugin is not loaded

| Line | What it means | What to do |
|---|---|---|
| `[HookScan] '<name>' will NOT be loaded -- PluginInit will not be called.`, then `Plugin '<name>' was NOT loaded: one or more of its hooks could not be resolved.` | A key that no feature lists was missed. Even a missed optional key refuses the plugin. The `[HookScan]   [required]` or `[optional]` lines above it name the key. | Add a row, give a pattern, or put the key in a feature. |
| `Plugin '<name>' is not loaded: Needs an update to run on CL-<running>: it does not declare PLUGIN_MAP_MIGRATED, so it would read this game's data where CL-<built> had it. It loads on CL-<built>.` | The plugin was built for another game build and does not read from the version map, so it runs only on its own build. | Move it to the version map and declare `PLUGIN_MAP_MIGRATED` (page 2, and [page 7](07-migrate-an-existing-plugin.md)). |
| `... its PluginInfo names no game build (gameBuiltCL is 0), so the loader cannot tell which build its SDK came from.` | `PLUGIN_MAP_MIGRATED` is set but the game build is 0. | Import `PluginGameBuild.props` (page 1, step 5). |
| `Plugin '<name>' is not loaded: Needs an update: this game build changed the layout of <A>[, <B> and <n> more], and it does not declare PLUGIN_MAP_MIGRATED, so it would read their old layouts.` | The game changed those classes on this build. | Same as the line above. |
| `Plugin '<name>' declares no game build; judging it as built on CL-<n>.` | No `gameBuiltCL` on interface 72 or higher. The plugin is judged as the oldest build. | Set the game build in `PluginInfo`. |
| `Plugin '<name>' was NOT loaded: <the same refusal text as above>` (after `PluginInit` ran) | The same refusal, found later: the plugin used its own SDK offsets without taking the SDK globals from the loader. The plugin is shut down again. | Call `PluginSdkRuntime::Init(self->hooks)` first in `PluginInit` (page 2). |

## Version problems

| Line | What it means | What to do |
|---|---|---|
| `Plugin interface version <N> is newer than this loader supports (up to <max>): <file>.dll`, then `[AutoUpdate] <plugin> (interface <N>): ...` naming the ModLoader release needed | Your plugin declares a level above the loader's. It is not loaded. The line is logged at INFO. | Players update ModLoader. As the author, set `PLUGIN_NEEDS_INTERFACE` to the level you use, and put newer features behind `PLUGIN_OPTIONAL_INTERFACE` (page 7). |
| `Plugin missing required exports: <file>.dll` | The DLL is not a loader plugin. | Check that `GetPluginInfo` is exported. |
| `Failed to load plugin DLL: <file>.dll (error: <code>)` | Windows could not load it, often a missing dependency DLL. | Look up the Windows error code. |
| `Plugin GetPluginInfo returned nullptr: <file>.dll` | `GetPluginInfo` returned nothing. | Return your `PluginInfo`. |

There is no line for an optional newer interface that the loader lacks. Nothing refuses your plugin, so you see it only from your own code. Ask for a newer member through its `PluginOpt` accessor: it returns `nullptr` on a loader without it. Check for that, turn the feature off, and show a message that names the minimum ModLoader version.

## Reload problems

RELOAD works only when your plugin's shutdown undoes everything it did; see the warning at the end of [page 1](01-build-against-the-sdk.md#test-it).

| Line | What it means | What to do |
|---|---|---|
| `Unload of <plugin> refused: Unload not supported: this plugin requires a game restart` | The plugin's load setting is "restart required". RELOAD does nothing. | Restart the game. |
| `Unload of <plugin> refused: Return to the main menu to reload this plugin` (also `Reload of`) | The plugin reloads only at the main menu, and a world is running. | Go to the main menu, then RELOAD. |
| `Unload of <plugin> refused: Unload blocked: <reason>` | The plugin says it is busy, for example saving. | Wait and retry. As the author, `SetBusyReason` clears it. |
| `Unload of <plugin> refused: This plugin is being unloaded right now.` | A second click while the first runs. | Wait. |
| `Plugin reload failed for index <n>` or `Plugin reloaded but PluginInit failed: <plugin>` | The new DLL did not load, or its init returned false. | Read the lines just above it. |
| `Unload of <plugin> skipped: the overlay is still drawing its panel. It is shut down but stays loaded; reload it to try again.` | A panel of yours was still in use, so the DLL was not freed. | RELOAD again. As the author, unregister panels and widgets first in `PluginShutdown`. |
| `<plugin> is still inside its PluginShutdown after 5 s; continuing the shutdown without it` | Your shutdown hangs. | Shutdown must not wait on the render thread. |
| `[PluginManager] Plugin '<name>' CRASHED in its <callback> -- code=0x<c> ...`, then `[PluginManager] '<name>' is switched off: the loader calls none of its callbacks until it is unloaded or reloaded. Detours it placed on game functions keep running.` | After a crash the plugin is muted, but the hooks it placed on game functions stay until you reload. | RELOAD or restart. The crash line has the fault offset in your DLL. |

## A harmless line

`[Map] <plugin>: feature "<title>" lists "<key>", which no request asked for`

You listed a key in a feature but never asked the loader for it in that load. It usually means a typo in the key list, or that your plugin skipped that lookup on purpose on this game build. Nothing is turned off and the plugin is not refused. Fix the typo, or drop the key from the feature if you skip it on purpose.
