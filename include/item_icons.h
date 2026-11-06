#pragma once

// Item icons, loaded the first time something draws them.
//
// Include after plugin_interface.h and the Game-SDK headers. Client builds
// only: there is no texture interface on a server or generic build, and Init
// then returns false and everything below does nothing.
//
// An item icon is the UTexture2D in the item's ItemIcon brush. Copying it into
// the loader blocks the game thread for a few milliseconds, so this copies an
// icon only once a window draws it, a few per tick, instead of all of them at
// start-up. Until an icon is ready, Draw reserves its space.
//
//   static ItemIcons::Loader g_icons;
//
//   Init:      g_icons.Init(self, "MyPlugin", { &UniqueNameOf, &IconBrushOf });
//   Shutdown:  g_icons.Shutdown();
//   Tick:      g_icons.Tick();                          // each engine tick
//   Render:    g_icons.Draw(imgui, textures, uniqueItemName, 20.0f);
//
// The plugin reads the item's name and icon brush itself, through its own
// version-mapped fields, so nothing here depends on a game build.
//
// Threads: Tick, IconIs and the scan that Init posts run on the game thread.
// Init itself can run in PluginInit: it only sets fields and posts the scan.
// Get, Has and Draw can run on any thread, so the render callback is fine.
// Shutdown frees only handles and touches no game object.
//
// No game object is kept. The scan keeps strings. Each load finds the icon's
// texture again inside one Tick call and lets go of it when the call ends, so
// nothing goes stale across world travel or garbage collection.

#include "plugin_interface.h"
#include "icon_requests.h"

