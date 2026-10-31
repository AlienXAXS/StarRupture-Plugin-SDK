#pragma once
// Stand-in for the Dumper-7 Basic.hpp, with only what include/object_ref.h
// touches, so the tests build without the game SDK. Member names and the
// GObjects access path match the real SDK.
#include <cstdint>
#include <vector>

namespace SDK
{
	using int32 = int32_t;

	enum class EObjectFlags : uint32_t
	{
		NoFlags = 0x00000000,
		BeginDestroyed = 0x00008000,
		FinishDestroyed = 0x00010000,
		MirroredGarbage = 0x40000000,
	};

	class UClass;
	class UObject;

	// One slot per index, like TUObjectArray::GetByIndex.
	class TUObjectArray
	{
	public:
		std::vector<UObject*> Slots;
		UObject* GetByIndex(const int32 Index) const
		{
			if (Index < 0 || Index >= static_cast<int32>(Slots.size()))
				return nullptr;
			return Slots[Index];
		}
	};

	class TUObjectArrayWrapper
	{
	public:
		TUObjectArray Array;
		TUObjectArray* operator->() { return &Array; }
	};

	class UObject
	{
	public:
		int32        Index = -1;
		UClass*      Class = nullptr;
		EObjectFlags Flags = EObjectFlags::NoFlags;

		static inline TUObjectArrayWrapper GObjects;
	};

	class UClass : public UObject {};
}
