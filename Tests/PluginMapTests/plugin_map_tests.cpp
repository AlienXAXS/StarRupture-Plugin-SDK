// plugin_map.h tests. A console program: SR_SIZE, ElementAt and EnumValue only
// need the loader's field answer and the SDK's object and name tables, so
// stand-ins for those (stubs/Basic.hpp, a scanner with only ResolveField) are
// enough and no game is needed.
#include <Basic.hpp>
#include <plugin_interface.h>
#include <plugin_map.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
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

// A Dumper-7 parameter struct as compiled: an in-parameter, then the return.
namespace SDK::Params { struct Widget_Resize { uint32_t InSize; uint8_t Pad[4]; uint64_t ReturnValue; }; }

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

// ---- run-time sized buffers, parameter blocks and container walks ----

// Reference element types: the compiled C++ layout the live arithmetic must
// reproduce (what the Game-SDK's TMap<K, V> compiles to).
#pragma warning(push)
#pragma warning(disable : 4324) // padded by alignas: the point of the V30 case
namespace
{
	struct K4 { uint32_t a; };
	struct K8 { uint32_t a, b; };
	struct V18 { uint64_t w[3]; };
	struct V2C { uint32_t w[11]; };
	struct alignas(16) V30 { uint8_t b[0x30]; };
	struct E10 { uint32_t w[3]; }; // a 12-byte set element

	template <class K, class V> struct Pair { K key; V value; };
	template <class T> struct SetElem { T value; int32_t hashNextId; int32_t hashIndex; };
	template <class T> union SparseSlot { T element; struct { int32_t prev, next; } link; };

	// Hand-checked strides of the cases below, held by the compiler too.
	static_assert(sizeof(SparseSlot<SetElem<Pair<K8, V18>>>) == 40, "stride 40");
	static_assert(sizeof(SparseSlot<SetElem<Pair<K4, V30>>>) == 80, "stride 80");
	static_assert(sizeof(SparseSlot<SetElem<Pair<K8, V30>>>) == 80, "stride 80");
	static_assert(sizeof(SparseSlot<SetElem<Pair<K8, V2C>>>) == 60, "stride 60");
	static_assert(sizeof(SparseSlot<SetElem<E10>>) == 20, "stride 20");

	template <class K, class V>
	void CheckAgainstCompiled(const char* what)
	{
		using P = Pair<K, V>;
		const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(sizeof(K), alignof(K), sizeof(V), alignof(V));
		char text[96];
		snprintf(text, sizeof(text), "%s: map layout is valid", what);
		Check(layout.valid, text);
		snprintf(text, sizeof(text), "%s: value offset", what);
		Check(layout.valueOffset == offsetof(P, value), text);
		snprintf(text, sizeof(text), "%s: HashNextId offset", what);
		Check(layout.hashNextIdOffset == offsetof(SetElem<P>, hashNextId), text);
		snprintf(text, sizeof(text), "%s: HashIndex offset", what);
		Check(layout.hashIndexOffset == offsetof(SetElem<P>, hashIndex), text);
		snprintf(text, sizeof(text), "%s: set element size", what);
		Check(layout.elementSize == sizeof(SetElem<P>), text);
		snprintf(text, sizeof(text), "%s: sparse stride", what);
		Check(layout.stride == sizeof(SparseSlot<SetElem<P>>), text);
	}

	// A TSet built to the engine's layout in plain memory.
	struct SyntheticSet
	{
		std::vector<uint8_t>  data;
		std::vector<uint32_t> bits;
		std::vector<int32_t>  hash;
		PluginMap::ScriptSetMemory mem{};
		PluginMap::ScriptSetLayout layout;

