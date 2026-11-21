// Waypoint tests. A console program for PluginWaypoint::Math, the bearing and
// distance arithmetic, which uses no SDK types. The rest of plugin_waypoint.h
// is compiled against the stand-ins in stubs\ (running it needs the game) but
// is not run here.
#include <plugin_waypoint.h>

#include <cmath>
#include <cstdio>

using namespace PluginWaypoint;

static int g_checks = 0;
static int g_failed = 0;

static void Check(bool ok, const char* what)
{
	++g_checks;
	if (!ok)
	{
		++g_failed;
		printf("    FAIL %s\n", what);
	}
}

static bool Near(double a, double b)
{
	return std::fabs(a - b) < 1e-9;
}

// +180 and -180 are the same direction: straight behind.
static bool IsBehind(double a)
{
	return Near(std::fabs(a), 180.0);
}

int main()
{
	printf("bearing, facing +X (yaw 0)\n");
	{
		Check(Near(Math::RelativeBearingDeg(100.0, 0.0, 0.0), 0.0), "target on +X is ahead (0)");
		Check(Near(Math::RelativeBearingDeg(100.0, 100.0, 0.0), 45.0), "target on +X,+Y is 45 to the right");
		Check(Near(Math::RelativeBearingDeg(0.0, 100.0, 0.0), 90.0), "target on +Y is right (+90)");
		Check(Near(Math::RelativeBearingDeg(0.0, -100.0, 0.0), -90.0), "target on -Y is left (-90)");
		Check(IsBehind(Math::RelativeBearingDeg(-100.0, 0.0, 0.0)), "target on -X is behind (+-180)");
	}

	printf("bearing, turned (yaw 90, facing +Y)\n");
	{
		Check(Near(Math::RelativeBearingDeg(0.0, 100.0, 90.0), 0.0), "target on +Y is ahead");
		Check(Near(Math::RelativeBearingDeg(100.0, 0.0, 90.0), -90.0), "target on +X is left");
		Check(Near(Math::RelativeBearingDeg(-100.0, 0.0, 90.0), 90.0), "target on -X is right");
		Check(IsBehind(Math::RelativeBearingDeg(0.0, -100.0, 90.0)), "target on -Y is behind");
	}

	printf("bearing wraps at the yaw seam\n");
	{
		// Facing 170 degrees, target at -170 degrees: 20 to the right, not -340.
		const double a = -170.0 * Math::kPi / 180.0;
		Check(Near(Math::RelativeBearingDeg(std::cos(a), std::sin(a), 170.0), 20.0),
			"yaw 170, target at -170 turns +20, not -340");
		// The other way: facing -170, target at 170 is 20 to the left, not +340.
		const double b = 170.0 * Math::kPi / 180.0;
		Check(Near(Math::RelativeBearingDeg(std::cos(b), std::sin(b), -170.0), -20.0),
			"yaw -170, target at 170 turns -20, not +340");
		// A yaw outside +-180 is the same direction as its wrapped value.
		Check(Near(Math::RelativeBearingDeg(0.0, 100.0, 450.0), 0.0), "yaw 450 is yaw 90");
		Check(Near(Math::RelativeBearingDeg(0.0, 100.0, -270.0), 0.0), "yaw -270 is yaw 90");
	}

	printf("bearing stays in range\n");
	{
		bool inRange = true;
		for (int yaw = -720; yaw <= 720; yaw += 15)
			for (int ang = -360; ang <= 360; ang += 15)
			{
				const double rad = ang * Math::kPi / 180.0;
				const double r = Math::RelativeBearingDeg(std::cos(rad), std::sin(rad), yaw);
				if (r < -180.0 || r > 180.0)
					inRange = false;
			}
		Check(inRange, "result is always within [-180, 180]");
	}

	printf("distance\n");
	{
		Check(Near(Math::Distance3dMeters(100.0, 0.0, 0.0), 1.0), "100 cm is 1 m");
		Check(Near(Math::Distance3dMeters(0.0, -3000.0, 0.0), 30.0), "the sign does not matter");
		Check(Near(Math::Distance3dMeters(300.0, 400.0, 0.0), 5.0), "3-4-5 in the plane, in metres");
		Check(Near(Math::Distance3dMeters(200.0, 300.0, 600.0), 7.0), "in 3d, 2-3-6 is 7");
		Check(Near(Math::Distance3dMeters(0.0, 0.0, 0.0), 0.0), "no offset is 0 m");
		Check(Near(Math::Distance3dMeters(150000.0, 0.0, 0.0), 1500.0), "a long way off");
	}

	printf("\n%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
