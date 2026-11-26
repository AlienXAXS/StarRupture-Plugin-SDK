// item_stack_limits.h tests. A console program: the helper only reads and
// writes two members of an item object and checks its GObjects slot, so
// stand-ins for the game SDK (stubs/) are enough and no game is needed. The
// offsets the loader would resolve are set by hand, the way a scanner answer
// sets them.
#include <Basic.hpp>
#include <plugin_interface.h>
#include <item_stack_limits.h>

#include <cstdio>
#include <cstddef>

using namespace SDK;
using ItemStackLimits::Compose;
using ItemStackLimits::kMaxStackCap;
using ItemStackLimits::Limits;

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

static UClass g_itemClass;

// Puts the item in slot `index` of the fake table.
static void Place(UAuItemDataBase& item, int32 index, int32_t maxStack, ENxItemStackType type = ENxItemStackType::StackByType)
{
	auto& slots = UObject::GObjects.Objects;
	if (static_cast<int32>(slots.size()) <= index)
		slots.resize(index + 1, nullptr);
	slots[index] = &item;
	item.Index = index;
	item.Class = &g_itemClass;
	item.Flags = EObjectFlags::NoFlags;
	item.MaxStack = maxStack;
	item.StackingType = type;
}

// The game's ENxItemStackType enum as an object in slot 0, with DoNotStack at
// the given number.
static UEnum g_stackEnum;
static void PlaceStackEnum(int64 doNotStack)
{
	static std::vector<TPair<FName, int64>> names;
	names = { { FName("ENxItemStackType::StackByType"), 0 }, { FName("ENxItemStackType::DoNotStack"), doNotStack } };
	g_stackEnum.Name = FName("ENxItemStackType");
	g_stackEnum.CastFlags = EClassCastFlags::Enum;
	g_stackEnum.Names = TArray<TPair<FName, int64>>(names.data(), static_cast<int32>(names.size()));
	auto& slots = UObject::GObjects.Objects;
	if (slots.empty())
		slots.resize(1, nullptr);
	slots[0] = &g_stackEnum;
}

static void ResolveBothRows()
{
	ItemStackLimits::MaxStackField().offset = static_cast<int32_t>(offsetof(UAuItemDataBase, MaxStack));
	ItemStackLimits::StackingTypeField().offset = static_cast<int32_t>(offsetof(UAuItemDataBase, StackingType));
}

static bool g_resolves = true;
static bool FakeResolveField(const IPluginSelf*, const PluginFieldRequest* request, PluginFieldResult* result)
{
	if (!g_resolves)
		return false;
	result->offset = request->builtOffset;
	result->size = request->size;
	result->boolMask = 0;
	return true;
}