		// `stride` is the reference layout's, not the code under test's. `used[i]` marks slot i live; fill(element, i) writes a live element's
		// payload. Unused slots get the free list; the rest of the memory is 0xCD.
		template <class Fill>
		SyntheticSet(const PluginMap::ScriptSetLayout& l, uint32_t stride, const std::vector<bool>& used, Fill fill) : layout(l)
		{
			const int32_t num = static_cast<int32_t>(used.size());
			data.assign(static_cast<size_t>(num) * stride + 16, 0xCD);
			bits.assign(static_cast<size_t>((num + 31) / 32) + 1, 0);
			int32_t hashSize = 1;
			while (hashSize < num) hashSize <<= 1;
			hash.assign(static_cast<size_t>(hashSize), -1);

			int32_t prevFree = -1, firstFree = -1, freeCount = 0;
			for (int32_t i = 0; i < num; ++i)
			{
				uint8_t* element = data.data() + static_cast<size_t>(i) * stride;
				if (used[static_cast<size_t>(i)])
				{
					bits[static_cast<size_t>(i) / 32] |= 1u << (i % 32);
					fill(element, i);
					const int32_t next = -1, index = i % hashSize;
					std::memcpy(element + l.hashNextIdOffset, &next, 4);
					std::memcpy(element + l.hashIndexOffset, &index, 4);
				}
				else
				{
					const int32_t prev = prevFree, none = -1;
					std::memcpy(element, &prev, 4);
					std::memcpy(element + 4, &none, 4);
					if (prevFree >= 0)
						std::memcpy(data.data() + static_cast<size_t>(prevFree) * stride + 4, &i, 4);
					else
						firstFree = i;
					prevFree = i;
					++freeCount;
				}
			}
			mem.data = data.data();
			mem.num = num;
			mem.max = num;
			mem.numBits = num;
			mem.maxBits = num;
			mem.firstFreeIndex = firstFree;
			mem.numFreeIndices = freeCount;
			if (num <= 128)
				std::memcpy(mem.inlineBits, bits.data(), sizeof(mem.inlineBits) < bits.size() * 4 ? sizeof(mem.inlineBits) : bits.size() * 4);
			else
				mem.secondaryBits = bits.data();
			mem.secondaryHash = hash.data();
			mem.hashSize = hashSize;
		}
		SyntheticSet(const SyntheticSet&) = delete;
	};

	// A map of `slots` slots, `value` bytes of V per live pair holding `i + 1`
	// words of marker, the key a counter.
	std::unique_ptr<SyntheticSet> MakeMap(const PluginMap::ScriptSetLayout& l, uint32_t stride, const std::vector<bool>& used, uint32_t valueSize)
	{
		return std::make_unique<SyntheticSet>(l, stride, used, [&](uint8_t* element, int32_t i)
		{
			const uint32_t key = 100u + static_cast<uint32_t>(i);
			std::memcpy(element + l.keyOffset, &key, 4);
			for (uint32_t b = 0; b < valueSize; ++b)
				element[l.valueOffset + b] = static_cast<uint8_t>(i + 1);
		});
	}

	struct FakeFunction {};
	struct FakeObject
	{
		const FakeFunction* lastFunction = nullptr;
		void*               lastParams = nullptr;
		uint32_t            returnOffset = 0;
		void ProcessEvent(FakeFunction* function, void* params)
		{
			lastFunction = function;
			lastParams = params;
			const uint64_t result = 0xA11CE;
			std::memcpy(static_cast<uint8_t*>(params) + returnOffset, &result, sizeof(result));
		}
	};
}

#pragma warning(pop)

static void TestBuffer()
{
	printf("StructBuffer\n");
	PluginFieldSpec size = SR_SIZE(FWidget);
	size.resolvedSize = 0x2C; // the build's size, not the compiled 0x10
	PluginMap::StructBuffer buffer(size);
	Check(static_cast<bool>(buffer), "a resolved size gives a buffer");
	Check(buffer.Size() == 0x2C, "of the resolved size, not the compiled one");
	Check(reinterpret_cast<uintptr_t>(buffer.Data()) % 16 == 0, "aligned to 16");
	bool zero = true;
	for (uint32_t i = 0; i < buffer.Size(); ++i)
		zero = zero && buffer.Data()[i] == 0;
	Check(zero, "zeroed");

	PluginField<uint32_t> member{ { "FWidget::Count", "FWidget", "Count", 0, 4, 0 } };
	member.offset = 0x28; // the last word of the live struct
	uint32_t* at = member.In(buffer.Data());
	Check(at && reinterpret_cast<uint8_t*>(at) + 4 == buffer.Data() + buffer.Size(), "members are reached through the existing accessor");
	*at = 7;

	std::vector<uint8_t> game(0x2C, 0x5A);
	Check(buffer.CopyFrom(game.data()) && buffer.Data()[0x2B] == 0x5A, "a game struct of that size copies in");
	std::vector<uint8_t> out(0x2C, 0);
	Check(buffer.CopyTo(out.data()) && out[0] == 0x5A, "and the block copies back out");
	Check(!buffer.CopyFrom(nullptr) && !buffer.CopyTo(nullptr), "null sources and destinations copy nothing");

	PluginMap::StructBuffer moved(std::move(buffer));
	Check(static_cast<bool>(moved) && !buffer && buffer.Size() == 0, "a move hands the block over");

	PluginFieldSpec unresolved = SR_SIZE(FWidget);
	Check(!PluginMap::StructBuffer(unresolved), "an unresolved size gives no buffer");
	Check(!PluginMap::StructBuffer(PluginMap::StructBuffer::kMaxSize + 1), "an implausible size gives no buffer");
	Check(!PluginMap::StructBuffer(), "a default buffer is empty");
	Check(buffer.Data() == nullptr, "and has no data");

	PluginMap::StructBuffer big(0x40);
	Check(big.Size() == 0x40 && reinterpret_cast<uintptr_t>(big.Data()) % 16 == 0, "alignment holds for other sizes");
}

