#pragma once
// MapOverlay: turn any world point into a pixel on the game's open full map,
// so a plugin can draw its own marks over it. Factored out of BetterDrone's
// drone marker: the same reads, the same checks, the same fail-closed rules.
// Header-only. The plugin keeps its own widget (a transparent overlay window
// of the loader's), tick and snapshot mutex.
//
// How the pixel is found: the map's terrain widget lays out 100 world units
// per widget unit and its top-left corner is the dev settings' pivot point, so
// the widget's geometry on screen gives the world-to-pixel transform at any
// zoom and DPI scale. Each Sample checks that transform against up to three
// building markers the game itself shows on the same map; when none of the
// ones it looked at sits where the transform puts it, nothing is drawn.
//
// Version map keys: the rows in PluginMapOverlay::Fields, already served by
// the loader's map (BetterDrone resolves the same ones). In OnPluginLoadHooks:
//   PluginMapOverlay::Resolve(scanner, self);            // false = a key missed
//   static PluginMap::Feature s_map{ "map-marks", "Marks on the map",
//       PluginMapOverlay::KeysWith(/* your own keys */), {} };
// In PluginInit / PluginShutdown: PluginMapOverlay::Init(self) / Shutdown(self).
// That registers the one UI state callback the map-open gate needs.
//
// Threads:
//   Sample runs on the game thread and is the only place a UObject is touched.
//   It writes a plain-data Snapshot; the plugin hands it to its render
//   callback under its own lock. ToPixel, PushClip and PopClip are arithmetic
//   and ImGui calls only. Nothing is kept past the tick: the map, its widgets
//   and the replication helper are looked up again each Sample.
//
// A fault while reading: Sample itself does not catch an access violation, so
// a plugin may wrap its whole tick in its own __try as BetterDrone does.
// SampleGuarded catches it for a plugin that has no such guard; on false, hide
// the overlay and stop sampling until RELOAD.

#include "plugin_interface.h"
#include "plugin_map.h"

#include <Chimera_classes.hpp>
#include <ChimeraUI_classes.hpp>
#include <CommonGame_classes.hpp>
#include <Engine_classes.hpp>
#include <UMG_classes.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

#define PLUGIN_MAP_OVERLAY_FIELD(Class, Member) \
	PluginField<decltype(SDK::Class::Member)>{ { #Class "::" #Member, #Class, #Member, 0, \
	                                             sizeof(decltype(SDK::Class::Member)), \
	                                             static_cast<int32_t>(offsetof(SDK::Class, Member)) } }
#define PLUGIN_MAP_OVERLAY_FIELD_ANY_SIZE(Class, Member) \
	PluginField<decltype(SDK::Class::Member)>{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_ANY_SIZE, 0, \
	                                             static_cast<int32_t>(offsetof(SDK::Class, Member)) } }

namespace PluginMapOverlay
{
	// The map's own terrain widget lays out 6000 widget units for 600000 world
	// units, so one widget unit is 100 world units. With the widget's size on
	// screen, that gives pixels per world unit at any zoom and DPI scale.
	constexpr double kWorldPerLocalUnit = 100.0;

	// The placement is checked against the game's own building markers. One that
	// sits further than this from where the transform puts it means the
	// transform is wrong, and nothing is drawn.
	constexpr double kTolerancePx = 2.0;

	// A Sample checks up to this many markers, looking at no more than kMaxScan
	// map widgets to find them, and is ready if at least one agrees. A merged
	// cluster marker sits at its cluster's centre and would disagree on its own.
	constexpr int kMaxReferences = 3;
	constexpr int kMaxScan       = 40;

	// Guard against reading a torn array header as a huge count.
	constexpr int32_t kMaxSaneCount = 100000;

	// ---- version map rows --------------------------------------------------------

