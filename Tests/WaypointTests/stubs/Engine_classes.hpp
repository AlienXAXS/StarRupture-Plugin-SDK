#pragma once
// Stand-in for the Dumper-7 Engine_classes.hpp, with only the types and calls
// include/plugin_waypoint.h names, so the header compiles without the game
// SDK. Names and signatures match the real SDK. The bodies are placeholders:
// the tests exercise PluginWaypoint::Math only and never call them.
#include <Basic.hpp>
#include <CoreUObject_classes.hpp>

namespace SDK
{
	struct FVector
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
	};

	struct FRotator
	{
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
	};

	class UWorld : public UObject {};

	class AActor : public UObject
	{
	public:
		FVector K2_GetActorLocation() const { return FVector(); }
	};

	class APlayerController : public UObject
	{
	public:
		void GetPlayerViewPoint(FVector*, FRotator*) const {}
		AActor* GetViewTarget() const { return nullptr; }
	};

	class UGameplayStatics : public UObject
	{
	public:
		static APlayerController* GetPlayerController(const UObject*, int32) { return nullptr; }
	};
}