static void TestParamsBlock()
{
	printf("ParamsBlock\n");
	// A function whose live ParmsSize (0x48) is not the compiled one (0x28):
	// an in-parameter at 0x00, a return at 0x30.
	PluginMap::ParamsBlock block(0x48, { { "InSize", 0x00, 0x10 }, { "ReturnValue", 0x30, 0x18 } });
	Check(static_cast<bool>(block), "a block of the live ParmsSize is valid");
	Check(block.Size() == 0x48, "sized from the live ParmsSize");
	Check(block.Param("InSize") == static_cast<uint8_t*>(block.Data()), "a parameter is at its reflected offset");
	Check(block.Param("ReturnValue") == static_cast<uint8_t*>(block.Data()) + 0x30, "so is the return value");
	Check(block.Param("Nope") == nullptr && block.Param(nullptr) == nullptr, "an unknown parameter is null");
	Check(reinterpret_cast<uintptr_t>(block.Data()) % 16 == 0, "aligned to 16");
	Check(*block.Param("ReturnValue") == 0 && block.Param("ReturnValue")[0x17] == 0, "zeroed");

	FakeObject object;
	object.returnOffset = 0x30;
	FakeFunction function;
	Check(PluginMap::Call(&object, &function, block), "the call goes through");
	Check(object.lastFunction == &function && object.lastParams == block.Data(), "with the function and the block");
	uint64_t result = 0;
	std::memcpy(&result, block.Param("ReturnValue"), sizeof(result));
	Check(result == 0xA11CE, "the return is read back at its offset");

	PluginMap::ParamsBlock tooBig(0x28, { { "ReturnValue", 0x30, 0x18 } });
	Check(!tooBig && tooBig.Param("ReturnValue") == nullptr, "a parameter beyond the live ParmsSize makes the block invalid");
	PluginMap::ParamsBlock overlapsEnd(0x48, { { "ReturnValue", 0x38, 0x18 } });
	Check(!overlapsEnd, "so does one that starts inside and ends outside");
	PluginMap::ParamsBlock noSize(0, {});
	Check(!noSize, "a zero ParmsSize is invalid");
	FakeObject untouched;
	Check(!PluginMap::Call(&untouched, &function, noSize) && untouched.lastParams == nullptr, "an invalid block makes no call");
	Check(!PluginMap::Call(static_cast<FakeObject*>(nullptr), &function, block), "a null object makes no call");
	PluginMap::ParamsBlock empty(0x10, {});
	Check(static_cast<bool>(empty), "a function with no reflected parameters to touch still gets a block");
}

// Each request answers with the offset and size its class and member name
// carry, as the loader's reflected lookup of a parameter would.
static bool ParamResolve(const IPluginSelf*, const PluginFieldRequest* request, PluginFieldResult* result)
{
	g_lastRequest = *request;
	if (request->member == nullptr)
	{
		result->offset = 0;
		result->size = 0x20; // ParmsSize on this build
	}
	else if (std::strcmp(request->member, "InSize") == 0)
	{
		result->offset = 0;
		result->size = 4;
	}
	else
	{
		result->offset = 0x18; // not the compiled 0x08: a parameter was inserted
		result->size = 8;
	}
	result->boolMask = 0;
	return true;
}