	namespace Fields
	{
		inline auto MapDataHelper        = PLUGIN_MAP_OVERLAY_FIELD(ACrGameStateBase, MapMenuDataReplicationHelper);
		inline auto BuildingsMarkers     = PLUGIN_MAP_OVERLAY_FIELD_ANY_SIZE(ACrMapMenuDataReplicationHelper, BuildingsMarkerDataContainer);
		inline auto BuildingsMarkerData  = PLUGIN_MAP_OVERLAY_FIELD(FBuildingsMarkerDataContainer, BuildingsMarkerData);
		inline auto MarkerBuildingHandle = PLUGIN_MAP_OVERLAY_FIELD(FBuildingMarkerDataFastArrayItem, BuildingHandle);
		inline auto MarkerLocation       = PLUGIN_MAP_OVERLAY_FIELD(FBuildingMarkerDataFastArrayItem, Location);
		inline auto MapMenuMapArea       = PLUGIN_MAP_OVERLAY_FIELD(UCrUW_MapMenu, MapMenuMapArea);
		inline auto MapMassMarkers       = PLUGIN_MAP_OVERLAY_FIELD_ANY_SIZE(UCrUW_MapMenuMapArea, MapMenuMassMarkers);
		inline auto MapTerrain           = PLUGIN_MAP_OVERLAY_FIELD(UCrUW_MapMenuMapArea, MapMenuTerrain);
		inline auto MapCanvasPanel       = PLUGIN_MAP_OVERLAY_FIELD(UCrUW_MapMenuMapArea, CanvasPanelMapArea);
		inline auto MarkerWidget         = PLUGIN_MAP_OVERLAY_FIELD(FEntityMarkerUIRepresentation, Marker);
		inline auto MapPivotPoint        = PLUGIN_MAP_OVERLAY_FIELD(UCrMapMenuDevSettings, MapAreaPivotPoint);

		// Structs stepped through: the size the running game reflects. The marker
		// array is read with PluginMap::ElementAt, the widget map with
		// PluginMap::ScriptMapView at a stride computed from the second size.
		inline auto MarkerItemSize           = SR_SIZE(FBuildingMarkerDataFastArrayItem);
		inline auto MarkerRepresentationSize = SR_SIZE(FEntityMarkerUIRepresentation);
	}

	// The keys of the rows above, then `extra`: a field or size row, or a key
	// string, as PluginMap::KeysOf takes. Use it for the keys of a feature that
	// draws on the map, so a build without them turns off that feature only.
	template <class... Extra>
	std::vector<const char*> KeysWith(const Extra&... extra)
	{
		return PluginMap::KeysOf(Fields::MapDataHelper, Fields::BuildingsMarkers, Fields::BuildingsMarkerData,
			Fields::MarkerBuildingHandle, Fields::MarkerLocation, Fields::MapMenuMapArea, Fields::MapMassMarkers,
			Fields::MapTerrain, Fields::MapCanvasPanel, Fields::MarkerWidget, Fields::MapPivotPoint,
			Fields::MarkerItemSize, Fields::MarkerRepresentationSize, extra...);
	}

