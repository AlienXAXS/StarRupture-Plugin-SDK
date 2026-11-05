// plugin_map.h tests. A console program: SR_SIZE, ElementAt and EnumValue only
// need the loader's field answer and the SDK's object and name tables, so
// stand-ins for those (stubs/Basic.hpp, a scanner with only ResolveField) are
// enough and no game is needed.
#include <Basic.hpp>
#include <plugin_interface.h>
#include <plugin_map.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace SDK;

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

// A struct whose compiled size (0x10) differs from the size a build reflects.
namespace SDK { struct FWidget { uint8_t bytes[0x10]; }; }

// A scanner that answers ResolveField with a fixed reflected size, and keeps
// the last request.
static uint32_t           g_reflectedSize = 0;
static bool               g_resolves = true;
static PluginFieldRequest g_lastRequest{};

static bool FakeResolveField(const IPluginSelf*, const PluginFieldRequest* request, PluginFieldResult* result)
{
	g_lastRequest = *request;
	if (!g_resolves)
		return false;
	result->offset = 0;
	result->size = g_reflectedSize;
	result->boolMask = 0;
	return true;
}

// A row as the loader would have filled it in.
static PluginFieldSpec ResolvedSize(uint32_t size)
{
	PluginFieldSpec row = SR_SIZE(FWidget);
	row.resolvedSize = size;
	return row;
}

static void AddEnum(UEnum& uenum, const char* name, std::vector<TPair<FName, int64>>& entries)
{
	uenum.Name = FName(name);
	uenum.CastFlags = EClassCastFlags::Enum;
	uenum.Names = TArray<TPair<FName, int64>>(entries.data(), static_cast<int32>(entries.size()));
	UObject::GObjects.Objects.push_back(&uenum);
}