static void TestParamMacros()
{
	printf("SR_PARAM, SR_PARAMS_SIZE and ParamsBlockOf\n");
	PluginFieldSpec size   = SR_PARAMS_SIZE(Widget, Resize);
	PluginFieldSpec inSize = SR_PARAM(Widget, Resize, InSize);
	PluginFieldSpec ret    = SR_PARAM(Widget, Resize, ReturnValue);

	Check(std::strcmp(size.key, "Widget_Resize") == 0 && std::strcmp(size.className, "Widget_Resize") == 0 && size.member == nullptr,
		"the size row is the parameter struct's name");
	Check((size.flags & (PLUGIN_FIELD_CHECK | PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD | PLUGIN_FIELD_ANY_SIZE)) == 0, "and carries no check, native, bitfield or size flag");
	Check((size.flags & PLUGIN_FIELD_LATE) != 0 && (inSize.flags & PLUGIN_FIELD_LATE) != 0 && (ret.flags & PLUGIN_FIELD_LATE) != 0,
		"the size row and the parameters may wait for a Blueprint class");
	Check(size.size == sizeof(SDK::Params::Widget_Resize), "the compiled size is recorded");
	Check(std::strcmp(ret.key, "Widget_Resize::ReturnValue") == 0 && std::strcmp(ret.className, "Widget_Resize") == 0
		&& std::strcmp(ret.member, "ReturnValue") == 0, "a parameter is keyed by struct and member");
	Check((ret.flags & PLUGIN_FIELD_ANY_SIZE) != 0 && ret.size == 0, "and takes the size the game reflects");
	Check(ret.builtOffset == static_cast<int32_t>(offsetof(SDK::Params::Widget_Resize, ReturnValue)), "the compiled offset is recorded");

	IPluginHookScanner scanner{};
	scanner.ResolveField = &ParamResolve;
	Check(!PluginMap::ParamsBlockOf(size, { &inSize, &ret }), "unresolved rows make an invalid block");

	Check(PluginMap::ResolveFields(&scanner, nullptr, size, inSize, ret), "the rows resolve");
	Check(size.resolvedSize == 0x20 && ret.offset == 0x18 && ret.resolvedSize == 8, "with the live offsets and sizes");

	PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(size, { &inSize, &ret });
	Check(static_cast<bool>(block) && block.Size() == 0x20, "the block is the live ParmsSize");
	Check(block.Param("InSize") == block.Data(), "InSize is at its live offset");
	Check(block.Param("ReturnValue") == static_cast<uint8_t*>(block.Data()) + 0x18, "the return is at the live offset, not the compiled one");

	PluginFieldSpec tooLate = ret;
	tooLate.offset = 0x1C; // 0x1C + 8 > 0x20
	Check(!PluginMap::ParamsBlockOf(size, { &tooLate }), "a parameter past the live ParmsSize is invalid");
	PluginFieldSpec unresolvedParam = SR_PARAM(Widget, Resize, ReturnValue);
	Check(!PluginMap::ParamsBlockOf(size, { &unresolvedParam }), "an unresolved parameter is invalid");
	Check(!PluginMap::ParamsBlockOf(size, { nullptr }), "a null row is invalid");
	PluginFieldSpec unresolvedSize = SR_PARAMS_SIZE(Widget, Resize);
	Check(!PluginMap::ParamsBlockOf(unresolvedSize, { &ret }), "an unresolved block size is invalid");
}

// A Blueprint class the game has not loaded yet: the loader answers false (pending)
// until g_classLoaded, then the numbers, and counts the requests it was asked.
static bool g_classLoaded = false;
static int  g_lateAsks = 0;
static bool PendingResolve(const IPluginSelf* self, const PluginFieldRequest* request, PluginFieldResult* result)
{
	++g_lateAsks;
	if (!g_classLoaded)
		return false;
	return ParamResolve(self, request, result);
}

