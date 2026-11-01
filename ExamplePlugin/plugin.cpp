#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include <Engine_classes.hpp>
#include <plugin_map.h>

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"ExamplePlugin",
	MODLOADER_BUILD_TAG,
	"Your Name",
	"A minimal example plugin showing the basic structure required by the mod loader",
	PLUGIN_INTERFACE_VERSION
};

// A feature is a part of the plugin that can be switched off while the rest runs.
// Its title is what a player reads when the game updates and the key below no
// longer resolves, so write it in plain English. The "Settings" entries in
// plugin_config.h belong to it, so the config page greys them out while it is off.
static PluginMap::Feature s_example{ "example-feature", "Example feature",
	{ "UWorld::BeginPlay" }, { "Settings.*" } };

static uintptr_t g_beginPlay = 0;

extern "C" {

	// Optional export. Declare features before resolving the keys they list: a key
	// that does not resolve then turns its feature off instead of refusing the plugin.
	__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scan)
	{
		PluginMap::DeclareFeatures(scan, self, s_example);
		g_beginPlay = PluginMap::Function(scan, self, "UWorld::BeginPlay", nullptr);
	}

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		// Initialize config system (optional - creates config file with defaults)
		ExamplePluginConfig::Config::Initialize(self);

		// Check if plugin is enabled via config
		if (!ExamplePluginConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file");
			return true; // Return true so plugin loads but doesn't activate
		}

		// Example: Read and log config values
		LOG_INFO("Config values:");
		LOG_INFO("  ExampleString: %s", ExamplePluginConfig::Config::GetExampleString());
		LOG_INFO("  ExampleNumber: %d", ExamplePluginConfig::Config::GetExampleNumber());
		LOG_INFO("  ExampleFloat: %.2f", ExamplePluginConfig::Config::GetExampleFloat());

		if (!s_example.On())
		{
			LOG_WARN("Example feature is off on this game build");
		}

		// Your initialization code would go here
		// Examples:
		// - Find memory patterns with scanner
		// - Install hooks with hooks interface
		// - Register callbacks

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		// Your cleanup code would go here
		// Examples:
		// - Remove installed hooks
		// - Free allocated resources
		// - Unregister callbacks

		g_self = nullptr;
	}

} // extern "C"
