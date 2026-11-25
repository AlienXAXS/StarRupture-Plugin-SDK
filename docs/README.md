# Writing a StarRupture plugin

This folder is for people who write plugins for the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader), in their own repo or as a contribution. You do not need to fork or build the loader. This SDK is the starting point.

Read the pages in order. Each one is short and stands on its own.

1. [Build against the SDK](01-build-against-the-sdk.md): set up a plugin project, build it, load it in the game and check it loaded.
2. [Read game values from the version map](02-read-game-values-from-the-version-map.md): how a plugin gets function addresses, member offsets and struct sizes by name, so one DLL runs on every current game build.
3. [Add a value to the version map](03-add-a-value-to-the-version-map.md): what to do when a value your plugin needs has no row yet, and how to send us the row.
4. [Get listed in Browse](04-get-listed-in-browse.md): what a Browse entry is, and how to send us yours so your plugin shows up in the loader's Browse tab.
5. [Publish on Nexus](05-publish-on-nexus.md): put a released version on Nexus Mods.

The full API reference is [PluginDevelopment.md](../PluginDevelopment.md). It is long, so use it to look things up, not to start.

## Where to ask

- A question about the SDK or this guide: open an issue on this repository.
- A value your plugin needs that the version map does not have: see [page 3](03-add-a-value-to-the-version-map.md), which says what to send.
- A problem in the loader itself: open an issue on the [ModLoader repository](https://github.com/AlienXAXS/StarRupture-ModLoader).