static void TestLateParams()
{
	printf("ResolveLateFields: pending until the class loads\n");
	PluginFieldSpec size   = SR_PARAMS_SIZE(Widget, Resize);
	PluginFieldSpec inSize = SR_PARAM(Widget, Resize, InSize);
	PluginFieldSpec ret    = SR_PARAM(Widget, Resize, ReturnValue);

	IPluginHookScanner scanner{};
	scanner.ResolveField = &PendingResolve;

	g_classLoaded = false;
	g_lateAsks = 0;
	Check(!PluginMap::ResolveFields(&scanner, nullptr, size, inSize, ret), "at load the class is not there: the rows do not resolve");
	Check(!PluginMap::IsResolved(size) && !PluginMap::IsResolved(inSize) && !PluginMap::IsResolved(ret), "and none is marked resolved");
	Check(!PluginMap::ParamsBlockOf(size, { &inSize }), "so the block is invalid: the feature stays off");
	Check(!PluginMap::ResolveLateFields(&scanner, nullptr, size, inSize, ret) && g_lateAsks == 6, "a late call still pending asks every row again and says so");

	g_classLoaded = true;
	g_lateAsks = 0;
	Check(PluginMap::ResolveLateFields(&scanner, nullptr, size, inSize, ret), "once the class is loaded the late call resolves them all");
	Check(PluginMap::IsResolved(size) && size.resolvedSize == 0x20 && ret.offset == 0x18, "with the live numbers");
	PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(size, { &inSize, &ret });
	Check(static_cast<bool>(block) && block.Size() == 0x20, "and the block is valid");

	// The class is freed again: the numbers stay and nothing asks the loader.
	g_classLoaded = false;
	g_lateAsks = 0;
	Check(PluginMap::ResolveLateFields(&scanner, nullptr, size, inSize, ret) && g_lateAsks == 0, "resolved rows are not asked again");
	Check(static_cast<bool>(PluginMap::ParamsBlockOf(size, { &inSize, &ret })), "and the block stays valid with the class unloaded");

	// A row that resolved alone is not asked again; the pending one is.
	PluginFieldSpec size2   = SR_PARAMS_SIZE(Widget, Resize);
	PluginFieldSpec inSize2 = SR_PARAM(Widget, Resize, InSize);
	g_classLoaded = true;
	PluginMap::ResolveField(&scanner, nullptr, size2);
	g_classLoaded = false;
	g_lateAsks = 0;
	Check(!PluginMap::ResolveLateFields(&scanner, nullptr, size2, inSize2) && g_lateAsks == 1, "only the pending row is asked");
}

static void TestLayouts()
{
	printf("Script layouts against the compiled layout\n");
	CheckAgainstCompiled<K8, V18>("key 8/4, value 0x18/8");
	CheckAgainstCompiled<K4, V30>("key 4/4, value 0x30/16");
	CheckAgainstCompiled<K8, V2C>("key 8/4, value 0x2C/4");
	CheckAgainstCompiled<K4, K4>("key 4/4, value 4/4");
	CheckAgainstCompiled<V18, K4>("key 0x18/8, value 4/4");

	const PluginMap::ScriptSetLayout set = PluginMap::SetLayout(sizeof(E10), alignof(E10));
	Check(set.valid && set.hashNextIdOffset == offsetof(SetElem<E10>, hashNextId)
		&& set.hashIndexOffset == offsetof(SetElem<E10>, hashIndex)
		&& set.elementSize == sizeof(SetElem<E10>) && set.stride == sizeof(SparseSlot<SetElem<E10>>), "a set of 12-byte elements");

	Check(!PluginMap::MapLayout(0, 4, 8, 4).valid, "a zero key size is no layout");
	Check(!PluginMap::MapLayout(8, 3, 8, 4).valid, "an alignment that is not a power of two is no layout");
	Check(!PluginMap::MapLayout(8, 4, 8, 0).valid, "a zero alignment is no layout");
	Check(!PluginMap::MapLayout(8, 4, 0x200000, 4).valid, "an implausible value size is no layout");
	Check(!PluginMap::SetLayout(0, 4).valid, "a zero set element is no layout");
}

