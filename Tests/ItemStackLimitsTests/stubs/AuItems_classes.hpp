#pragma once
// Stand-in for the Dumper-7 AuItems_classes.hpp: UAuItemDataBase with the two
// members item_stack_limits.h reads, at the offsets the Game-SDK gives them.
#include <Basic.hpp>

namespace SDK
{
	enum class ENxItemStackType : uint8_t
	{
		StackByType = 0,
		NewInstace = 1,
		DoNotStack = 2,
	};

	class UAuItemDataBase : public UObject
	{
	public:
		ENxItemStackType StackingType = ENxItemStackType::StackByType;
		int32_t          MaxStack = 1;
	};
}
