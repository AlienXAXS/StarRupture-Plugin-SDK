#pragma once

// Helpers for a plugin that takes its game addresses, its game-class member
// offsets and the Game-SDK globals from the mod loader (plugin interface 72).
// The loader's version map holds rows for every game build it supports, so a
// plugin that asks through these keeps working when an update moves them.
//
// Needs no generated header: the game build the plugin was compiled for is
// PLUGIN_GAME_BUILT_CL (PluginGameBuild.props in this SDK). A
// plugin with rows of its own (version_map.generated.h) includes
// plugin_version_map.h instead, which adds them on top of this header.
//
// Include it after plugin_interface.h and the Game-SDK headers.
//
// In OnPluginLoadHooks:
//   g_beginPlay = PluginMap::Function(scanner, self, "UWorld::BeginPlay", "40 53 48 83 EC 20 ...");
//   PluginMap::ResolveFields(scanner, self, g_craftComponent, g_crafterSize);
// A part of the plugin that can be off while the rest runs is a feature
// (PluginMap::Feature): declare it before the resolves it covers, and a key
// that does not resolve turns that feature off instead of refusing the plugin.
//   static PluginMap::Feature s_laser{ "mining-laser", "Mining laser tuning",
//       { "ADroneLaser::Beam", "ADroneLaser::Fire" }, { "Drone.LaserScale" } };
//   PluginMap::DeclareFeatures(scanner, self, s_laser);
// ... later: if (s_laser.On()) { ... }
// In PluginInit, as its first statement, before any early return (off its
// game build the loader refuses a plugin whose PluginInit did not take the
// globals, a disabled one included):
//   PluginSdkRuntime::Init(self->hooks);
// and PluginSdkRuntime::GetWorld() in place of SDK::UWorld::GetWorld().

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace PluginMap
{
    // The request the helpers below send: hookName is a key in the loader's
    // map (PLUGIN_SCAN_FLAG_MAP_KEY), and pattern is this plugin's own
    // fallback, which may be null. A plugin that also has rows of its own
    // sets candidates (or PluginVersionMap::Attach) and passes the request
    // to scanner->Resolve itself.
    inline PluginScanRequest Request(const char* key, const char* pattern, int kind,
        uint32_t flags = 0, int32_t resultOffset = 0, int32_t followRel32At = 0)
    {
        PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
        request.hookName      = key;
        request.pattern       = pattern;
        request.kind          = kind;
        request.flags         = flags | PLUGIN_SCAN_FLAG_MAP_KEY;
        request.resultOffset  = resultOffset;
        request.followRel32At = followRel32At;
        return request;
    }

    // The game function `key`, "Class::Function" in Dumper-7 naming, the same
    // key for every plugin: the loader's map for the running build first,
    // then `pattern` (may be null). flags: PLUGIN_SCAN_FLAG_OPTIONAL, and
    // PLUGIN_SCAN_FLAG_FOLLOW_REL32 to follow the E8/E9 at followRel32At.
    // A kind other than FUNCTION_START or DATA makes `key` a site (see Site).
    // 0 = a recorded failure, which refuses the plugin unless a declared
    // feature lists `key` (then that feature is off).
    inline uintptr_t Function(IPluginHookScanner* scanner, const IPluginSelf* self, const char* key,
        const char* pattern, int kind = PLUGIN_SCAN_FUNCTION_START, uint32_t flags = 0,
        int32_t resultOffset = 0, int32_t followRel32At = 0)
    {
        const PluginScanRequest request = Request(key, pattern, kind, flags, resultOffset, followRel32At);
        return scanner->Resolve(self, &request);
    }

    // Function's address as a function pointer, e.g.
    // FunctionAs<void (*)(SDK::UWorld*)>(scanner, self, "UWorld::BeginPlay", pattern).
    template <class Fn>
    Fn FunctionAs(IPluginHookScanner* scanner, const IPluginSelf* self, const char* key,
        const char* pattern, int kind = PLUGIN_SCAN_FUNCTION_START, uint32_t flags = 0,
        int32_t resultOffset = 0, int32_t followRel32At = 0)
    {
        static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
            "FunctionAs takes a function pointer type");
        return reinterpret_cast<Fn>(Function(scanner, self, key, pattern, kind, flags, resultOffset, followRel32At));
    }

    // A game global (kind DATA) by its name, e.g. Global<SDK::UWorld*>(scanner,
    // self, "GWorld"). `pattern`, if given, matches the global's own bytes.
    // Null = a recorded failure.
    template <class T>
    T* Global(IPluginHookScanner* scanner, const IPluginSelf* self, const char* key, const char* pattern = nullptr)
    {
        return reinterpret_cast<T*>(Function(scanner, self, key, pattern, PLUGIN_SCAN_DATA));
    }

    // A site of this plugin's own: an address inside a function (kind
    // IN_FUNCTION, CODE or VTABLE), or with kind ANY an address inside a
    // global struct. The loader reads it as "<PluginInfo name>/<hookName>",
    // under a fingerprint of this request (kind, pattern, resultOffset, and
    // followRel32At when followed), so a site needs its own pattern, and a
    // changed pattern or offset reads none of the old rows.
    // 0 = a recorded failure.
    inline uintptr_t Site(IPluginHookScanner* scanner, const IPluginSelf* self, const char* hookName,
        const char* pattern, int kind, uint32_t flags = 0, int32_t resultOffset = 0, int32_t followRel32At = 0)
    {
        return Function(scanner, self, hookName, pattern, kind, flags, resultOffset, followRel32At);
    }
}

