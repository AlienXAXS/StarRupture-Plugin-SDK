#pragma once

#include "plugin_interface.h"

namespace ExamplePluginConfig
{
	// Define config schema with a few simple examples
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the example plugin"
		},
		{
			"General",
			"ExampleString",
			ConfigValueType::String,
			"Hello World",
			"An example string value"
		},
		{
			"Settings",
			"ExampleNumber",
			ConfigValueType::Integer,
			"42",
			"An example integer value"
		},
		{
			"Settings",
			"ExampleFloat",
			ConfigValueType::Float,
			"3.14",
			"An example float value"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;

			// Initialize config from schema - creates file with defaults if missing
			if (s_self)
			{
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
			}
		}

		// Example accessors for each config value
		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		static const char* GetExampleString()
		{
			static char buffer[256];
			if (s_self && s_self->config->ReadString(s_self, "General", "ExampleString", buffer, sizeof(buffer), "Hello World"))
			{
				return buffer;
			}
			return "Hello World";
		}

		static int GetExampleNumber()
		{
			return s_self ? s_self->config->ReadInt(s_self, "Settings", "ExampleNumber", 42) : 42;
		}

		static float GetExampleFloat()
		{
			return s_self ? s_self->config->ReadFloat(s_self, "Settings", "ExampleFloat", 3.14f) : 3.14f;
		}

	private:
		static IPluginSelf* s_self;
	};
}