	// Asks the loader for every row above. Call in OnPluginLoadHooks. False
	// when any did not resolve; the loader has then recorded the failure.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		return PluginMap::ResolveFields(scanner, self,
			Fields::MapDataHelper, Fields::BuildingsMarkers, Fields::BuildingsMarkerData, Fields::MarkerBuildingHandle,
			Fields::MarkerLocation, Fields::MapMenuMapArea, Fields::MapMassMarkers, Fields::MapTerrain,
			Fields::MapCanvasPanel, Fields::MarkerWidget, Fields::MapPivotPoint, Fields::MarkerItemSize,
			Fields::MarkerRepresentationSize);
	}

	// ---- the menu layer and the map-open gate ----------------------------------

	// The layer UCrUIManagerSubsystem opens the map, the inventory and every
	// building window on, and the one it opens the death screen on. The HUD
	// layout pushes its escape menu there too. The HUD itself sits on another
	// layer, so an empty stack here means no menu is up. Walked only to get at
	// a widget that is about to be acted on.
	constexpr const char* kMenuLayerTag = "UI.Layer.Menu";

	// The layer the building menu opens on in newer game builds.
	constexpr const char* kGameMenuLayerTag = "UI.Layer.GameMenu";

	// Game thread. The local player's controller, or null outside a game. Valid
	// for the current tick only.
	inline SDK::ACrPlayerControllerBase* LocalController()
	{
		SDK::UWorld* world = PluginSdkRuntime::GetWorld();
		if (!world)
			return nullptr;

		SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
		if (!pc || !pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
			return nullptr;

		return static_cast<SDK::ACrPlayerControllerBase*>(pc);
	}

	// The local player's layer with this tag, or null. Valid for the current
	// tick only.
	inline SDK::UCommonActivatableWidgetContainerBase* FindLayer(SDK::ACrPlayerControllerBase* pc, const char* tag)
	{
		auto* ui = static_cast<SDK::UGameUIManagerSubsystem*>(
			SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(pc, SDK::UCrUIManagerSubsystem::StaticClass()));
		SDK::UGameUIPolicy* policy = ui ? ui->CurrentPolicy : nullptr;
		if (!policy)
			return nullptr;

		for (int32_t i = 0; i < policy->RootViewportLayouts.Num(); ++i)
		{
			const SDK::FRootViewportLayoutInfo& info = policy->RootViewportLayouts[i];
			if (!info.RootLayout || info.LocalPlayer != pc->Player)
				continue;

			// Not const: the SDK's const TMap indexer does not compile.
			auto& layers = info.RootLayout->Layers;
			for (int32_t l = 0; l < layers.NumAllocated(); ++l)
			{
				if (!layers.IsValidIndex(l))
					continue;

				SDK::UCommonActivatableWidgetContainerBase* stack = layers[l].Value();
				if (stack && layers[l].Key().TagName.ToString() == tag)
					return stack;
			}
		}

		return nullptr;
	}

	// The local player's menu layer, or null. Valid for the current tick only.
	inline SDK::UCommonActivatableWidgetContainerBase* FindMenuLayer(SDK::ACrPlayerControllerBase* pc)
	{
		return FindLayer(pc, kMenuLayerTag);
	}

	// The menu of type T, if it is the active widget on the layer and not
	// already closing. DisplayedWidget is what the stack's GetActiveWidget
	// returns.
	template <class T>
	T* ActiveMenu(SDK::UCommonActivatableWidgetContainerBase* layer)
	{
		SDK::UCommonActivatableWidget* top = layer->DisplayedWidget;
		if (!top || !top->bIsActive || !top->IsA(T::StaticClass()))
			return nullptr;

		return static_cast<T*>(top);
	}

	inline SDK::UCrUW_MapMenu* ActiveMap(SDK::UCommonActivatableWidgetContainerBase* layer)
	{
		return ActiveMenu<SDK::UCrUW_MapMenu>(layer);
	}

	// The loader's UI state, as the callback below last stored it. Any thread.
	inline std::atomic<uint64_t> g_uiState{ 0 };

	inline void OnUIState(uint64_t, uint64_t newState, void*)
	{
		g_uiState.store(newState, std::memory_order_relaxed);
	}

	// Registers the callback that tells whether the map is on the menu layer.
	// Call from PluginInit. The loader's tick runs before the plugin's, so the
	// state a tick reads is the state of that frame.
	inline void Init(IPluginSelf* self)
	{
		if (self && self->hooks && self->hooks->GameMenu)
			self->hooks->GameMenu->RegisterOnUIStateChanged(PLUGIN_UI_MAP, &OnUIState, nullptr);
	}

	// Call from PluginShutdown, after the last Sample or ActiveLocalMap.
	inline void Shutdown(IPluginSelf* self)
	{
		if (self && self->hooks && self->hooks->GameMenu)
			self->hooks->GameMenu->UnregisterOnUIStateChanged(&OnUIState, nullptr);

		g_uiState.store(0, std::memory_order_relaxed);
	}

	// Game thread only. The map, if it is the active widget on the local
	// player's menu layer. Valid for the current tick only.
	inline SDK::UCrUW_MapMenu* ActiveLocalMap()
	{
		// No map on the menu layer, no walk of it.
		if (!(g_uiState.load(std::memory_order_relaxed) & PLUGIN_UI_MAP))
			return nullptr;

		SDK::ACrPlayerControllerBase* pc = LocalController();
		SDK::UCommonActivatableWidgetContainerBase* layer = pc ? FindMenuLayer(pc) : nullptr;
		return layer ? ActiveMap(layer) : nullptr;
	}

	// ---- reading the map's geometry --------------------------------------------

	struct Geo
	{
		bool   visible;
		double topLeft[2];   // viewport pixels
		double absSize[2];
		double localSize[2];
	};

	// Game thread. False when the widget could not be read.
	inline bool SampleGeo(SDK::UWidget* w, SDK::UObject* worldContext, Geo* out)
	{
		__try
		{
			memset(out, 0, sizeof(*out));
			out->visible = w->IsVisible();

			const SDK::FGeometry geo       = w->GetCachedGeometry();
			const SDK::FVector2D localSize = SDK::USlateBlueprintLibrary::GetLocalSize(geo);
			const SDK::FVector2D absSize   = SDK::USlateBlueprintLibrary::GetAbsoluteSize(geo);
			out->localSize[0] = localSize.X;
			out->localSize[1] = localSize.Y;
			out->absSize[0]   = absSize.X;
			out->absSize[1]   = absSize.Y;

			SDK::FVector2D origin;
			origin.X = 0.0;
			origin.Y = 0.0;
			SDK::FVector2D pixel;
			SDK::FVector2D viewport;
			SDK::USlateBlueprintLibrary::LocalToViewport(worldContext, geo, origin, &pixel, &viewport);
			out->topLeft[0] = pixel.X;
			out->topLeft[1] = pixel.Y;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	inline bool LaidOut(const Geo& g)
	{
		return g.absSize[0] > 0.0 && g.absSize[1] > 0.0 && g.localSize[0] > 0.0 && g.localSize[1] > 0.0;
	}

	// World to viewport pixels.
	struct Transform
	{
		double originX, originY; // pixel of the world origin
		double scale;            // pixels per world unit
	};

	// The terrain widget's top-left corner is the dev settings' pivot point, and
	// its on-screen size per widget unit is the scale.
	inline Transform MakeTransform(const Geo& terrain, double pivotX, double pivotY)
	{
		Transform t;
		t.scale   = terrain.absSize[0] / terrain.localSize[0] / kWorldPerLocalUnit;
		t.originX = terrain.topLeft[0] - pivotX * t.scale;
		t.originY = terrain.topLeft[1] - pivotY * t.scale;
		return t;
	}

	namespace Detail
	{
		// Tick only. A marker that agreed last tick is tried first. Handle numbers,
		// not pointers: the widget is looked up again each tick.
		inline int32_t g_refIndex  = -1;
		inline int32_t g_refSerial = 0;

		// A building marker's widget sits with its top-left corner on the
		// building's world point. -1 when it is hidden, not laid out or where the
		// transform puts it outside the canvas (Slate does not paint it there, so
		// its geometry is whatever it last was), else 1 when it is where the
		// transform says and 0 when it is not.
		inline int CompareReference(SDK::UCrUW_MapMenuMarker* marker, double worldX, double worldY, SDK::UObject* ctx,
		                            const Transform& t, const Geo& canvas)
		{
			const double expectedX = t.originX + worldX * t.scale;
			const double expectedY = t.originY + worldY * t.scale;
			if (expectedX < canvas.topLeft[0] || expectedX > canvas.topLeft[0] + canvas.absSize[0] ||
				expectedY < canvas.topLeft[1] || expectedY > canvas.topLeft[1] + canvas.absSize[1])
				return -1;

			Geo g;
			if (!marker || !SampleGeo(marker, ctx, &g) || !g.visible || !LaidOut(g))
				return -1;

			const double dx = expectedX - g.topLeft[0];
			const double dy = expectedY - g.topLeft[1];
			return std::fabs(dx) <= kTolerancePx && std::fabs(dy) <= kTolerancePx ? 1 : 0;
		}

		// Looks at up to kMaxReferences building markers the game shows and counts
		// how many sit where the transform puts them. When the widget map cannot be
		// walked at the running game's layout, counts none and sets *widgetMapFault
		// to the loader's reason text.
		inline void CheckReferences(SDK::UCrUW_MapMenuMapArea* area, SDK::ACrMapMenuDataReplicationHelper* helper,
		                            SDK::UObject* ctx, const Transform& t, const Geo& canvas, int* checked, int* agreed,
		                            const char** widgetMapFault)
		{
			*checked = 0;
			*agreed  = 0;
			__try
			{
				auto* widgetsAt   = Fields::MapMassMarkers.In(area);
				auto* containerAt = Fields::BuildingsMarkers.In(helper);
				auto* dataAt      = containerAt ? Fields::BuildingsMarkerData.In(containerAt) : nullptr;
				if (!widgetsAt || !dataAt)
					return;

				// The widget map is walked at the stride of the running game's
				// pair layout (key and value sizes), not the compiled one.
				const PluginMap::ScriptMapView widgets(widgetsAt,
					PluginMap::MapLayout(sizeof(SDK::FMassEntityHandle), alignof(SDK::FMassEntityHandle),
						Fields::MarkerRepresentationSize.resolvedSize, alignof(SDK::FEntityMarkerUIRepresentation)));
				if (!widgets)
				{
					*widgetMapFault = widgets.ReasonText();
					return;
				}

				auto& data = *dataAt;
				const int32_t dataNum = data.Num();
				if (widgets.Num() > kMaxSaneCount || dataNum < 0 || dataNum > kMaxSaneCount)
					return;

				int scanned = 0;
				bool done = false;
				for (int pass = 0; pass < 2 && !done; ++pass)
				{
					widgets.ForEach([&](uint8_t* keyAt, uint8_t* valueAt)
					{
						const auto& handle = *reinterpret_cast<const SDK::FMassEntityHandle*>(keyAt);
						const bool isLast = handle.Index == g_refIndex && handle.SerialNumber == g_refSerial;
						if (isLast != (pass == 0))
							return true;
						if (pass == 1 && ++scanned > kMaxScan)
							return false;

						for (int32_t d = 0; d < dataNum; ++d)
						{
							const uint8_t* b = PluginMap::ElementAt(data, d, Fields::MarkerItemSize);
							const auto* bHandle  = Fields::MarkerBuildingHandle.In(b);
							const auto* location = Fields::MarkerLocation.In(b);
							auto*       marker   = Fields::MarkerWidget.In(valueAt);
							if (!bHandle || !location || !marker)
								continue;
							if (bHandle->Index != handle.Index || bHandle->SerialNumber != handle.SerialNumber)
								continue;

							const int r = CompareReference(*marker, location->X, location->Y, ctx, t, canvas);
							if (r >= 0)
							{
								++*checked;
								if (r == 1)
								{
									++*agreed;
									g_refIndex  = handle.Index;
									g_refSerial = handle.SerialNumber;
								}
								else if (isLast)
									g_refIndex = -1;
							}
							break;
						}

						if (*checked >= kMaxReferences)
						{
							done = true;
							return false;
						}
						return true;
					});
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				*checked = 0;
				*agreed  = 0;
			}
		}

		inline SDK::ACrMapMenuDataReplicationHelper* ReplicationHelper()
		{
			SDK::UWorld* world = PluginSdkRuntime::GetWorld();
			if (!world || !world->GameState || !world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
				return nullptr;

			return PluginMap::ValueOr(Fields::MapDataHelper, static_cast<SDK::ACrGameStateBase*>(world->GameState));
		}
	}

	// ---- sampling the map ----------------------------------------------------------

	// What the render callback draws from, as plain numbers.
	struct Snapshot
	{
		bool   valid;
		double originX, originY; // pixel of the world origin
		double scale;            // pixels per world unit
		float  dpi;              // pixels per widget unit
		float  clip[4];          // the map canvas, viewport pixels
	};

	enum class Status
	{
		Ready,        // the snapshot is valid
		NoMap,        // the map is not the active widget on the menu layer
		Unreadable,   // a widget, the dev settings or the geometry could not be read
		Mismatch      // the game's own markers are not where the transform puts them
	};

	struct Result
	{
		Status      status;
		int         checked;        // markers compared; with Mismatch, how many
		const char* widgetMapFault; // null, or why the game's widget map could not be walked (the
		                            // transform stands, as with no markers to compare against)
	};

	// Game thread, once per tick. Fills `out`, valid only when the status is
	// Ready, and leaves it all zero otherwise. Hide the overlay on anything but
	// Ready. With Mismatch, `checked` markers were compared and none agreed.
	inline Result Sample(Snapshot* out)
	{
		*out = Snapshot{};
		Result result = { Status::NoMap, 0, nullptr };

		SDK::UCrUW_MapMenu* map = ActiveLocalMap();
		SDK::UCrUW_MapMenuMapArea* area = PluginMap::ValueOr(Fields::MapMenuMapArea, map);
		SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
		auto* terrainWidget = PluginMap::ValueOr(Fields::MapTerrain, area);
		auto* canvasWidget  = PluginMap::ValueOr(Fields::MapCanvasPanel, area);
		const auto* pivot   = Fields::MapPivotPoint.In(settings);
		if (!area || !terrainWidget || !canvasWidget || !pivot)
		{
			result.status = map ? Status::Unreadable : Status::NoMap;
			return result;
		}

		SDK::ACrPlayerControllerBase* pc = LocalController();
		Geo terrain, canvas;
		if (!SampleGeo(terrainWidget, pc, &terrain) || !SampleGeo(canvasWidget, pc, &canvas) ||
			!LaidOut(terrain) || !LaidOut(canvas))
		{
			result.status = Status::Unreadable;
			return result;
		}

		const Transform t = MakeTransform(terrain, pivot->X, pivot->Y);

		// Hidden when markers the game placed are not where the transform says;
		// with none to compare against, the transform stands.
		SDK::ACrMapMenuDataReplicationHelper* helper = Detail::ReplicationHelper();
		int checked = 0;
		int agreed  = 0;
		if (helper)
			Detail::CheckReferences(area, helper, pc, t, canvas, &checked, &agreed, &result.widgetMapFault);
		result.checked = checked;
		if (checked > 0 && agreed == 0)
		{
			result.status = Status::Mismatch;
			return result;
		}

		out->valid   = true;
		out->originX = t.originX;
		out->originY = t.originY;
		out->scale   = t.scale;
		out->dpi     = static_cast<float>(canvas.absSize[0] / canvas.localSize[0]);
		out->clip[0] = static_cast<float>(canvas.topLeft[0]);
		out->clip[1] = static_cast<float>(canvas.topLeft[1]);
		out->clip[2] = static_cast<float>(canvas.topLeft[0] + canvas.absSize[0]);
		out->clip[3] = static_cast<float>(canvas.topLeft[1] + canvas.absSize[1]);
		result.status = Status::Ready;
		return result;
	}

	// Sample under SEH. False after an access violation: the plugin hides its
	// overlay and stops sampling until RELOAD. Apart from Sample: a function
	// with __try may not hold objects that need unwinding, and Sample's callees
	// may.
	inline bool SampleGuarded(Snapshot* out, Result* result)
	{
		__try
		{
			*result = Sample(out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			*out = Snapshot{};
			return false;
		}
	}

	// ---- drawing (render thread) -------------------------------------------------

	// The viewport pixel of a world point, as the game's own map puts it (X
	// right, Y down).
	inline void ToPixel(const Snapshot& s, double worldX, double worldY, float* px, float* py)
	{
		*px = static_cast<float>(s.originX + worldX * s.scale);
		*py = static_cast<float>(s.originY + worldY * s.scale);
	}

	// Keeps what is drawn between Push and Pop inside the map canvas.
	inline void PushClip(IModLoaderImGui* ui, PluginDrawList dl, const Snapshot& s)
	{
		ui->DL_PushClipRect(dl, s.clip[0], s.clip[1], s.clip[2], s.clip[3], true);
	}

	inline void PopClip(IModLoaderImGui* ui, PluginDrawList dl)
	{
		ui->DL_PopClipRect(dl);
	}
}

#undef PLUGIN_MAP_OVERLAY_FIELD
#undef PLUGIN_MAP_OVERLAY_FIELD_ANY_SIZE