#include <AuItems_classes.hpp>
#include <Engine_classes.hpp>
#include <AssetRegistry_classes.hpp>
#include <AssetRegistry_parameters.hpp>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace ItemIcons
{
	static_assert(std::is_same_v<IconRequests::Handle, PluginTextureHandle>,
		"the request table stores plugin texture handles");

	// How the plugin reads an item. Both run on the game thread, on an item
	// definition the scan has just fetched.
	struct Accessors
	{
		// The item's unique name, or an empty string if it has none.
		std::string (*uniqueName)(SDK::UAuItemDataBase* item);
		// The item's ItemIcon brush, or null.
		const SDK::FSlateBrush* (*icon)(SDK::UAuItemDataBase* item);
	};

	class Loader
	{
	public:
		// Starts the item scan on the game thread and holds the splash screen
		// until it is done. tag prefixes the log lines. budgetMs is how long one
		// Tick may spend loading icons; it always loads at least one. False if
		// there is no texture interface (server or generic build).
		bool Init(IPluginSelf* self, const char* tag, Accessors accessors,
			double budgetMs = IconRequests::kDefaultBudgetMs)
		{
			IPluginHooks* hooks = self ? self->hooks : nullptr;
			if (!hooks || !hooks->Engine || !hooks->ImGuiTextures)
			{
				Log(self, tag, "texture hooks unavailable (server/generic build) - icons disabled.");
				return false;
			}

			m_self      = self;
			m_tag       = tag;
			m_accessors = accessors;
			m_budgetMs  = budgetMs;
			m_active    = true;

			IPluginSplash* splash = hooks->Splash;
			if (splash && splash->IsVisible())
			{
				splash->AcquireSplashHold();
				m_splashHeld = true;
			}

			RequestScan();
			return true;
		}

		void Shutdown()
		{
			m_active = false;

			IPluginHooks* hooks = m_self ? m_self->hooks : nullptr;
			for (PluginTextureHandle handle : m_table.Clear())
				if (hooks && hooks->ImGuiTextures)
					hooks->ImGuiTextures->FreeTexture(handle);

			ReleaseSplash();
			m_sources.clear();
			m_scanned = false;
			m_self    = nullptr;
		}

		// Loads the icons asked for since the last tick, within the budget.
		// Call once per engine tick, on the game thread.
		void Tick()
		{
			if (!m_active)
				return;

			// The first scan has not run (the post at Init never ran): ask again.
			if (!m_scanned)
			{
				RequestScan();
				return;
			}

			IconRequests::Serve(m_table,
				[this](const std::string& name) { return LoadIcon(name); },
				&NowMs, m_budgetMs,
				[this](const std::string& name) { LogF("giving up on the icon for '%s'.", name.c_str()); });
		}

		// The icon's handle, or null if the item has no icon or it has not loaded
		// yet. A null for an item that has an icon asks for it to be loaded.
		PluginTextureHandle Get(const std::string& uniqueItemName)
		{
			return m_table.Get(uniqueItemName);
		}

		// True if the item has an icon, loaded or still to load.
		bool Has(const std::string& uniqueItemName) const
		{
			return m_table.Has(uniqueItemName);
		}

		// Draws the item's icon at the cursor, size by size, and moves on by gap.
		// While the icon loads it leaves an empty square the same size, so the
		// text after it does not move when it arrives. An item with no icon takes
		// no room. Call from a render callback.
		void Draw(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
			const std::string& uniqueItemName, float size, float gap = 6.0f)
		{
			if (!imgui || !textures || !m_table.Has(uniqueItemName))
				return;

			if (PluginTextureHandle icon = m_table.Get(uniqueItemName))
				textures->Image(icon, size, size);
			else
				imgui->Dummy(size, size);
			imgui->SameLine(0.0f, gap);
		}

		// True if the item's icon is the texture with this identity (the
		// texture's UObject::GetFullName), loaded or not.
		bool IconIs(const std::string& uniqueItemName, const std::string& textureId) const
		{
			const auto it = m_sources.find(uniqueItemName);
			return it != m_sources.end() && it->second.textureId == textureId;
		}

	private:
		// What it takes to find an item's icon again: the item's Blueprint asset,
		// and the icon's identity. Plain strings, so nothing here can go stale
		// when the game frees objects.
		struct Source
		{
			std::string package;
			std::string asset;
			std::string textureId;
		};

		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		static double NowMs()
		{
			using Ms = std::chrono::duration<double, std::milli>;
			return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		static void Log(IPluginSelf* self, const char* tag, const char* format, ...)
		{
			if (!self || !self->logger)
				return;
			char text[512];
			va_list args;
			va_start(args, format);
			vsnprintf(text, sizeof(text), format, args);
			va_end(args);
			self->logger->Info(self, "%s: %s", tag ? tag : "ItemIcons", text);
		}

		template <class... Args>
		void LogF(const char* format, Args... args) const
		{
			Log(m_self, m_tag, format, args...);
		}

		void ReleaseSplash()
		{
			if (!m_splashHeld.exchange(false))
				return;
			IPluginSplash* splash = (m_self && m_self->hooks) ? m_self->hooks->Splash : nullptr;
			if (splash)
			{
				splash->ClearSubBar();
				splash->ReleaseSplashHold();
			}
		}

		// Posts the scan to the game thread. Safe to call every tick: only one
		// is in flight at a time.
		void RequestScan()
		{
			bool expected = false;
			if (!m_scanPosted.compare_exchange_strong(expected, true))
				return;
			m_self->hooks->Engine->PostToGameThread(&ScanJob, this);
		}

		static void ScanJob(void* context)
		{
			static_cast<Loader*>(context)->Scan();
		}

		// --- Finding items, game thread only ---

		SDK::UObject* GetAssetRegistry()
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo)
				return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!func)
			{
				LogF("could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		bool GetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject)
				return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LogF("could not resolve IAssetRegistry::GetAssetsByClass.");
				return false;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName     = classPath;
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			outAssets = std::move(parms.OutAssetData);
			return parms.ReturnValue;
		}

		// Every item type is defined as a Blueprint (a UAuItemBlueprint asset
		// whose GeneratedClass's CDO is the actual UAuItemDataBase), the way the
		// game's own UCrUW_CheatItemsTab::DebugGatherAllItems lists them.
		static SDK::FTopLevelAssetPath ItemBlueprintClassPath()
		{
			SDK::FString packagePath(L"/Script/AuItems");
			SDK::FString className(L"AuItemBlueprint");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		// The engine's package lookup and loading functions, as the loader
		// resolved them for the running build.
		bool ResolveEngineFunctions()
		{
			if (m_engineFnsTried)
				return m_staticFindObjectByName && m_findPackage && m_packageFullyLoad && m_loadPackage;

			m_engineFnsTried = true;

			IPluginEngineEvents* engine = m_self->hooks->Engine;
			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				m_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				m_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				m_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				m_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!m_staticFindObjectByName || !m_findPackage || !m_packageFullyLoad || !m_loadPackage)
			{
				LogF("failed to resolve object/package lookup functions - icon list will be incomplete.");
				return false;
			}
			return true;
		}

		// Loads the Blueprint's package and resolves it down to the item
		// definition that holds the icon: package -> generated "<AssetName>_C"
		// class -> ClassDefaultObject.
		SDK::UAuItemDataBase* ResolveItem(const std::string& assetName, const std::string& packageName)
		{
			if (packageName.empty() || !ResolveEngineFunctions())
				return nullptr;

			const std::wstring packageNameW(packageName.begin(), packageName.end());
			const std::wstring generatedClassNameW = std::wstring(assetName.begin(), assetName.end()) + L"_C";

			SDK::UPackage* package = m_findPackage(nullptr, packageNameW.c_str());
			if (package)
				m_packageFullyLoad(package);
			else
				package = m_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);
			if (!package)
				return nullptr;

			SDK::UObject* generatedObj = m_staticFindObjectByName(SDK::UClass::StaticClass(), package, generatedClassNameW.c_str(), false);
			if (!generatedObj)
				return nullptr;

			SDK::UObject* cdo = static_cast<SDK::UClass*>(generatedObj)->ClassDefaultObject;
			if (!cdo || !cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
				return nullptr;

			return static_cast<SDK::UAuItemDataBase*>(cdo);
		}

		// The item's icon if it is a UTexture2D. ResourceObject might be a
		// UTexture, a UMaterialInterface, etc., so the IsA check is the gate.
		// The pointer is good for the tick that fetched it and no longer.
		SDK::UTexture2D* IconTexture(SDK::UAuItemDataBase* item)
		{
			const SDK::FSlateBrush* brush = m_accessors.icon(item);
			SDK::UObject* resource = brush ? brush->ResourceObject : nullptr;
			if (!resource)
				return nullptr;

			try
			{
				return resource->IsA(SDK::UTexture2D::StaticClass()) ? static_cast<SDK::UTexture2D*>(resource) : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// Lists every item type through the asset registry and keeps, for each
		// one that has a texture icon, the strings that find it again. Copies no
		// texture. Runs on the game thread, posted by RequestScan.
		void Scan()
		{
			m_scanPosted = false;
			if (!m_active)
				return;

			IPluginSplash* splash = m_self->hooks->Splash;
			if (splash && splash->IsVisible())
			{
				char status[96];
				snprintf(status, sizeof(status), "%s: scanning item icons...", m_tag);
				splash->SetSubStatus(status);
				splash->SetSubProgress(0.0f);
			}

			LogF("scanning the asset registry for item icons...");

			std::unordered_map<std::string, Source> sources;
			try
			{
				SDK::UObject* registryObject = GetAssetRegistry();
				SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;

				if (!registry)
				{
					LogF("could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!GetAssetsByClass(registry, ItemBlueprintClassPath(), assetData))
						LogF("IAssetRegistry::GetAssetsByClass failed.");

					const int rawCount = assetData.Num();
					for (int i = 0; i < rawCount; ++i)
					{
						Source source;
						source.asset   = assetData[i].AssetName.ToString();
						source.package = assetData[i].PackageName.GetRawString();

						SDK::UAuItemDataBase* item = ResolveItem(source.asset, source.package);
						if (!item)
							continue;

						const std::string uniqueName = m_accessors.uniqueName(item);
						if (uniqueName.empty())
							continue;

						SDK::UTexture2D* icon = IconTexture(item);
						if (!icon)
							continue;

						source.textureId = icon->GetFullName();
						sources.emplace(uniqueName, std::move(source));
					}

					LogF("resolved %d item icon(s) from %d Blueprint asset(s).",
						static_cast<int>(sources.size()), rawCount);
				}
			}
			catch (...)
			{
				LogF("exception while resolving item icons via the asset registry.");
			}

			std::unordered_set<std::string> names;
			for (const auto& [name, source] : sources)
				names.insert(name);

			m_sources = std::move(sources);
			m_table.SetKnown(std::move(names));
			m_scanned = true;
			ReleaseSplash();
		}

		// --- Loading, game thread only ---

		// Finds the item's texture again and copies it into the loader. Returns
		// null while the texture is not ready (D3D12 not up, texture still
		// streaming, an object already dying), which Serve retries on a later
		// tick. When the loader has no free texture slot it throws
		// std::out_of_range, and that icon is given up on at once: a slot does
		// not free up by waiting. A texture another plugin already holds is found
		// by name before the loader looks for a slot, so it still loads from a
		// full table. The texture pointer lives only inside this call.
		PluginTextureHandle LoadIcon(const std::string& uniqueItemName)
		{
			IPluginHooks* hooks = m_self ? m_self->hooks : nullptr;
			const auto source = m_sources.find(uniqueItemName);
			if (!hooks || !hooks->ImGuiTextures || source == m_sources.end())
				return nullptr;

			try
			{
				SDK::UAuItemDataBase* item = ResolveItem(source->second.asset, source->second.package);
				if (!item || !SDK::UKismetSystemLibrary::IsValid(item))
					return nullptr;
				SDK::UTexture2D* icon = IconTexture(item);
				if (!icon || !SDK::UKismetSystemLibrary::IsValid(icon))
					return nullptr;

				const std::string texName = icon->GetName();
				return hooks->ImGuiTextures->LoadFromUTexture2D(icon, texName.c_str());
			}
			catch (const std::out_of_range&)
			{
				if (m_table.GiveUp(uniqueItemName))
					LogF("giving up on the icon for '%s': the loader has no free texture slot.", uniqueItemName.c_str());
				return nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		IconRequests::Table m_table;

		// Set by Init and cleared by Shutdown. Only the scan and Tick use them,
		// both on the game thread.
		IPluginSelf* m_self       = nullptr;
		const char*  m_tag        = "ItemIcons";
		Accessors    m_accessors{};
		double       m_budgetMs   = IconRequests::kDefaultBudgetMs;

		std::atomic<bool> m_active{ false };
		std::atomic<bool> m_scanPosted{ false };
		std::atomic<bool> m_splashHeld{ false };

		// Game thread only.
		bool                                    m_scanned = false;
		std::unordered_map<std::string, Source> m_sources;

		bool                     m_engineFnsTried         = false;
		StaticFindObjectByNameFn m_staticFindObjectByName = nullptr;
		FindPackageFn            m_findPackage            = nullptr;
		PackageFullyLoadFn       m_packageFullyLoad       = nullptr;
		LoadPackageFn            m_loadPackage            = nullptr;
	};
}
