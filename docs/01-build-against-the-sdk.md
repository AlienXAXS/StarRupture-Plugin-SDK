# Build against the SDK

This page takes you from nothing to a plugin DLL that the loader accepts. You need Visual Studio 2022 with the C++ desktop workload, and Git.

## Steps

1. Download `dwmapi-Client.dll` (or `dwmapi-Server.dll`, for a server plugin) from the [latest release](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/releases/latest). It is the loader itself. Rename it `dwmapi.dll` and put it in the game's `Binaries\Win64\` folder, next to `StarRuptureGameSteam-Win64-Shipping.exe`. On Steam that is usually `steamapps\common\StarRupture\StarRupture\Binaries\Win64`.
2. Clone this SDK next to your plugin, with its submodule:

   ```
   AnyFolder\
     StarRupture-Plugin-SDK\      (git clone --recursive)
     StarRupture-Plugin-<Name>\
   ```

   The `StarRupture SDK` submodule holds the game's headers. `--recursive` brings it along.
3. Pin the SDK to a release tag, so your build does not change under you:

   ```
   git -C StarRupture-Plugin-SDK checkout v1.27.0
   git -C StarRupture-Plugin-SDK submodule update --init --recursive
   ```

   `main` moves only when the loader is released, so it is also safe. Do not use other branches: they can carry an interface version that no released loader accepts.
4. Point your project at the SDK. A plugin in its own repo finds the SDK as the sibling folder, so there is nothing to set, as long as your repo has a `Shared.props` with those defaults. Copy one from a plugin that has it, for example ConfirmHotkey's. To use another location, pass `/p:PluginSDKInclude=<path to include>` and `/p:GameSDKRoot=<path to StarRupture SDK>` to MSBuild.
5. In your plugin's `Shared.props`, import `PluginGameBuild.props` after those paths are set. ExamplePlugin already does this, because it uses this SDK's own `Shared.props`, so skip this step if you start from it inside the SDK solution. The file reads the game build of the headers you compile against (the `build_cl.txt` in the `StarRupture SDK` folder) and defines `PLUGIN_GAME_BUILT_CL`:

   ```xml
   <Import Project="$(PluginSDKInclude)\..\PluginGameBuild.props"
           Condition="Exists('$(PluginSDKInclude)\..\PluginGameBuild.props')" />
   ```

6. Build with the `Client Release` configuration, or `Server Release` for a dedicated server plugin. The other configurations are for debugging. Client builds define `MODLOADER_CLIENT_BUILD` and server builds define `MODLOADER_SERVER_BUILD`, so guard code that only exists on one side with `#if defined(...)`.
7. If your plugin shows its version from `MODLOADER_BUILD_TAG`, as ExamplePlugin does, pass your version on the command line:

   ```
   msbuild MyPlugin.sln /p:Configuration="Client Release" /p:Platform=x64 /p:ModLoaderBuildTag=1.0.0
   ```

   Without it the plugin reports `dev`. The loader's auto-updater uses this string to tell which build is installed, so a release build always sets it.

## The interface version

Every plugin declares an interface level in `PluginInfo`: fill `interfaceVersion` with `PLUGIN_DECLARED_INTERFACE`. That is `PLUGIN_NEEDS_INTERFACE`, the level your plugin needs; set it in the project's preprocessor definitions, and unset it is 75. Raise it only when the compiler says a member you use needs a higher level. On `main` the loader accepts levels 70 to 80 (`PLUGIN_INTERFACE_VERSION_MIN` and `_MAX` in `include/plugin_interface.h`).

- A plugin built against an older interface still loads. The interface only grows.
- A plugin that declares a version newer than the loader's maximum is refused, and the Plugins tab says it needs a newer ModLoader.
- If a plugin uses only calls from an older interface, its code can still run on an older loader. When it uses something newer, turn that feature off on an older loader, or have your message name the minimum ModLoader version.

## How the loader loads it

The loader finds the DLL in the game's `Binaries\Win64\ModLoader\Plugins` folder and calls its exports in this order:

1. `GetPluginInfo` returns the name, version, author, interface version and target (client or server).
2. `OnPluginLoadHooks` is optional. It is the only place a plugin asks for game addresses and offsets. Page 2 covers it.
3. `PluginInit` starts the plugin.
4. `PluginShutdown` stops it, when the game closes or the plugin is reloaded.

## Worked example: ExamplePlugin

`ExamplePlugin/` in this repo is the smallest plugin that loads. To start from it:

1. Copy the folder, rename it, and change the project name and GUID in `ExamplePlugin.vcxproj`.
2. Add the project to `StarRupture-Plugin-SDK.sln`, or to your own `.sln` that imports the same `Shared.props`.
3. Build `Client Release`. The DLL is written to `bin\x64\Client Release\plugins\`.
4. Copy the DLL to the game's `Binaries\Win64\ModLoader\Plugins\` folder.

ExamplePlugin runs only on the game build it was compiled for. Page 2 shows what to change so it runs on every current build.

## Test it

1. Start the game and open the loader's overlay. The Plugins tab lists your plugin as Loaded, or says why not (Incompatible, needs a newer ModLoader, a key that did not resolve).
2. Read `ModLoader\Logs\ModLoader.log` for the lines about your plugin.
3. After a rebuild, install the new DLL and press RELOAD in the overlay instead of restarting the game. A plugin can ask for a restart in its `PluginInfo`; then the overlay says so.

   RELOAD works cleanly only when your plugin's shutdown undoes everything it set up (hooks, UI registrations, timers) and the plugin keeps no UObject pointers: fetch objects again each tick, because objects die on travel and on garbage collection. RELOAD assumes that shutdown, callbacks, hooks and resource cleanup are all correct. If your plugin holds a lot of native or graphics (RHI) resources, restarting the game is the safer way to test. [PluginDevelopment.md](../PluginDevelopment.md) lists those settings.

Before you ask anyone for help, include that log.
