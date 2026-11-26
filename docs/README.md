# Writing a StarRupture plugin

This folder is for people who write plugins for the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader), in their own repo or as a contribution. You do not need to fork or build the loader. This SDK is the starting point.

Read the pages in order. Each one is short and stands on its own.

1. [Build against the SDK](01-build-against-the-sdk.md): set up a plugin project, build it, load it in the game and check it loaded.
2. [Read game values from the version map](02-read-game-values-from-the-version-map.md): how a plugin gets function addresses, member offsets and struct sizes by name, so one DLL runs on every current game build. The version object map, or version map, is the loader's table of those values for each game build.
3. [Add a value to the version map](03-add-a-value-to-the-version-map.md): what to do when a value your plugin needs has no row yet, and how to send us the row.
4. [Get listed in Browse](04-get-listed-in-browse.md): what a Browse entry is, and how to send us yours so your plugin shows up in the loader's Browse tab.
5. [Publish on Nexus](05-publish-on-nexus.md): put a released version on Nexus Mods.
6. [Fix a plugin that will not load or a feature that is off](06-fix-a-plugin-that-will-not-load.md): read ModLoader.log, find the line for your plugin, and see what it means and what to do.
7. [Migrate an existing plugin](07-migrate-an-existing-plugin.md): move a released plugin to the current SDK and the version map without changing its own build, versioning or release setup.

The full API reference is [PluginDevelopment.md](../PluginDevelopment.md). It is long, so use it to look things up, not to start.

## Where to ask

- A question about the SDK, this guide or the loader: ask on [Discord](https://discord.gg/G5a4GYsvrz).
- A problem with the SDK or this guide (a bug, a missing value row): open an issue on the [SDK repository](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/issues). For value rows, [page 3](03-add-a-value-to-the-version-map.md) says what to send.
- A problem in the loader itself: tell us on [Discord](https://discord.gg/G5a4GYsvrz). The loader repository has issues turned off.