static void TestMapWalk()
{
	printf("TMap walk\n");
	struct Case { const char* what; uint32_t keySize, keyAlign, valueSize, valueAlign, stride; };
	const Case cases[] = {
		{ "value 0x18/8", 8, 4, 0x18, 8, 40 },
		{ "value 0x30/16", 4, 4, 0x30, 16, 80 },
		{ "value 0x2C/4", 8, 4, 0x2C, 4, 60 },
	};
	for (const Case& c : cases)
	{
		char text[128];
		const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(c.keySize, c.keyAlign, c.valueSize, c.valueAlign);

		// Slots 1, 2, 5 and 6 are holes; the last slot is a hole too.
		const std::vector<bool> used{ true, false, false, true, true, false, false, true, true, false };
		auto map = MakeMap(layout, c.stride, used, c.valueSize);
		PluginMap::ScriptMapView view(&map->mem, layout);
		snprintf(text, sizeof(text), "%s: a map with holes is walkable", c.what);
		Check(static_cast<bool>(view), text);
		if (!view)
			printf("      (%s)\n", view.ReasonText());

		std::vector<uint32_t> keys;
		bool valuesOk = true;
		view.ForEach([&](uint8_t* key, uint8_t* value)
		{
			uint32_t k;
			std::memcpy(&k, key, 4);
			keys.push_back(k);
			const uint8_t marker = static_cast<uint8_t>(k - 100 + 1);
			for (uint32_t b = 0; b < c.valueSize; ++b)
				valuesOk = valuesOk && value[b] == marker;
		});
		snprintf(text, sizeof(text), "%s: the live pairs are yielded in slot order", c.what);
		Check(keys == std::vector<uint32_t>({ 100, 103, 104, 107, 108 }), text);
		snprintf(text, sizeof(text), "%s: every value is read whole at the run-time stride", c.what);
		Check(valuesOk, text);
		snprintf(text, sizeof(text), "%s: Num counts the live pairs", c.what);
		Check(view.Num() == 5, text);

		uint8_t* found = view.Find([](const uint8_t* key) { uint32_t k; std::memcpy(&k, key, 4); return k == 107; });
		snprintf(text, sizeof(text), "%s: Find returns the value of a key past a hole", c.what);
		Check(found && found[0] == 8 && found[c.valueSize - 1] == 8 && found == map->data.data() + 7 * c.stride + layout.valueOffset, text);
		snprintf(text, sizeof(text), "%s: Find of a missing key is null", c.what);
		Check(view.Find([](const uint8_t* key) { uint32_t k; std::memcpy(&k, key, 4); return k == 999; }) == nullptr, text);

		int seen = 0;
		view.ForEach([&](uint8_t*, uint8_t*) { return ++seen < 2; });
		snprintf(text, sizeof(text), "%s: a bool fn stops the walk", c.what);
		Check(seen == 2, text);

		// A full map has no holes.
		auto full = MakeMap(layout, c.stride, std::vector<bool>(6, true), c.valueSize);
		PluginMap::ScriptMapView fullView(&full->mem, layout);
		snprintf(text, sizeof(text), "%s: a full map has all its pairs", c.what);
		Check(fullView && fullView.Num() == 6, text);

		// More than 128 slots keep their bits on the heap.
		std::vector<bool> many(200, true);
		many[3] = many[150] = false;
		auto big = MakeMap(layout, c.stride, many, c.valueSize);
		PluginMap::ScriptMapView bigView(&big->mem, layout);
		snprintf(text, sizeof(text), "%s: a map past the inline bits is walkable", c.what);
		Check(bigView && bigView.Num() == 198, text);
	}

	// An empty map: no allocation at all.
	{
		const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(8, 4, 0x18, 8);
		PluginMap::ScriptSetMemory empty{};
		PluginMap::ScriptMapView view(&empty, layout);
		int calls = 0;
		view.ForEach([&](uint8_t*, uint8_t*) { ++calls; });
		Check(static_cast<bool>(view) && calls == 0 && view.Num() == 0, "an empty map is valid and yields nothing");
		Check(view.Find([](const uint8_t*) { return true; }) == nullptr, "and Find on it is null");
	}

	// A map that was emptied keeps its slots, all on the free list.
	{
		const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(4, 4, 0x30, 16);
		auto map = MakeMap(layout, 80, std::vector<bool>(4, false), 0x30);
		PluginMap::ScriptMapView view(&map->mem, layout);
		Check(view && view.Num() == 0, "a map with only holes is valid and empty");
	}

	// A set, through the same walk.
	{
		const PluginMap::ScriptSetLayout layout = PluginMap::SetLayout(sizeof(E10), alignof(E10));
		SyntheticSet set(layout, 20, { true, false, true, true }, [](uint8_t* element, int32_t i)
		{
			const uint32_t words[3] = { static_cast<uint32_t>(i), 7u, 9u };
			std::memcpy(element, words, sizeof(words));
		});
		PluginMap::ScriptSetView view(&set.mem, layout);
		std::vector<uint32_t> first;
		view.ForEach([&](uint8_t* element) { uint32_t w; std::memcpy(&w, element, 4); first.push_back(w); });
		Check(view && first == std::vector<uint32_t>({ 0, 2, 3 }), "a TSet of 12-byte elements walks at its own stride");
	}
}

