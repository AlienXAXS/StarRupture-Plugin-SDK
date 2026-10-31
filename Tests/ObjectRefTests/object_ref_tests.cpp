// ObjectRef tests. A console program: the helper only reads the object's
// Index, Class and Flags and the GObjects slot table, so a stand-in for those
// (stubs/Basic.hpp) is enough and no game is needed.
#include <object_ref.h>

#include <cstdio>
#include <type_traits>

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

struct UActor : UObject {};

static UClass g_classA, g_classB;

// Puts obj in slot `index` of the fake table.
static void Place(UObject& obj, int32 index, UClass* cls)
{
	auto& slots = UObject::GObjects->Slots;
	if (static_cast<int32>(slots.size()) <= index)
		slots.resize(index + 1, nullptr);
	slots[index] = &obj;
	obj.Index = index;
	obj.Class = cls;
	obj.Flags = EObjectFlags::NoFlags;
}

int main()
{
	static_assert(std::is_trivially_copyable_v<ObjectRef<UObject>>, "ObjectRef is a plain value");
	static_assert(sizeof(ObjectRef<UObject>) <= 24, "ObjectRef stays small");

	printf("validity\n");
	{
		UActor a;
		Place(a, 3, &g_classA);
		ObjectRef<UActor> ref(&a);
		Check(ref.Get() == &a, "live object is returned");

		ObjectRef<UActor> none;
		Check(none.Get() == nullptr, "default reference is empty");
		ObjectRef<UActor> fromNull(nullptr);
		Check(fromNull.Get() == nullptr, "reference to nullptr is empty");

		ref.Reset();
		Check(ref.Get() == nullptr, "Reset empties the reference");
		ref.Set(&a);
		Check(ref.Get() == &a, "Set re-arms the reference");
	}

	printf("slot reuse and collection\n");
	{
		UActor a, b;
		Place(a, 5, &g_classA);
		ObjectRef<UActor> ref(&a);

		UObject::GObjects->Slots[5] = &b;
		Check(ref.Get() == nullptr, "slot now holds another object: null");

		UObject::GObjects->Slots[5] = nullptr;
		Check(ref.Get() == nullptr, "slot cleared: null");

		UObject::GObjects->Slots[5] = &a;
		Check(ref.Get() == &a, "slot holds the object again: returned");

		// The same address reused for a new object of another class.
		a.Class = &g_classB;
		Check(ref.Get() == nullptr, "same slot and address, class changed: null");
		a.Class = &g_classA;
	}

	printf("dead flags\n");
	{
		const EObjectFlags dead[] = { EObjectFlags::BeginDestroyed, EObjectFlags::FinishDestroyed, EObjectFlags::MirroredGarbage };
		const char* names[] = { "BeginDestroyed: null", "FinishDestroyed: null", "MirroredGarbage: null" };
		for (int i = 0; i < 3; ++i)
		{
			UActor a;
			Place(a, 9, &g_classA);
			ObjectRef<UActor> ref(&a);
			a.Flags = dead[i];
			Check(ref.Get() == nullptr, names[i]);
		}
		UActor a;
		Place(a, 9, &g_classA);
		ObjectRef<UActor> ref(&a);
		a.Flags = static_cast<EObjectFlags>(0x4);
		Check(ref.Get() == &a, "unrelated flag: still live");
	}

	printf("copy and compare\n");
	{
		UActor a, b;
		Place(a, 11, &g_classA);
		Place(b, 12, &g_classA);
		ObjectRef<UActor> ra(&a), rb(&b);
		ObjectRef<UActor> copy = ra;
		Check(copy == ra, "copy equals original");
		Check(copy.Get() == &a, "copy resolves to the same object");
		Check(ra != rb, "references to different objects differ");
		Check(ObjectRef<UActor>() == ObjectRef<UActor>(), "two empty references are equal");
		Check(ra != ObjectRef<UActor>(), "empty differs from set");
		copy = rb;
		Check(copy == rb && copy.Get() == &b, "assignment takes the other reference");

		ObjectRef<UObject> base(&a);
		Check(base.Get() == &a, "ObjectRef<UObject> over a derived object");
	}

	printf("\n%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