// One member (or, with member null, one class size) the plugin reads, as
// compiled; the loader's ResolveField sets `offset` (and `boolMask`) for the
// running build. Set once in OnPluginLoadHooks, read-only afterwards.
struct PluginFieldSpec
{
    const char* key;            // "Class::Member", the map's key
    const char* className;      // the Game-SDK's class name
    const char* member;         // null = a size row for className
    uint32_t    flags;          // PluginFieldFlags
    uint32_t    size;           // sizeof as compiled; 0 with PLUGIN_FIELD_ANY_SIZE = take the size the game reflects
    int32_t     builtOffset;    // offsetof as compiled; -1 for a bitfield or a size row
    int32_t     offset   = -1;  // -1 = not resolved
    uint8_t     boolMask = 0;
    uint32_t    resolvedSize = 0; // the size the loader reports for the member on the running build; 0 = not resolved
};

template <typename T>
struct PluginField : PluginFieldSpec
{
    // The member in `object`, or null when the object is null or the member
    // did not resolve on this build.
    T* In(void* object) const
    {
        return offset < 0 || !object ? nullptr : reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
    }
    const T* In(const void* object) const
    {
        return offset < 0 || !object ? nullptr : reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
    }
};

// A bool bitfield member: one byte and a mask, read-modify-write of that byte.
struct PluginBitField : PluginFieldSpec
{
    // False also when the object is null or the member did not resolve.
    bool Get(const void* object) const
    {
        return offset >= 0 && object && (static_cast<const uint8_t*>(object)[offset] & boolMask) != 0;
    }
    // False when nothing was written.
    bool Set(void* object, bool value) const
    {
        if (offset < 0 || !object || !boolMask)
            return false;
        uint8_t& byte = static_cast<uint8_t*>(object)[offset];
        byte = value ? static_cast<uint8_t>(byte | boolMask) : static_cast<uint8_t>(byte & ~boolMask);
        return true;
    }
};

// A member of an SDK class, e.g. SR_FIELD(SDK::UCrCraftingComponent*, ACrCrafter, CraftComponent, 0).
#define SR_FIELD(T, Class, Member, flags) \
    PluginField<T>{ { #Class "::" #Member, #Class, #Member, (flags), sizeof(T), \
                      static_cast<int32_t>(offsetof(SDK::Class, Member)) } }
