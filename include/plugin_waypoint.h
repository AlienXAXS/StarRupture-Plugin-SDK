#pragma once
// Waypoint: point the player at something. A small window shows a label, an X
// button, an arrow that turns with the camera and the distance to the target,
// and closes itself when the player arrives or the target is gone.
// Header-only; uses only IModLoaderImGui calls the loader already has, and
// needs nothing from the loader's version map (no offsets, sizes or natives).
//
// The helper registers nothing. The plugin keeps its own tick and widget:
//   - call Waypoint::Tick(world) from the plugin's engine tick (game thread),
//   - call Waypoint::Render(ui) from the widget's render callback,
//   - show the widget after Set/SetActor, hide it when Active() is false.
//
// Threads:
//   Tick runs on the game thread and is the only place a UObject is touched.
//   It writes a plain-data snapshot under a mutex. Render runs on the render
//   thread, copies that snapshot and does only arithmetic and ImGui calls.
//   Set, SetActor, Clear and Active are safe from any thread (a button click
//   runs on the render thread). No UObject pointer is kept anywhere: the
//   target is either a resolver function that finds it again every tick, or an
//   ObjectRef<AActor> that is re-checked every tick.
//
// Math:: holds the bearing and distance arithmetic as pure doubles with no SDK
// types, so a plugin that draws its own marker (the Compass's HUD, a map) can
// use it alone.

#include "plugin_interface.h"
#include "object_ref.h"