int main()
{
	printf("SR_SIZE\n");
	{
		const PluginFieldSpec row = SR_SIZE(FWidget);
		Check(row.member == nullptr, "a size row has no member");
		Check(std::strcmp(row.key, "FWidget") == 0 && std::strcmp(row.className, "FWidget") == 0, "key and class are the struct name");
		Check((row.flags & (PLUGIN_FIELD_CHECK | PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD | PLUGIN_FIELD_ANY_SIZE)) == 0, "no check, native, bitfield or any-size flag");
		Check(row.size == sizeof(SDK::FWidget), "the compiled size is recorded");
		Check(row.resolvedSize == 0, "unresolved until the loader answers");

		const PluginFieldSpec checked = SR_SIZE_CHECK(FWidget);
		Check((checked.flags & PLUGIN_FIELD_CHECK) != 0, "SR_SIZE_CHECK still checks");

		IPluginHookScanner scanner{};
		scanner.ResolveField = &FakeResolveField;
		PluginFieldSpec live = SR_SIZE(FWidget);
		g_reflectedSize = 0x18;
		g_resolves = true;
		Check(PluginMap::ResolveField(&scanner, nullptr, live), "the live size resolves");
		Check(live.resolvedSize == 0x18, "the reflected size lands in resolvedSize");
		Check(g_lastRequest.member == nullptr && (g_lastRequest.flags & PLUGIN_FIELD_CHECK) == 0, "the request is a size row without CHECK");

		PluginFieldSpec failed = SR_SIZE(FWidget);
		g_resolves = false;
		Check(!PluginMap::ResolveField(&scanner, nullptr, failed), "a refused request reports false");
		Check(failed.resolvedSize == 0, "and leaves the size unresolved");
	}

	printf("ElementAt\n");
	{
		// Elements 0x18 apart, not sizeof(FWidget) = 0x10 apart.
		uint8_t buffer[0x18 * 3]{};
		const PluginFieldSpec size = ResolvedSize(0x18);

		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 0, size) == buffer, "element 0 is the start");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 1, size) == buffer + 0x18, "element 1 is one resolved size on");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 2, size) == buffer + 0x30, "element 2 is two resolved sizes on");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 1, size) != buffer + sizeof(SDK::FWidget), "not stepped by sizeof");

		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 3, size) == nullptr, "index past the end is null");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, -1, size) == nullptr, "negative index is null");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 0, 0, size) == nullptr, "empty array is null");
		Check(PluginMap::ElementAt(static_cast<void*>(nullptr), 3, 0, size) == nullptr, "null data is null");

		const PluginFieldSpec unresolved = SR_SIZE(FWidget);
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 0, unresolved) == nullptr, "an unresolved size is null, not sizeof");
		Check(PluginMap::ElementAt(static_cast<void*>(buffer), 3, 0, ResolvedSize(0)) == nullptr, "a zero size is null");

		const void* constData = buffer;
		Check(PluginMap::ElementAt(constData, 3, 2, size) == buffer + 0x30, "a const buffer works");

		// The TArray form reads the data pointer and Num from the array.
		TArray<FWidget> array(reinterpret_cast<FWidget*>(buffer), 3);
		Check(PluginMap::ElementAt(array, 1, size) == buffer + 0x18, "TArray element 1");
		Check(PluginMap::ElementAt(array, 3, size) == nullptr, "TArray index past Num is null");
		Check(PluginMap::ElementAt(array, 0, unresolved) == nullptr, "TArray with an unresolved size is null");
		const TArray<FWidget>& constArray = array;
		Check(PluginMap::ElementAt(constArray, 2, size) == buffer + 0x30, "const TArray element 2");
		TArray<FWidget> empty;
		Check(PluginMap::ElementAt(empty, 0, size) == nullptr, "empty TArray is null");
	}

	printf("EnumValue\n");
	{
		std::vector<TPair<FName, int64>> state{
			{ FName("ECrPlayerControlState::Normal"), 0 },
			{ FName("ECrPlayerControlState::BuildMode"), 1 },
			{ FName("ECrPlayerControlState::DeconstructMode"), 5 },
			{ FName("ECrPlayerControlState::ECrPlayerControlState_MAX"), 6 },
		};
		std::vector<TPair<FName, int64>> plain{
			{ FName("Storage"), 1 },
			{ FName("Source"), 2 },
		};
		UEnum controlState, logistics, notAnEnum;
		AddEnum(controlState, "ECrPlayerControlState", state);
		AddEnum(logistics, "ECrMassLogisticsType", plain);
		notAnEnum.Name = FName("ECrNotAnEnum");
		UObject::GObjects.Objects.push_back(&notAnEnum);

		int64_t value = -1;
		Check(PluginMap::EnumValue("ECrPlayerControlState", "DeconstructMode", value) == PluginMap::EnumLookup::Found, "a qualified entry is found");
		Check(value == 5, "and gives the game's number, not the compiled one");
		Check(PluginMap::EnumValue("ECrPlayerControlState", "Normal", value) == PluginMap::EnumLookup::Found && value == 0, "a first entry of 0 is found");
		Check(PluginMap::EnumValue("ECrMassLogisticsType", "Storage", value) == PluginMap::EnumLookup::Found && value == 1, "an unqualified entry is found");

		value = 77;
		Check(PluginMap::EnumValue("ECrPlayerControlState", "FlyMode", value) == PluginMap::EnumLookup::EntryMissing, "a missing entry is reported as such");
		Check(value == 77, "and leaves the value alone");
		Check(PluginMap::EnumValue("ECrPlayerControlState", "Mode", value) == PluginMap::EnumLookup::EntryMissing, "an entry is matched whole, not by suffix");
		Check(PluginMap::EnumValue("ECrPlayerControlState", nullptr, value) == PluginMap::EnumLookup::EntryMissing, "a null entry is missing");

		Check(PluginMap::EnumValue("ECrNoSuchEnum", "Normal", value) == PluginMap::EnumLookup::EnumMissing, "a missing enum is reported as such");
		Check(value == 77, "and leaves the value alone too");
		Check(PluginMap::EnumValue("ECrNotAnEnum", "Normal", value) == PluginMap::EnumLookup::EnumMissing, "an object that is not an enum is not one");
		Check(PluginMap::EnumValue(nullptr, "Normal", value) == PluginMap::EnumLookup::EnumMissing, "a null enum name is missing");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