int main()
{
	PlaceStackEnum(static_cast<int64>(ENxItemStackType::DoNotStack));

	printf("compose\n");
	{
		Check(Compose(50, 200) == 200, "a bigger size raises the stack");
		Check(Compose(50, 20) == 50, "a smaller size never lowers a vanilla stack");
		Check(Compose(50, 50) == 50, "an equal size is the vanilla stack");
		Check(Compose(1, 100000) == kMaxStackCap, "the size is capped at 9999");
		Check(kMaxStackCap == 9999, "the cap is 9999");
		Check(Compose(1, 0) == 1 && Compose(1, -5) == 1, "zero and negative sizes leave the item at its minimum");
		Check(Compose(20000, 500) == 20000, "a vanilla stack above the cap is kept, not cut to the cap");
	}

	printf("keys and resolve\n");
	{
		auto keys = ItemStackLimits::Keys();
		Check(keys.size() == 2, "two keys");
		Check(std::string(keys[0]) == "UAuItemDataBase::MaxStack", "MaxStack key");
		Check(std::string(keys[1]) == "UAuItemDataBase::StackingType", "StackingType key");
		const PluginMap::Feature feature{ "stack-limits", "Item stack sizes", ItemStackLimits::Keys(), {} };
		Check(feature.keys.size() == 2 && feature.On(), "the keys make a feature, as in the header's example");

		IPluginHookScanner scanner{};
		scanner.ResolveField = &FakeResolveField;
		g_resolves = true;
		Check(ItemStackLimits::Resolve(&scanner, nullptr), "Resolve is true when the loader has both rows");
		Check(ItemStackLimits::Available(), "available after resolve");
		g_resolves = false;
		Check(!ItemStackLimits::Resolve(&scanner, nullptr), "Resolve is false when a row is missing");
	}

	printf("apply and restore\n");
	{
		ResolveBothRows();
		UAuItemDataBase a, b, c, d;
		Place(a, 1, 50);
		Place(b, 2, 1000);
		Place(c, 3, 10, ENxItemStackType::DoNotStack);
		Place(d, 4, 5);

		Limits limits;
		limits.SetItems({ &a, &b, &c, &d, nullptr });
		Check(limits.Tracked() == 4, "a null item is not tracked");

		limits.SetTarget(500);
		Check(limits.Apply() == 2, "Apply changes the two items that rise (a and d)");
		Check(a.MaxStack == 500 && d.MaxStack == 500, "raised to the size");
		Check(b.MaxStack == 1000, "an item above the size keeps its stack");
		Check(c.MaxStack == 10, "a do-not-stack item is not touched");
		Check(limits.Apply() == 0, "a second Apply with no change writes nothing");

		limits.SetTarget(200);
		limits.Apply();
		Check(a.MaxStack == 200 && d.MaxStack == 200, "a lower target re-composes from vanilla");
		limits.SetTarget(9000000);
		limits.Apply();
		Check(a.MaxStack == kMaxStackCap, "a huge target stops at the cap");
		limits.SetTarget(500);
		limits.Apply();
		Check(limits.Raised() == 2, "Raised counts the items above vanilla");

		Check(limits.RestoreAll() == 2, "RestoreAll changes the two raised items back");
		Check(a.MaxStack == 50 && d.MaxStack == 5 && b.MaxStack == 1000 && c.MaxStack == 10,
			"every item is back to its exact vanilla value");
		Check(limits.RestoreAll() == 0, "a second RestoreAll changes nothing");

		limits.Apply();
		Check(a.MaxStack == 500, "Apply works again after a restore");
		limits.SetTarget(0);
		Check(limits.Apply() == 2 && a.MaxStack == 50 && d.MaxStack == 5, "a target of 0 restores");
	}

	printf("freed items and the item list\n");
	{
		ResolveBothRows();
		UAuItemDataBase a, b;
		Place(a, 1, 50);
		Place(b, 2, 20);

		Limits limits;
		limits.SetItems({ &a, &b });
		limits.SetTarget(300);
		limits.Apply();

		// The game frees b: its slot is cleared, and a new object may take the address.
		UObject::GObjects.Objects[2] = nullptr;
		b.MaxStack = 77;
		Check(limits.Apply() == 0, "a freed item is skipped, nothing written to it");
		Check(limits.RestoreAll() == 1 && a.MaxStack == 50, "restore writes only the live item");
		Check(b.MaxStack == 77, "the freed item's memory is never written");

		// A flagged-for-destroy item is dead too.
		UObject::GObjects.Objects[2] = &b;
		b.Flags = EObjectFlags::BeginDestroyed;
		b.MaxStack = 20;
		limits.SetTarget(300);
		limits.Apply();
		Check(b.MaxStack == 20, "an item being destroyed is not written");

		// Dropping an item from the list puts it back first.
		b.Flags = EObjectFlags::NoFlags;
		limits.Apply();
		Check(b.MaxStack == 300, "b rises once it is live");
		limits.SetItems({ &a });
		Check(b.MaxStack == 20, "an item dropped from the list is restored");

		// Re-listing keeps the recorded vanilla value, never recaptures a raised one.
		limits.SetItems({ &a, &b });
		limits.SetTarget(400);
		limits.Apply();
		limits.SetItems({ &a, &b });
		limits.Apply();
		limits.RestoreAll();
		Check(a.MaxStack == 50 && b.MaxStack == 20, "vanilla survives a re-list while raised");
	}

	printf("an item listed twice\n");
	{
		ResolveBothRows();
		UAuItemDataBase a;
		Place(a, 1, 50);

		Limits limits;
		limits.SetItems({ &a, &a });
		Check(limits.Tracked() == 1, "a repeated item is tracked once");
		limits.SetTarget(500);
		Check(limits.Apply() == 1 && limits.Apply() == 0, "Apply settles with a repeated item");
		limits.SetTarget(200);
		Check(limits.Apply() == 1 && limits.Apply() == 0, "a new target settles too");
		limits.SetItems({ &a, &a });
		limits.Apply();
		limits.RestoreAll();
		Check(a.MaxStack == 50, "a repeated item restores to its vanilla value after a re-list");
	}

	printf("an original of zero or less\n");
	{
		ResolveBothRows();
		UAuItemDataBase a, b;
		Place(a, 1, 0);
		Place(b, 2, -1);

		Limits limits;
		limits.SetItems({ &a, &b });
		limits.SetTarget(500);
		limits.Apply();
		Check(a.MaxStack == 500 && b.MaxStack == 500, "raised to the size");
		limits.RestoreAll();
		Check(a.MaxStack == 0 && b.MaxStack == -1, "restored to the exact original");
	}

	printf("the does-not-stack value\n");
	{
		ResolveBothRows();
		UAuItemDataBase a, b;
		Place(a, 1, 50, ENxItemStackType::DoNotStack);   // the compiled number
		Place(b, 2, 20, static_cast<ENxItemStackType>(5));

		PlaceStackEnum(5);
		Limits renumbered;
		renumbered.SetItems({ &a, &b });
		renumbered.SetTarget(500);
		renumbered.Apply();
		Check(b.MaxStack == 20, "the running game's number for DoNotStack is honoured");
		Check(a.MaxStack == 500, "the compiled number is not used");
		Check(!renumbered.StackTypeMissed(), "the lookup is not reported as missed");
		renumbered.RestoreAll();

		UObject::GObjects.Objects[0] = nullptr;
		Limits missing;
		missing.SetItems({ &a, &b });
		missing.SetTarget(500);
		Check(!missing.StackTypeMissed(), "not missed before the first Apply");
		Check(missing.Apply() == 0 && a.MaxStack == 50 && b.MaxStack == 20, "with no enum nothing is raised");
		Check(missing.StackTypeMissed(), "the miss is reported");
		PlaceStackEnum(static_cast<int64>(ENxItemStackType::DoNotStack));
		Check(missing.Apply() == 0, "the lookup runs once, not every tick");
	}

	printf("rows missing\n");
	{
		UAuItemDataBase a;
		Place(a, 1, 50);
		ItemStackLimits::MaxStackField().offset = -1;
		Limits limits;
		limits.SetItems({ &a });
		limits.SetTarget(500);
		Check(!ItemStackLimits::Available(), "not available with a row unresolved");
		Check(limits.Apply() == 0 && a.MaxStack == 50, "Apply does nothing and does not crash");
		Check(limits.RestoreAll() == 0, "RestoreAll does nothing");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