#include <Engine_classes.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace PluginWaypoint
{
	namespace Math
	{
		constexpr double kPi = 3.14159265358979323846;

		// Angle from the camera's forward direction to the target, in degrees.
		// dx, dy: target minus viewer in world units. yawDeg: the camera yaw
		// (Unreal convention: 0 is +X, increasing turns toward +Y).
		// 0 is straight ahead, +90 is right, -90 is left, +-180 is behind.
		// The result is always in [-180, 180].
		inline double RelativeBearingDeg(double dx, double dy, double yawDeg)
		{
			double rel = std::atan2(dy, dx) * 180.0 / kPi - yawDeg;
			rel = std::fmod(rel, 360.0);
			if (rel > 180.0)
				rel -= 360.0;
			else if (rel < -180.0)
				rel += 360.0;
			return rel;
		}

		// Straight-line distance in metres from a world-unit offset
		// (Unreal world units are centimetres).
		inline double Distance3dMeters(double dx, double dy, double dz)
		{
			return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
		}
	}

	// Where the target is right now. Plain data: safe to copy to another thread.
	struct Resolved
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		std::string label;   // optional; empty keeps the label given to Set
	};

	// Finds the target again. Called on the GAME thread only, once per tick
	// while the waypoint is active. Return false when the target is gone this
	// tick. Do not keep a UObject* from one call to the next.
	using ResolveFn = bool (*)(void* user, SDK::UWorld* world, Resolved& out);

	struct Options
	{
		// The waypoint closes itself within this many metres of the target.
		float autoCloseMeters = 10.0f;
		// How many ticks in a row the resolver may fail before the waypoint
		// closes. While it fails the window says "Location lost."
		int lostGraceTicks = 120;
		// Side of the square reserved for the arrow, in pixels.
		float arrowAreaSize = 90.0f;
		// Arrow fill colour (r, g, b, a).
		float arrowColor[4] = { 0.95f, 0.65f, 0.15f, 1.0f };
	};

	class Waypoint
	{
	public:
		explicit Waypoint(const Options& options = Options()) : m_opt(options) {}

		// Point at whatever fn finds. fallbackLabel is shown when fn leaves
		// Resolved::label empty. user is handed back to fn and must outlive
		// the waypoint (a static or plugin-owned object, never a UObject*).
		// Any thread.
		void Set(ResolveFn fn, void* user, std::string fallbackLabel)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_fn = fn;
			m_user = user;
			m_actor.Reset();
			Begin(fallbackLabel);
		}

		// Point at an actor. Build the ObjectRef on the game thread from a
		// live actor; Tick re-checks it every tick and reads its location.
		// Any thread.
		void SetActor(ObjectRef<SDK::AActor> actor, std::string label)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_fn = nullptr;
			m_user = nullptr;
			m_actor = actor;
			Begin(label);
		}

		// Stop pointing. Any thread.
		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_active = false;
		}

		// True from Set/SetActor until the waypoint is cleared, reached or lost.
		// Any thread.
		bool Active() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_active;
		}

		// GAME thread, once per tick. Resolves the target, samples the camera
		// and the player, and stores the result for Render. Closes the
		// waypoint when the player is within autoCloseMeters or the resolver
		// has failed lostGraceTicks ticks in a row.
		void Tick(SDK::UWorld* world)
		{
			ResolveFn fn;
			void* user;
			ObjectRef<SDK::AActor> actorRef;
			char fallback[kLabelSize];
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (!m_active)
					return;
				fn = m_fn;
				user = m_user;
				actorRef = m_actor;
				std::memcpy(fallback, m_label, kLabelSize);
			}

			Snapshot snap;
			bool lostThisTick = false;

			SDK::APlayerController* pc = world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
			if (pc)
			{
				// Camera yaw, as the loader's own look-at code reads it. The
				// control rotation can differ from the camera in a vehicle or
				// the drone.
				SDK::FVector camLoc{};
				SDK::FRotator camRot{};
				pc->GetPlayerViewPoint(&camLoc, &camRot);

				SDK::AActor* viewTarget = pc->GetViewTarget();
				const SDK::FVector playerLoc = viewTarget ? viewTarget->K2_GetActorLocation() : camLoc;

				snap.poseValid = true;
				snap.playerX = playerLoc.X;
				snap.playerY = playerLoc.Y;
				snap.playerZ = playerLoc.Z;
				snap.viewYaw = camRot.Yaw;

				Resolved target;
				bool found = false;
				if (fn)
				{
					found = fn(user, world, target);
				}
				else if (SDK::AActor* actor = actorRef.Get())
				{
					const SDK::FVector loc = actor->K2_GetActorLocation();
					target.x = loc.X;
					target.y = loc.Y;
					target.z = loc.Z;
					found = true;
				}

				if (found)
				{
					snap.targetValid = true;
					snap.targetX = target.x;
					snap.targetY = target.y;
					snap.targetZ = target.z;
					if (target.label.empty())
						std::memcpy(snap.label, fallback, kLabelSize);
					else
						CopyLabel(snap.label, target.label);
				}
				else
				{
					lostThisTick = true;
					std::memcpy(snap.label, fallback, kLabelSize);
				}
			}
			else
			{
				// No world or player yet (loading, travel): keep waiting. This
				// is not the target going missing, so it does not count.
				std::memcpy(snap.label, fallback, kLabelSize);
			}

			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_active)
				return;

			m_lostTicks = lostThisTick ? m_lostTicks + 1 : 0;
			if (m_lostTicks >= m_opt.lostGraceTicks)
			{
				m_active = false;
				return;
			}

			if (snap.poseValid && snap.targetValid)
			{
				const double meters = Math::Distance3dMeters(snap.targetX - snap.playerX,
					snap.targetY - snap.playerY, snap.targetZ - snap.playerZ);
				if (meters <= m_opt.autoCloseMeters)
				{
					m_active = false;
					return;
				}
			}

			m_snap = snap;
		}

		// RENDER thread, from the widget's callback. Draws the label, an X
		// button, the arrow and the distance. Returns false when there is
		// nothing to show (cleared, reached or lost, or X was pressed): hide
		// the widget then.
		bool Render(IModLoaderImGui* imgui)
		{
			Snapshot snap;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (!m_active)
					return false;
				snap = m_snap;
			}

			imgui->Text(snap.label);
			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton("X##PluginWaypointClose"))
			{
				Clear();
				return false;
			}

			imgui->Separator();

			if (!snap.poseValid)
			{
				imgui->TextDisabled("Player unavailable.");
				return true;
			}
			if (!snap.targetValid)
			{
				imgui->TextDisabled("Location lost.");
				return true;
			}

			const double dx = snap.targetX - snap.playerX;
			const double dy = snap.targetY - snap.playerY;
			const double dz = snap.targetZ - snap.playerZ;

			DrawArrow(imgui, Math::RelativeBearingDeg(dx, dy, snap.viewYaw));

			const double meters = Math::Distance3dMeters(dx, dy, dz);
			char text[32];
			if (meters < 1000.0)
				snprintf(text, sizeof(text), "%.0fm", meters);
			else
				snprintf(text, sizeof(text), "%.1fkm", meters / 1000.0);
			imgui->Text(text);
			return true;
		}

	private:
		static constexpr int kLabelSize = 64;

		// Everything Render needs, as plain data.
		struct Snapshot
		{
			bool poseValid = false;
			bool targetValid = false;
			double playerX = 0.0, playerY = 0.0, playerZ = 0.0;
			double viewYaw = 0.0;
			double targetX = 0.0, targetY = 0.0, targetZ = 0.0;
			char label[kLabelSize] = {};
		};

		// Callers hold m_mutex.
		void Begin(const std::string& label)
		{
			CopyLabel(m_label, label);
			m_lostTicks = 0;
			m_snap = Snapshot();
			std::memcpy(m_snap.label, m_label, kLabelSize);
			m_active = true;
		}

		// Copies as much of text as fits, without cutting a UTF-8 character.
		static void CopyLabel(char (&dst)[kLabelSize], const std::string& text)
		{
			size_t n = text.size();
			if (n > kLabelSize - 1)
			{
				n = kLabelSize - 1;
				while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
					--n;
			}
			std::memcpy(dst, text.data(), n);
			dst[n] = '\0';
		}

		// An isosceles arrow centred in a square reserved at the layout
		// cursor, pointing relativeDegrees from "up" (0 up, + clockwise).
		// Drawn with the v48 draw list, so it turns smoothly.
		void DrawArrow(IModLoaderImGui* imgui, double relativeDegrees) const
		{
			const float area = m_opt.arrowAreaSize;
			const float tipRadius = area * (38.0f / 90.0f);
			const float backRadius = area * (20.0f / 90.0f);
			const float halfWidth = area * (20.0f / 90.0f);

			float originX, originY;
			imgui->GetCursorScreenPos(&originX, &originY);
			imgui->Dummy(area, area);

			const float centerX = originX + area * 0.5f;
			const float centerY = originY + area * 0.5f;

			// Screen space: up is -Y, right is +X.
			const double angleRad = relativeDegrees * Math::kPi / 180.0;
			const float dirX = static_cast<float>(std::sin(angleRad));
			const float dirY = static_cast<float>(-std::cos(angleRad));
			// Perpendicular to (dirX, dirY).
			const float perpX = -dirY;
			const float perpY = dirX;

			const float tipX = centerX + dirX * tipRadius;
			const float tipY = centerY + dirY * tipRadius;
			const float backX = centerX - dirX * backRadius;
			const float backY = centerY - dirY * backRadius;
			const float leftX = backX + perpX * halfWidth;
			const float leftY = backY + perpY * halfWidth;
			const float rightX = backX - perpX * halfWidth;
			const float rightY = backY - perpY * halfWidth;

			PluginDrawList dl = imgui->GetWindowDrawList();
			const unsigned int fill = imgui->GetColorU32FromVec4(
				m_opt.arrowColor[0], m_opt.arrowColor[1], m_opt.arrowColor[2], m_opt.arrowColor[3]);
			const unsigned int outline = imgui->GetColorU32FromVec4(0.0f, 0.0f, 0.0f, 0.6f);
			imgui->DL_AddTriangleFilled(dl, tipX, tipY, leftX, leftY, rightX, rightY, fill);
			imgui->DL_AddTriangle(dl, tipX, tipY, leftX, leftY, rightX, rightY, outline, 2.0f);
		}

		const Options m_opt;

		mutable std::mutex m_mutex;
		bool m_active = false;
		ResolveFn m_fn = nullptr;
		void* m_user = nullptr;
		ObjectRef<SDK::AActor> m_actor;
		char m_label[kLabelSize] = {};
		int m_lostTicks = 0;
		Snapshot m_snap;
	};
}