static void TestWalkFailsClosed()
{
	printf("TMap walk fails closed\n");
	const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(8, 4, 0x30, 16);
	const std::vector<bool> used{ true, false, true, true, false, true };
	auto good = MakeMap(layout, 80, used, 0x30);

	auto yields = [](const PluginMap::ScriptMapView& view)
	{
		int n = 0;
		view.ForEach([&](uint8_t*, uint8_t*) { ++n; });
		return n;
	};
	{
		PluginMap::ScriptMapView view(&good->mem, layout);
		Check(view && view.Reason() == PluginMap::ScriptWalkReason::None, "the unbroken map passes");
	}
	{
		PluginMap::ScriptMapView view(nullptr, layout);
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::NullContainer && yields(view) == 0, "a null container yields nothing");
	}
	{
		PluginMap::ScriptMapView view(&good->mem, PluginMap::MapLayout(0, 4, 0x30, 16));
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::BadLayout && yields(view) == 0, "an unresolved size yields nothing");
	}
	{
		PluginMap::ScriptMapView view(&good->mem, PluginMap::ScriptSetLayout{});
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::BadLayout, "a default layout yields nothing");
	}
	{
		PluginMap::ScriptSetMemory copy = good->mem;
		copy.num = -3;
		PluginMap::ScriptMapView view(&copy, layout);
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::BadHeader, "a negative count fails");
		copy = good->mem;
		copy.max = copy.num - 1;
		Check(!PluginMap::ScriptMapView(&copy, layout), "a count past the capacity fails");
		copy = good->mem;
		copy.data = nullptr;
		Check(!PluginMap::ScriptMapView(&copy, layout), "elements with no data fail");
		copy = good->mem;
		copy.num = 1 << 25;
		copy.max = copy.num;
		Check(!PluginMap::ScriptMapView(&copy, layout), "an absurd count fails");
	}
	{
		PluginMap::ScriptSetMemory copy = good->mem;
		copy.numBits = copy.num + 1;
		PluginMap::ScriptMapView view(&copy, layout);
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::BadBits, "bits that do not cover the slots fail");
		copy = good->mem;
		copy.inlineBits[0] |= 1u << 1; // a hole marked used
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadBits, "bits that disagree with the free count fail");
		copy = good->mem;
		copy.num = 200;
		copy.max = 200;
		copy.numBits = 200;
		copy.numFreeIndices = 0;
		Check(!PluginMap::ScriptMapView(&copy, layout), "past the inline bits with no heap bits fails");
	}
	{
		PluginMap::ScriptSetMemory copy = good->mem;
		copy.firstFreeIndex = 0; // a used slot
		PluginMap::ScriptMapView view(&copy, layout);
		Check(!view && view.Reason() == PluginMap::ScriptWalkReason::BadFreeList, "a free list that starts on a used slot fails");
		copy = good->mem;
		copy.firstFreeIndex = -1;
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadFreeList, "holes with no free list fail");
		copy = good->mem;
		copy.numFreeIndices = 0;
		Check(!PluginMap::ScriptMapView(&copy, layout), "a free list with no free count fails");
		copy = good->mem;
		const int32_t loop = 1;
		std::vector<uint8_t> data = good->data;
		std::memcpy(data.data() + 1 * 80 + 4, &loop, 4); // slot 1 -> slot 1
		copy.data = data.data();
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadFreeList, "a free list that loops fails");
	}
	{
		PluginMap::ScriptSetMemory copy = good->mem;
		copy.hashSize = 6; // not a power of two
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadElement, "a hash size that is not a power of two fails");
		copy = good->mem;
		copy.hashSize = 2; // an element's bucket is outside it
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadElement, "a bucket outside the hash fails");
		copy = good->mem;
		std::vector<uint8_t> data = good->data;
		const int32_t bad = 77;
		std::memcpy(data.data() + 2 * 80 + layout.hashNextIdOffset, &bad, 4);
		copy.data = data.data();
		Check(PluginMap::ScriptMapView(&copy, layout).Reason() == PluginMap::ScriptWalkReason::BadElement, "a chain link outside the slots fails");
	}
	{
		// Memory that is no container of this shape.
		std::vector<uint8_t> noise(sizeof(PluginMap::ScriptSetMemory), 0xCD);
		PluginMap::ScriptMapView view(noise.data(), layout);
		Check(!view && yields(view) == 0, "an unknown layout (garbage memory) yields nothing");
		std::vector<uint8_t> ones(sizeof(PluginMap::ScriptSetMemory), 0xFF);
		Check(!PluginMap::ScriptMapView(ones.data(), layout), "so does all-ones memory");
	}
	{
		// The same live map read with a layout built from the compiled value
		// size (the stub's FWidget is 0x10): the stride is wrong and nothing is read.
		const PluginMap::ScriptSetLayout compiled = PluginMap::MapLayout(8, 4, sizeof(SDK::FWidget), 4);
		Check(compiled.stride != 80, "the compiled-size stride differs from the live one");
		PluginMap::ScriptMapView view(&good->mem, compiled);
		Check(!view && yields(view) == 0, "a layout from a compiled size yields nothing on a resized map");
		PluginMap::ScriptMapView live(&good->mem, layout);
		int n = 0;
		bool ok = true;
		live.ForEach([&](uint8_t*, uint8_t* value) { ++n; ok = ok && value[0x2F] == value[0]; });
		Check(live && n == 4 && ok, "while the live-size layout reads the same map whole");
	}
}