// A bitfield bool; its built byte and mask are the row for the plugin's built
// game build (the loader's map, or the plugin's own).
#define SR_BITFIELD(Class, Member) \
    PluginBitField{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_BITFIELD, 1, -1 } }
// A member whose size is not the same on every build the plugin runs on: the
// loader accepts whatever size the game reflects and reports it in
// `resolvedSize`. Read it by that size (a float on one build, a double on the
// next), and treat a size the plugin does not handle as the member not there.
// A reflected member only: `flags` must not carry NATIVE, CHECK or BITFIELD.
#define SR_FIELD_ANY_SIZE(Class, Member, flags)     PluginFieldSpec{ #Class "::" #Member, #Class, #Member, (flags) | PLUGIN_FIELD_ANY_SIZE, 0,                      static_cast<int32_t>(offsetof(SDK::Class, Member)) }
// The size of an SDK class, checked on the running build (PLUGIN_FIELD_CHECK).
#define SR_SIZE_CHECK(Class) \
    PluginFieldSpec{ #Class, #Class, nullptr, PLUGIN_FIELD_CHECK, sizeof(SDK::Class), -1 }
// The size of an SDK class as the running game reflects it, not compared with
// the compiled one: the loader reports it in `resolvedSize`. For code that only
// steps through an array of the struct (PluginMap::ElementAt), so a build that
// resizes it does not turn the plugin off. Code that copies the struct, takes it
// from a ProcessEvent return or hands a buffer of it to the game depends on the
// compiled layout: use SR_SIZE_CHECK there.
#define SR_SIZE(Class) \
    PluginFieldSpec{ #Class, #Class, nullptr, PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::Class), -1 }

namespace PluginMap
{
    // Asks the loader for one field: the map's rows for its key first, then
    // `rows`, this plugin's own (may be null). builtCL is the game build the
    // plugin's SDK was generated for. False = the loader recorded a failure
    // (which refuses the plugin, unless a declared feature lists the field's
    // key: then that feature is off).
    inline bool ResolveField(IPluginHookScanner* scanner, const IPluginSelf* self, PluginFieldSpec& field,
        const PluginFieldCandidate* rows = nullptr, uint32_t rowCount = 0, uint32_t builtCL = PLUGIN_GAME_BUILT_CL)
    {
        PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
        request.key            = field.key;
        request.className      = field.className;
        request.member         = field.member;
        request.flags          = field.flags;
        request.size           = field.size;
        request.builtCL        = builtCL;
        request.builtOffset    = field.builtOffset;
        request.candidates     = rows;
        request.candidateCount = rows ? rowCount : 0;
        PluginFieldResult result{};
        if (!scanner->ResolveField(self, &request, &result))
            return false;
        field.offset       = result.offset;
        field.boolMask     = result.boolMask;
        field.resolvedSize = result.size;
        return true;
    }

    // Every field and check row; each is asked even after a failure, so the
    // log lists all of them. True when all resolved.
    template <class... Fields>
    bool ResolveFields(IPluginHookScanner* scanner, const IPluginSelf* self, Fields&... fields)
    {
        static_assert((std::is_base_of_v<PluginFieldSpec, Fields> && ...), "ResolveFields takes PluginField, PluginBitField and SR_SIZE_CHECK rows");
        bool all = true;
        ((all = ResolveField(scanner, self, fields) && all), ...);
        return all;
    }
}

namespace PluginMap
{
    // The member's value, or `fallback` when it did not resolve (its feature is
    // off), so code that can do without it still runs.
    template <class T, class Object>
    T ValueOr(const PluginField<T>& field, const Object* object, T fallback = T{})
    {
        const T* at = field.In(static_cast<const void*>(object));
        return at ? *at : fallback;
    }

    // A feature's keys from the rows it needs: a field or SR_SIZE_CHECK row, or
    // a key string (a Function, Global or Site key).
    inline void AddKey(std::vector<const char*>& keys, const char* key) { keys.push_back(key); }
    inline void AddKey(std::vector<const char*>& keys, const PluginFieldSpec& field) { keys.push_back(field.key); }

    template <class... Key>
    std::vector<const char*> KeysOf(const Key&... keys)
    {
        std::vector<const char*> all;
        (AddKey(all, keys), ...);
        return all;
    }
}

namespace PluginMap
{
    // Element `index` of `count` structs at `data`, stepped by the size the
    // loader resolved for `size` (an SR_SIZE row), not by a sizeof. Read the
    // address with the struct's resolved member offsets. Null when `data` is
    // null, `index` is outside 0..count-1 or the size did not resolve.
    inline const uint8_t* ElementAt(const void* data, int32_t count, int32_t index, const PluginFieldSpec& size)
    {
        if (!data || index < 0 || index >= count || size.resolvedSize == 0)
            return nullptr;
        return static_cast<const uint8_t*>(data) + static_cast<size_t>(index) * size.resolvedSize;
    }
    inline uint8_t* ElementAt(void* data, int32_t count, int32_t index, const PluginFieldSpec& size)
    {
        return const_cast<uint8_t*>(ElementAt(static_cast<const void*>(data), count, index, size));
    }

    // The same for a game TArray of the struct.
    template <class Element>
    const uint8_t* ElementAt(const SDK::TArray<Element>& array, int32_t index, const PluginFieldSpec& size)
    {
        return ElementAt(static_cast<const void*>(array.GetDataPtr()), array.Num(), index, size);
    }
    template <class Element>
    uint8_t* ElementAt(SDK::TArray<Element>& array, int32_t index, const PluginFieldSpec& size)
    {
        return const_cast<uint8_t*>(ElementAt(static_cast<const SDK::TArray<Element>&>(array), index, size));
    }
}

namespace PluginMap
{
    // How an EnumValue lookup ended.
    enum class EnumLookup
    {
        Found,         // the entry's value was written
        EntryMissing,  // the enum is loaded, but has no entry of that name on this build
        EnumMissing,   // no enum of that name is loaded
    };

    // The value of a game enum's entry, read by name from the running game's
    // UEnum: the numbers of a game enum can move in an update, its names are
    // what the game saves. `enumName` is the enum's object name as the Game-SDK
    // spells it ("ECrPlayerControlState"), `entry` the entry without the enum's
    // prefix ("DeconstructMode"). `value` is written only on Found.
    //
    // GAME THREAD ONLY, like any UObject access. It walks every object the
    // game has loaded, so look a value up once, when the feature first runs,
    // and keep it; never per frame. On EntryMissing or EnumMissing the plugin
    // logs it once and skips the path that needs the value, or uses its
    // compiled value only on the build it was compiled for.
    inline EnumLookup EnumValue(const char* enumName, const char* entry, int64_t& value)
    {
        const SDK::UEnum* uenum = enumName
            ? SDK::UObject::FindObjectFast<SDK::UEnum>(enumName, SDK::EClassCastFlags::Enum) : nullptr;
        if (!uenum)
            return EnumLookup::EnumMissing;
        for (int32_t i = 0; entry && i < uenum->Names.Num(); ++i)
        {
            // An entry is stored as "EnumName::Entry" (a plain enum: "Entry").
            const std::string name = uenum->Names[i].First.ToString();
            const size_t scope = name.rfind("::");
            if (name.compare(scope == std::string::npos ? 0 : scope + 2, std::string::npos, entry) == 0)
            {
                value = uenum->Names[i].Second;
                return EnumLookup::Found;
            }
        }
        return EnumLookup::EntryMissing;
    }
}

// Mass entity helpers a plugin reaches the game's entity manager and signal
// subsystem world through. Both read by name through the loader, so a plugin
// does not hard-code an offset, and a build with no row turns off the feature
// that lists the key.
//
//   PluginMap::ResolveMassEntityManager(scanner, self);          // key: PluginMap::MassEntityManagerField
//   void* manager = PluginMap::MassEntityManagerOf(subsystem);   // null when unresolved
//
// The signal subsystem's world is reflected and needs MassSignals_classes.hpp,
// which only a plugin that uses it includes:
//   PluginMap::ResolveMassSignalWorld(scanner, self);            // key: PluginMap::MassSignalWorldField()
//   SDK::UWorld* world = PluginMap::MassSignalWorldOf(subsystem);
namespace SDK
{
    class UEngine;
    class UMassSignalSubsystem;
    class UWorld;
}

namespace PluginMap
{
    // A TSharedPtr<T>: the object pointer and the shared reference controller.
    struct SharedRef
    {
        void* Object;
        void* Controller;
    };
    static_assert(sizeof(SharedRef) == 0x10, "a TSharedPtr is two pointers");

    // UMassEntitySubsystem::EntityManager is a TSharedPtr the Game-SDK folds
    // into padding (not reflected), so it has no SDK name: a native row. The
    // loader's map holds its row per game build; the offset here and the row
    // below are CL-127004's, used when the map has none for the running build.
    inline PluginField<SharedRef> MassEntityManagerField{ { "UMassEntitySubsystem::EntityManager",
        "UMassEntitySubsystem", "EntityManager", PLUGIN_FIELD_NATIVE, sizeof(SharedRef), 0x38 } };

    inline const PluginFieldCandidate kMassEntityManagerRows[] = {
        { 127004, 0x38, static_cast<uint32_t>(sizeof(SharedRef)), 0, nullptr },
    };

    // Asks the loader for MassEntityManagerField. False when it did not resolve.
    inline bool ResolveMassEntityManager(IPluginHookScanner* scanner, const IPluginSelf* self)
    {
        return ResolveField(scanner, self, MassEntityManagerField, kMassEntityManagerRows,
            static_cast<uint32_t>(sizeof(kMassEntityManagerRows) / sizeof(kMassEntityManagerRows[0])));
    }

    // The FMassEntityManager* a UMassEntitySubsystem holds, or null when the
    // subsystem is null or the member did not resolve.
    inline void* MassEntityManagerOf(const void* subsystem)
    {
        const SharedRef* at = MassEntityManagerField.In(subsystem);
        return at ? at->Object : nullptr;
    }

    // UMassSignalSubsystem::CachedWorld, the world the subsystem was created
    // in. A template so this header does not need MassSignals_classes.hpp: the
    // offset as compiled is taken where a plugin uses it.
    template <class Subsystem = SDK::UMassSignalSubsystem>
    PluginField<SDK::UWorld*>& MassSignalWorldField()
    {
        static PluginField<SDK::UWorld*> field{ { "UMassSignalSubsystem::CachedWorld", "UMassSignalSubsystem",
            "CachedWorld", 0, sizeof(SDK::UWorld*), static_cast<int32_t>(offsetof(Subsystem, CachedWorld)) } };
        return field;
    }

    // Asks the loader for MassSignalWorldField(). False when it did not resolve.
    template <class Subsystem = SDK::UMassSignalSubsystem>
    bool ResolveMassSignalWorld(IPluginHookScanner* scanner, const IPluginSelf* self)
    {
        return ResolveField(scanner, self, MassSignalWorldField<Subsystem>());
    }

    // The world a UMassSignalSubsystem holds, or null when the subsystem is
    // null or the member did not resolve.
    template <class Subsystem = SDK::UMassSignalSubsystem>
    SDK::UWorld* MassSignalWorldOf(const void* subsystem)
    {
        const auto* at = MassSignalWorldField<Subsystem>().In(subsystem);
        return at ? *at : nullptr;
    }
}

// A part of a plugin that can be off while the rest runs. `keys` are the names
// the feature needs, as the plugin passes them to the loader: a Function,
// Global or Site key (the hookName, without any "<plugin>/"), or a field's
// "Class::Member". `configKeys` are the "Section.Key" config entries it drives
// ("Section.*" for a whole section), which the config page shows as off while
// the feature is off. A key listed by more than one feature turns off all of
// them.
//
// Shared keys: list a key in every feature that uses it, so its miss turns off
// exactly those. Leave a key core (in no feature) only when the plugin can do
// nothing without it, such as the object or hook every feature starts from:
// its miss refuses the plugin. Build the lists with KeysOf from the rows
// themselves, so a typo cannot leave a key core.
struct PluginMapFeature
{
    const char*              id;          // unique within the plugin
    const char*              title;       // what a player reads
    std::vector<const char*> keys;
    std::vector<const char*> configKeys;

    // True until the loader reports a key of this feature unresolved. Set by
    // DeclareFeatures, before OnPluginLoadHooks returns; read-only afterwards.
    bool on = true;
    bool On() const { return on; }
};

namespace PluginMap
{
    using Feature = PluginMapFeature;

    // Declares one feature. Call before resolving the keys it lists. False = the
    // loader did not take it (it declares nothing, so its keys stay core).
    inline bool DeclareFeature(IPluginHookScanner* scanner, const IPluginSelf* self, Feature& feature)
    {
        PluginFeatureDesc desc = PLUGIN_FEATURE_DESC_INIT;
        desc.id             = feature.id;
        desc.title          = feature.title;
        desc.keys           = feature.keys.data();
        desc.keyCount       = static_cast<uint32_t>(feature.keys.size());
        desc.configKeys     = feature.configKeys.data();
        desc.configKeyCount = static_cast<uint32_t>(feature.configKeys.size());
        desc.enabled        = &feature.on;
        return scanner->DeclareFeature(self, &desc);
    }

    // Every feature; each is declared even after a failure. True when all were.
    template <class... Features>
    bool DeclareFeatures(IPluginHookScanner* scanner, const IPluginSelf* self, Features&... features)
    {
        static_assert((std::is_same_v<Features, Feature> && ...), "DeclareFeatures takes PluginMap::Feature rows");
        bool all = true;
        ((all = DeclareFeature(scanner, self, features) && all), ...);
        return all;
    }
}

// A member the plugin can live without, found by name through the loader's
// reflection on first use and cached as an offset per class: a member of a
// Blueprint (_C) class, which has no SDK offset to compile in, or a native
// member whose feature simply turns off when it is missing. Game thread only (it is not synchronised). The cache
// key is the class's object index and name, never the class pointer alone,
// so a class unloaded and replaced looks the member up again.
template <typename T>
struct PluginLazyField
{
    const char* member;

    // The member in `object`, or null when the object is null, the property
    // is not there or its size is not sizeof(T): the feature using it is off.
    template <class Object>
    T* In(const IPluginObjectProperties* properties, Object* object)
    {
        if (!object || !object->Class || !properties)
            return nullptr;
        static_assert(sizeof(object->Class->Name) == sizeof(uint64_t), "FName is the 8-byte index and number");
        uint64_t name = 0;
        std::memcpy(&name, &object->Class->Name, sizeof(name));
        if (object->Class->Index != m_classIndex || name != m_className)
        {
            m_offset = -1;
            const PluginPropertyHandle property = properties->FindPropertyOnObject(object, member);
            if (property && properties->GetPropertySize(property) == sizeof(T))
            {
                const auto* at = static_cast<const uint8_t*>(properties->GetPropertyRawPtr(object, property));
                if (at)
                    m_offset = static_cast<int32_t>(at - reinterpret_cast<const uint8_t*>(object));
            }
            m_classIndex = object->Class->Index;
            m_className  = name;
        }
        return m_offset < 0 ? nullptr : reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(object) + m_offset);
    }

    // The cache, written by In only.
    int32_t  m_classIndex = -1;
    uint64_t m_className  = 0;
    int32_t  m_offset     = -1;
};

// The Game-SDK globals the loader found on the running build, handed to this
// plugin's own copy of the SDK. Each plugin compiles the SDK itself, so the
// loader's InitManually does not reach it.
namespace PluginSdkRuntime
{
    // Set by Init; null before, or when the loader had none to give.
    inline const PluginSdkGlobals*& Globals()
    {
        static const PluginSdkGlobals* globals = nullptr;
        return globals;
    }

    // The loader's reflection reader, set by Init with the globals; GetWorld
    // reads the engine's viewport through it off the SDK's build.
    inline const IPluginObjectProperties*& ObjectProperties()
    {
        static const IPluginObjectProperties* properties = nullptr;
        return properties;
    }

    // Takes the globals and points the SDK's GObjects and FName::AppendString
    // at them. False when the loader has none to give; the SDK then keeps its
    // Offsets::, which hold only on the build it was generated for, and the
    // loader does not run this plugin on any other build.
    inline bool Init(IPluginHooks* hooks)
    {
        const PluginSdkGlobals* globals = hooks && hooks->Engine && hooks->Engine->GetSdkGlobals
            ? hooks->Engine->GetSdkGlobals() : nullptr;
        if (!globals || globals->structSize < sizeof(PluginSdkGlobals) || !globals->gobjects || !globals->appendString)
            return false;
        SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(globals->gobjects));
        SDK::FName::InitManually(reinterpret_cast<void*>(globals->appendString));
        Globals() = globals;
        ObjectProperties() = hooks->ObjectProperties;
        return true;
    }

    // True on the build the SDK was generated for: no globals from the
    // loader, or its GObjects and AppendString are where Offsets:: has them.
    inline bool OnSdkBuild()
    {
        const PluginSdkGlobals* globals = Globals();
        const uintptr_t base = SDK::InSDKUtils::GetImageBase();
        return !globals || (globals->gobjects == base + SDK::Offsets::GObjects
            && globals->appendString == base + SDK::Offsets::AppendString);
    }

    // The current world, in place of SDK::UWorld::GetWorld(): the world of the
    // game viewport (UEngine::GameViewport->World). Game thread only.
    //
    // The engine's GWorld global is not that world. Code that builds a world
    // of its own (a preview or capture world the engine is not told about)
    // can leave GWorld pointing at it, while the viewport always shows the
    // world being played. The viewport's world and GWorld are both kept by
    // the game's world context, so they go null together on travel.
    //
    // On the SDK's own build the members are read as compiled. On any other
    // they are found by name in the running game's reflection, and their
    // size checked, the way the loader's map reads a reflected member. GWorld
    // (Offsets::GWorld, or the one the loader found) only when there is no
    // engine or no viewport yet (before the engine is up, and on a dedicated
    // server), or when a member is not found on this build.
    template <class World = SDK::UWorld, class Engine = SDK::UEngine>
    World* GetWorld()
    {
        Engine* engine = Engine::GetEngine();
        if (OnSdkBuild())
        {
            if (engine && engine->GameViewport)
                return static_cast<World*>(engine->GameViewport->World);
            return World::GetWorld();
        }

        using Viewport = decltype(std::declval<Engine&>().GameViewport);
        static PluginLazyField<Viewport> s_gameViewport{ "GameViewport" };
        static PluginLazyField<World*> s_viewportWorld{ "World" };
        if (const IPluginObjectProperties* properties = ObjectProperties(); engine && properties)
        {
            Viewport* viewport = s_gameViewport.In(properties, engine);
            if (viewport && *viewport)
            {
                if (World** world = s_viewportWorld.In(properties, *viewport))
                    return *world;
            }
        }
        const uintptr_t gworld = Globals()->gworld;
        return gworld ? *reinterpret_cast<World* const*>(gworld) : nullptr;
    }
}

