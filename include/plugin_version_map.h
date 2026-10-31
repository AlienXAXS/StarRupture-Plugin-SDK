#pragma once

// Helpers for a plugin that takes its game offsets from the mod loader's
// version map (plugin interface 72): its function locators and member offsets
// per game build, and the Game-SDK globals of the running build.
//
// Include it after plugin_interface.h, the Game-SDK headers and the plugin's own
// version_map.generated.h (plugin flavour, written by starrupture-ci
// ci/version-map.mjs), whose VersionMap::kBuiltCL, kFunctions, kFunctionRows,
// kFields and kFieldRows it reads.
//
// In OnPluginLoadHooks:
//   PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;   // hookName, pattern, ...
//   PluginVersionMap::Attach(req);                     // this key's rows, if any
//   scanner->Resolve(self, &req);
//   PluginVersionMap::ResolveFields(scanner, self, g_craftComponent, g_isOn);
// In PluginInit, before anything walks objects or reads names:
//   PluginSdkRuntime::Init(self->hooks);
// and PluginSdkRuntime::GetWorld() in place of SDK::UWorld::GetWorld().

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace PluginVersionMap
{
    // The entry for `key` in a generated index (sorted by key), or null.
    template <class Index, size_t N>
    const Index* Find(const std::array<Index, N>& index, const char* key)
    {
        if (!key)
            return nullptr;
        size_t lo = 0, hi = N;
        while (lo < hi)
        {
            const size_t mid = lo + (hi - lo) / 2;
            const int order = std::strcmp(index[mid].key, key);
            if (order == 0)
                return &index[mid];
            if (order < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return nullptr;
    }

    // Gives the request the map's locators for its hookName on every build in
    // the map. A key with no rows leaves the request as it is.
    inline void Attach(PluginScanRequest& request)
    {
        if (const auto* entry = Find(VersionMap::kFunctions, request.hookName))
        {
            request.candidates     = VersionMap::kFunctionRows.data() + entry->first;
            request.candidateCount = entry->count;
        }
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
    uint32_t    size;           // sizeof as compiled
    int32_t     builtOffset;    // offsetof as compiled; -1 for a bitfield or a size row
    int32_t     offset   = -1;  // -1 = not resolved
    uint8_t     boolMask = 0;
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
// A bitfield bool; its built byte and mask are the map's kBuiltCL row.
#define SR_BITFIELD(Class, Member) \
    PluginBitField{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_BITFIELD, 1, -1 } }
// The size of an SDK class, checked on the running build (PLUGIN_FIELD_CHECK).
#define SR_SIZE_CHECK(Class) \
    PluginFieldSpec{ #Class, #Class, nullptr, PLUGIN_FIELD_CHECK, sizeof(SDK::Class), -1 }

namespace PluginVersionMap
{
    // Asks the loader for one field with the map's rows for its key. False =
    // the loader recorded a failure (which refuses the plugin).
    inline bool ResolveField(IPluginHookScanner* scanner, IPluginSelf* self, PluginFieldSpec& field)
    {
        PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
        request.key         = field.key;
        request.className   = field.className;
        request.member      = field.member;
        request.flags       = field.flags;
        request.size        = field.size;
        request.builtCL     = VersionMap::kBuiltCL;
        request.builtOffset = field.builtOffset;
        if (const auto* entry = Find(VersionMap::kFields, field.key))
        {
            request.candidates     = VersionMap::kFieldRows.data() + entry->first;
            request.candidateCount = entry->count;
        }
        PluginFieldResult result{};
        if (!scanner->ResolveField(self, &request, &result))
            return false;
        field.offset   = result.offset;
        field.boolMask = result.boolMask;
        return true;
    }

    // Every field and check row; each is asked even after a failure, so the
    // log lists all of them. True when all resolved.
    template <class... Fields>
    bool ResolveFields(IPluginHookScanner* scanner, IPluginSelf* self, Fields&... fields)
    {
        static_assert((std::is_base_of_v<PluginFieldSpec, Fields> && ...), "ResolveFields takes PluginField, PluginBitField and SR_SIZE_CHECK rows");
        bool all = true;
        ((all = ResolveField(scanner, self, fields) && all), ...);
        return all;
    }
}

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

    // The current world, in place of SDK::UWorld::GetWorld(), which reads
    // Offsets::GWorld. On the SDK's own build that getter, as the loader does;
    // on any other, the engine's GWorld the loader found, or null.
    template <class World = SDK::UWorld>
    World* GetWorld()
    {
        if (OnSdkBuild())
            return World::GetWorld();
        const uintptr_t gworld = Globals()->gworld;
        return gworld ? *reinterpret_cast<World* const*>(gworld) : nullptr;
    }
}

// A member of a Blueprint (_C) class, which has no SDK offset to compile in:
// found by name through the loader's reflection on first use, then cached as
// an offset per class. Game thread only (it is not synchronised). The cache
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