// The stride is the whole point: the pointers the walk gives must be where the
// live-size arithmetic puts them, for a value size that is not the compiled one.
static void TestStrideIsLive()
{
	printf("TMap walk stride\n");
	const PluginMap::ScriptSetLayout layout = PluginMap::MapLayout(4, 4, 0x28, 8);
	auto map = MakeMap(layout, 56, { true, true, false, true }, 0x28);
	PluginMap::ScriptMapView view(&map->mem, layout);
	std::vector<ptrdiff_t> at;
	view.ForEach([&](uint8_t* key, uint8_t*) { at.push_back(key - map->data.data()); });
	Check(view && at.size() == 3 && at[0] == 0 && at[1] == 56 && at[2] == 3 * 56, "pairs are stepped by the live stride");
	Check(layout.stride == 56, "which for a 0x28 value is 0x38, not a compiled 0x10 or 0x20");
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

	printf("SR_NATIVE_ANY_SIZE\n");
	{
		const auto field = SR_NATIVE_ANY_SIZE(uint8_t, FWidget, Block, 0x120);
		Check(std::strcmp(field.key, "FWidget::Block") == 0, "the key is Class::Member");
		Check(field.flags == (PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE), "native and any-size, nothing else");
		Check(field.size == 0, "the compiled size is 0");
		Check(field.builtOffset == 0x120, "the built offset is the one given");

		IPluginHookScanner scanner{};
		scanner.ResolveField = &FakeResolveField;
		auto live = SR_NATIVE_ANY_SIZE(uint8_t, FWidget, Block, 0x120);
		g_reflectedSize = 0x30;
		g_resolves = true;
		Check(PluginMap::ResolveField(&scanner, nullptr, live), "the member resolves");
		Check(live.resolvedSize == 0x30, "the loader's size lands in resolvedSize");
		Check((g_lastRequest.flags & (PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE)) == (PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE), "the request carries both flags");
	}

	TestBuffer();
	TestParamsBlock();
	TestParamMacros();
	TestLateParams();
	TestLayouts();
	TestMapWalk();
	TestWalkFailsClosed();
	TestStrideIsLive();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
