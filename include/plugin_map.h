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
#include <initializer_list>
#include <new>
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
// A member reflection does not describe (a plain C++ type such as
// FMassEntityManager) whose size is not the same on every build: `offset` is its
// byte offset on the build the plugin was compiled for, and the loader reports
// the size the running build has in `resolvedSize`, as SR_FIELD_ANY_SIZE does.
// T is the type to read it as, e.g. uint8_t for a block read by `resolvedSize`.
// Needs a loader carrying feat/native-any-size: an older loader refuses the
// NATIVE | ANY_SIZE combination and the request fails.
#define SR_NATIVE_ANY_SIZE(T, Class, Member, offset) \
    PluginField<T>{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE, 0, (offset) } }
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
// A parameter (or the return value) of a game UFunction, at the offset and size
// the running game reflects it, not the ones the compiled Dumper-7 parameter
// struct Params::<Class>_<Function> has. Class is the game's class name without
// its prefix letter, as Dumper-7 spells it in that struct (CrEnviroWaveSubsystem,
// not UCrEnviroWaveSubsystem). Param is a member of the struct, "ReturnValue" for
// the return. The loader reports the offset in `offset` and the size in
// `resolvedSize`; a function name more than one class has, or none, is refused.
// Hand it to PluginMap::ParamsBlockOf with SR_PARAMS_SIZE. Needs a loader
// carrying feat/param-offsets.
#define SR_PARAM(Class, Function, Param) \
    PluginFieldSpec{ #Class "_" #Function "::" #Param, #Class "_" #Function, #Param, PLUGIN_FIELD_ANY_SIZE, 0, \
                     static_cast<int32_t>(offsetof(SDK::Params::Class##_##Function, Param)) }
// The size of a UFunction's parameter block (its ParmsSize) on the running game,
// in `resolvedSize`. The compiled size is recorded but not compared.
#define SR_PARAMS_SIZE(Class, Function) \
    PluginFieldSpec{ #Class "_" #Function, #Class "_" #Function, nullptr, PLUGIN_FIELD_FLAG_NONE, \
                     sizeof(SDK::Params::Class##_##Function), -1 }

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

// Structs the plugin cannot hold at a compiled size.
//
// SR_SIZE gives a struct's size on the running build (resolvedSize) and the
// field accessors give each member's offset on it. A plugin that copies the
// struct, has the game fill it or walks a game container of it still needs
// memory of that size. The helpers below give it: StructBuffer (a block of the
// struct's live size), ParamsBlock (the parameter block of a ProcessEvent call)
// and ScriptMapView / ScriptSetView (a TMap or TSet whose element size is only
// known live). Members of all of them are read and written through the same
// accessors as any game object, e.g. field.In(buffer.Data()).
namespace PluginMap
{
    // A zeroed block of one struct's live size, aligned to 16 bytes (every game
    // struct aligns to 16 or less). Empty when the size did not resolve or is
    // implausible, so the feature that needs it is turned off:
    //   PluginMap::StructBuffer params(kElectricitySize);
    //   if (!params) return;
    // The block lives as long as the object; moving it hands the block over.
    class StructBuffer
    {
    public:
        static constexpr size_t kAlignment = 16;
        static constexpr uint32_t kMaxSize = 1u << 20; // a larger "struct" is a bad size, not a struct

        StructBuffer() = default;
        explicit StructBuffer(const PluginFieldSpec& size) : StructBuffer(size.resolvedSize) {}
        explicit StructBuffer(uint32_t size)
        {
            if (size == 0 || size > kMaxSize)
                return;
            m_data = static_cast<uint8_t*>(::operator new(size, std::align_val_t{ kAlignment }));
            m_size = size;
            std::memset(m_data, 0, size);
        }
        StructBuffer(const StructBuffer&) = delete;
        StructBuffer& operator=(const StructBuffer&) = delete;
        StructBuffer(StructBuffer&& other) noexcept : m_data(other.m_data), m_size(other.m_size)
        {
            other.m_data = nullptr;
            other.m_size = 0;
        }
        StructBuffer& operator=(StructBuffer&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_data = other.m_data;
                m_size = other.m_size;
                other.m_data = nullptr;
                other.m_size = 0;
            }
            return *this;
        }
        ~StructBuffer() { Release(); }

        explicit operator bool() const { return m_data != nullptr; }
        uint8_t* Data() { return m_data; }
        const uint8_t* Data() const { return m_data; }
        uint32_t Size() const { return m_size; }

        // Copies the struct at `source` (a game struct of this live size) in.
        // False, and nothing copied, when the buffer is empty or `source` null.
        bool CopyFrom(const void* source)
        {
            if (!m_data || !source)
                return false;
            std::memcpy(m_data, source, m_size);
            return true;
        }
        // Copies the block over the game struct at `destination`.
        bool CopyTo(void* destination) const
        {
            if (!m_data || !destination)
                return false;
            std::memcpy(destination, m_data, m_size);
            return true;
        }

    private:
        void Release()
        {
            if (m_data)
                ::operator delete(m_data, std::align_val_t{ kAlignment });
            m_data = nullptr;
            m_size = 0;
        }

        uint8_t* m_data = nullptr;
        uint32_t m_size = 0;
    };
}

namespace PluginMap
{
    // One parameter (or the return value) of a UFunction as the running game
    // reflects it: its property offset in the parameter block, and its size.
    struct FunctionParam
    {
        const char* name;
        uint32_t    offset;
        uint32_t    size;
    };

    // The parameter block of a ProcessEvent call, sized from the function's
    // live ParmsSize rather than from a compiled parameter struct. The caller
    // passes the live ParmsSize and the reflected offset and size of each
    // parameter it touches (the return value included); a parameter that does
    // not fit inside ParmsSize makes the block invalid, so a build that changed
    // the function turns the feature off instead of writing out of bounds.
    //
    //   PluginMap::ParamsBlock block(parmsSize, { { "InSize", inOffset, inSize }, { "ReturnValue", retOffset, retSize } });
    //   if (!block) return;
    //   std::memcpy(block.Param("InSize"), &value, sizeof(value));
    //   PluginMap::Call(object, function, block);
    //   const uint8_t* result = block.Param("ReturnValue");
    //
    // Game thread only, like any ProcessEvent.
    class ParamsBlock
    {
    public:
        ParamsBlock(uint32_t liveParmsSize, std::vector<FunctionParam> params)
            : m_buffer(liveParmsSize), m_params(std::move(params))
        {
            for (const FunctionParam& param : m_params)
            {
                if (param.size == 0 || param.offset > liveParmsSize || param.size > liveParmsSize - param.offset)
                {
                    m_buffer = StructBuffer();
                    break;
                }
            }
        }

        explicit operator bool() const { return static_cast<bool>(m_buffer); }
        void* Data() { return m_buffer.Data(); }
        uint32_t Size() const { return m_buffer.Size(); }

        // The parameter `name` in the block, or null when the block is invalid
        // or has no such parameter.
        uint8_t* Param(const char* name)
        {
            if (!m_buffer || !name)
                return nullptr;
            for (const FunctionParam& param : m_params)
                if (param.name && std::strcmp(param.name, name) == 0)
                    return m_buffer.Data() + param.offset;
            return nullptr;
        }

    private:
        StructBuffer m_buffer;
        std::vector<FunctionParam> m_params;
    };

    // object->ProcessEvent(function, block). False, and no call, when the block
    // is invalid or the object or function is null.
    template <class Object, class Function>
    bool Call(Object* object, Function* function, ParamsBlock& block)
    {
        if (!object || !function || !block)
            return false;
        object->ProcessEvent(function, block.Data());
        return true;
    }

    // A ParamsBlock from the SR_PARAMS_SIZE row and the SR_PARAM rows of the
    // parameters the call touches, once the loader has resolved them
    // (ResolveFields). Invalid, so the feature stays off, when any row did not
    // resolve or a parameter does not fit the block.
    //
    //   PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(g_stageSize, { &g_stageReturn });
    //   if (!block) return;
    //   PluginMap::Call(wave, function, block);
    //   const uint8_t* settings = block.Param("ReturnValue");
    inline ParamsBlock ParamsBlockOf(const PluginFieldSpec& blockSize, std::initializer_list<const PluginFieldSpec*> params)
    {
        std::vector<FunctionParam> list;
        for (const PluginFieldSpec* param : params)
        {
            if (!param || !param->member || param->offset < 0 || param->resolvedSize == 0)
                return ParamsBlock(0, {});
            list.push_back(FunctionParam{ param->member, static_cast<uint32_t>(param->offset), param->resolvedSize });
        }
        return ParamsBlock(blockSize.resolvedSize, std::move(list));
    }
}

// Walking a game TMap or TSet whose element size is only known at run time.
//
// A TMap<K, V> is a TSet of TPair<K, V>, a TSet is a sparse array of elements
// (the element, then two int32 for the hash chain) and a sparse array is an
// array whose unused slots hold a free-list link and whose used slots are
// marked in a bit array. Compiled, `SDK::TMap<K, FStruct>` steps that array by
// sizeof(FStruct), which is wrong the moment the game resizes the struct. These
// views step it by the stride computed from the live key and value sizes.
//
// The layout is Unreal Engine 5's script layout, the same arithmetic as
// FScriptMap::GetScriptLayout, FScriptSet::GetScriptLayout and
// FScriptSparseArray::GetScriptLayout (UE 5.0 and later; the Game-SDK's
// UnrealContainers.hpp compiles to the same offsets). The loader's reflection
// interface does not expose an FMapProperty's stored layout, so it is computed
// from the key and value size and alignment. Sizes come from SR_SIZE rows; an
// alignment is the live UStruct::MinAlignment, which a struct keeps across a
// resize unless its members change. A layout that cannot be right for the
// container in memory is caught when a view is made (Reason says why), and the
// view then yields nothing.
namespace PluginMap
{
    // The offsets of a TSet element on the running build.
    struct ScriptSetLayout
    {
        uint32_t hashNextIdOffset = 0; // FSetElementId HashNextId
        uint32_t hashIndexOffset  = 0; // int32 HashIndex
        uint32_t elementSize      = 0; // TSetElement size, padded to its alignment
        uint32_t stride           = 0; // the sparse array's step: TSetElement padded to hold a free-list link
        uint32_t keyOffset        = 0; // map: TPair::Key in an element
        uint32_t valueOffset      = 0; // map: TPair::Value in an element; 0 for a set
        bool     valid            = false;
    };

    namespace ScriptLayoutDetail
    {
        inline bool IsPowerOfTwo(uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }
        inline uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
        inline uint32_t Max(uint32_t a, uint32_t b) { return a > b ? a : b; }
        // A size or alignment no game struct has.
        inline bool PlausibleSize(uint32_t size) { return size != 0 && size <= StructBuffer::kMaxSize; }
        inline bool PlausibleAlign(uint32_t align) { return IsPowerOfTwo(align) && align <= 64; }

        // FScriptSet::GetScriptLayout on an element of `size` and `align`.
        inline ScriptSetLayout SetOf(uint32_t size, uint32_t align)
        {
            ScriptSetLayout layout;
            if (!PlausibleSize(size) || !PlausibleAlign(align))
                return layout;
            layout.hashNextIdOffset = AlignUp(size, 4);
            layout.hashIndexOffset  = layout.hashNextIdOffset + 4;
            const uint32_t alignment = Max(align, 4);
            layout.elementSize      = AlignUp(layout.hashIndexOffset + 4, alignment);
            // FScriptSparseArray::GetScriptLayout: a free slot holds two int32.
            layout.stride           = AlignUp(Max(layout.elementSize, 8), alignment);
            layout.valid            = true;
            return layout;
        }
    }

    // The layout of a TSet of elements of `elementSize` and `elementAlign`.
    inline ScriptSetLayout SetLayout(uint32_t elementSize, uint32_t elementAlign)
    {
        return ScriptLayoutDetail::SetOf(elementSize, elementAlign);
    }

    // The layout of a TMap<Key, Value> (FScriptMap::GetScriptLayout): the pair
    // is the key, then the value at its alignment, padded to the larger one.
    inline ScriptSetLayout MapLayout(uint32_t keySize, uint32_t keyAlign, uint32_t valueSize, uint32_t valueAlign)
    {
        using namespace ScriptLayoutDetail;
        if (!PlausibleSize(keySize) || !PlausibleAlign(keyAlign) || !PlausibleSize(valueSize) || !PlausibleAlign(valueAlign))
            return ScriptSetLayout{};
        const uint32_t valueOffset = AlignUp(keySize, valueAlign);
        const uint32_t pairAlign   = Max(keyAlign, valueAlign);
        const uint32_t pairSize    = AlignUp(valueOffset + valueSize, pairAlign);
        ScriptSetLayout layout = SetOf(pairSize, pairAlign);
        layout.keyOffset   = 0;
        layout.valueOffset = valueOffset;
        return layout;
    }

    // The engine's TSet as it sits in memory (x64): a sparse array (data, its
    // used-slot bits, the free list), then the hash. Only the sparse array is
    // read; the hash is checked, never followed.
    struct ScriptSetMemory
    {
        uint8_t*  data;           // TArray: Data, Num, Max
        int32_t   num;
        int32_t   max;
        uint32_t  inlineBits[4];  // TBitArray<TInlineAllocator<4>>
        uint32_t* secondaryBits;
        int32_t   numBits;
        int32_t   maxBits;
        int32_t   firstFreeIndex;
        int32_t   numFreeIndices;
        int32_t   inlineHash[2];  // TInlineAllocator<1> int32 (8 with padding)
        int32_t*  secondaryHash;
        int32_t   hashSize;
        int32_t   padding;
    };
    static_assert(sizeof(void*) != 8 || sizeof(ScriptSetMemory) == 0x50, "a TSet is 0x50 bytes");
    static_assert(sizeof(void*) != 8 || offsetof(ScriptSetMemory, hashSize) == 0x48, "TSet::HashSize is at 0x48");

    // Why a view yields nothing. None = it yields every element.
    enum class ScriptWalkReason
    {
        None,
        NullContainer,
        BadLayout,       // a key, value or alignment that is not a game struct's
        BadHeader,       // counts that no container has
        BadBits,         // used-slot bits that do not match the counts
        BadFreeList,     // a free list that does not walk the unused slots
        BadElement,      // a hash field outside the hash: the element layout is not this container's
    };

    inline const char* ScriptWalkReasonText(ScriptWalkReason reason)
    {
        switch (reason)
        {
        case ScriptWalkReason::None:          return "ok";
        case ScriptWalkReason::NullContainer: return "container is null";
        case ScriptWalkReason::BadLayout:     return "element layout has an implausible size or alignment";
        case ScriptWalkReason::BadHeader:     return "container counts are inconsistent";
        case ScriptWalkReason::BadBits:       return "used-slot bits do not match the counts";
        case ScriptWalkReason::BadFreeList:   return "free list does not match the unused slots";
        case ScriptWalkReason::BadElement:    return "an element's hash fields are outside the hash: wrong element stride";
        }
        return "unknown";
    }

    // A walk over the live elements of a TSet, at the run-time stride.
    //   PluginMap::ScriptSetView view(set, PluginMap::SetLayout(size, align));
    //   if (!view) { log(view.ReasonText()); return; }
    //   view.ForEach([&](uint8_t* element) { ... });
    // Game thread only, and only inside the tick that fetched the container:
    // the pointers it gives die with the container's next change.
    class ScriptSetView
    {
    public:
        ScriptSetView(const void* set, const ScriptSetLayout& layout)
            : m_layout(layout)
        {
            m_reason = Validate(static_cast<const ScriptSetMemory*>(set));
            if (m_reason == ScriptWalkReason::None)
                m_set = static_cast<const ScriptSetMemory*>(set);
        }

        explicit operator bool() const { return m_reason == ScriptWalkReason::None; }
        ScriptWalkReason Reason() const { return m_reason; }
        const char* ReasonText() const { return ScriptWalkReasonText(m_reason); }
        const ScriptSetLayout& Layout() const { return m_layout; }

        // The live element count (0 for an empty or an invalid view).
        int32_t Num() const { return m_set ? m_set->num - m_set->numFreeIndices : 0; }

        // fn(uint8_t* element) for each live element, in slot order. A fn that
        // returns bool stops the walk by returning false. Gives nothing for an
        // invalid view.
        template <class Fn>
        void ForEach(Fn&& fn) const
        {
            if (!m_set)
                return;
            for (int32_t i = 0; i < m_set->num; ++i)
            {
                if (!IsUsed(*m_set, i))
                    continue;
                uint8_t* element = m_set->data + static_cast<size_t>(i) * m_layout.stride;
                if constexpr (std::is_same_v<decltype(fn(element)), bool>)
                {
                    if (!fn(element))
                        return;
                }
                else
                {
                    fn(element);
                }
            }
        }

    private:
        static const uint32_t* Bits(const ScriptSetMemory& set)
        {
            return set.secondaryBits ? set.secondaryBits : set.inlineBits;
        }
        static bool IsUsed(const ScriptSetMemory& set, int32_t index)
        {
            return ((Bits(set)[index >> 5] >> (index & 31)) & 1u) != 0;
        }
        static int32_t ReadInt(const uint8_t* at)
        {
            int32_t value;
            std::memcpy(&value, at, sizeof(value));
            return value;
        }

        ScriptWalkReason Validate(const ScriptSetMemory* set) const
        {
            if (!set)
                return ScriptWalkReason::NullContainer;
            if (!m_layout.valid || m_layout.stride == 0 || m_layout.hashIndexOffset + 4 > m_layout.elementSize)
                return ScriptWalkReason::BadLayout;

            const int32_t num = set->num;
            const int32_t kMaxElements = 1 << 24;
            if (num < 0 || num > kMaxElements || set->max < num || set->numFreeIndices < 0 || set->numFreeIndices > num
                || (num > 0 && !set->data))
                return ScriptWalkReason::BadHeader;
            if (num == 0)
                return set->numFreeIndices == 0 ? ScriptWalkReason::None : ScriptWalkReason::BadHeader;

            // The bits are one per slot; more than the inline four words need the heap block.
            if (set->numBits != num || (num > 128 && !set->secondaryBits))
                return ScriptWalkReason::BadBits;
            int32_t used = 0;
            for (int32_t i = 0; i < num; ++i)
                used += IsUsed(*set, i) ? 1 : 0;
            if (used != num - set->numFreeIndices)
                return ScriptWalkReason::BadBits;

            // The free list threads the unused slots through their first 8 bytes. With
            // a wrong stride it reads other bytes, and does not close on itself.
            int32_t free = set->firstFreeIndex;
            if (set->numFreeIndices == 0 && free != -1)
                return ScriptWalkReason::BadFreeList;
            for (int32_t seen = 0; seen < set->numFreeIndices; ++seen)
            {
                if (free < 0 || free >= num || IsUsed(*set, free))
                    return ScriptWalkReason::BadFreeList;
                free = ReadInt(set->data + static_cast<size_t>(free) * m_layout.stride + 4);
            }
            if (free != -1)
                return ScriptWalkReason::BadFreeList;

            // A used element's hash fields are in range of the hash.
            if (used > 0)
            {
                const uint32_t hashSize = static_cast<uint32_t>(set->hashSize);
                if (set->hashSize <= 0 || !ScriptLayoutDetail::IsPowerOfTwo(hashSize) || set->hashSize > kMaxElements * 2)
                    return ScriptWalkReason::BadElement;
                for (int32_t i = 0; i < num; ++i)
                {
                    if (!IsUsed(*set, i))
                        continue;
                    const uint8_t* element = set->data + static_cast<size_t>(i) * m_layout.stride;
                    const int32_t next  = ReadInt(element + m_layout.hashNextIdOffset);
                    const int32_t index = ReadInt(element + m_layout.hashIndexOffset);
                    if (index < 0 || index >= set->hashSize || next < -1 || next >= num)
                        return ScriptWalkReason::BadElement;
                }
            }
            return ScriptWalkReason::None;
        }

        const ScriptSetMemory* m_set = nullptr;
        ScriptSetLayout        m_layout;
        ScriptWalkReason       m_reason = ScriptWalkReason::None;
    };

    // A walk over the live pairs of a TMap, at the run-time stride.
    //   PluginMap::ScriptMapView map(mapField.In(object),
    //       PluginMap::MapLayout(keySize, keyAlign, valueSize.resolvedSize, valueAlign));
    //   if (!map) return;
    //   map.ForEach([&](uint8_t* key, uint8_t* value) { ... });
    //   uint8_t* value = map.Find([&](const uint8_t* key) { return SameTag(key, tag); });
    class ScriptMapView
    {
    public:
        ScriptMapView(const void* map, const ScriptSetLayout& layout) : m_set(map, layout) {}

        explicit operator bool() const { return static_cast<bool>(m_set); }
        ScriptWalkReason Reason() const { return m_set.Reason(); }
        const char* ReasonText() const { return m_set.ReasonText(); }
        int32_t Num() const { return m_set.Num(); }

        // fn(uint8_t* key, uint8_t* value) for each live pair; a fn that returns
        // bool stops the walk by returning false.
        template <class Fn>
        void ForEach(Fn&& fn) const
        {
            const ScriptSetLayout& layout = m_set.Layout();
            m_set.ForEach([&](uint8_t* element)
            {
                uint8_t* key = element + layout.keyOffset;
                uint8_t* value = element + layout.valueOffset;
                if constexpr (std::is_same_v<decltype(fn(key, value)), bool>)
                    return fn(key, value);
                else
                {
                    fn(key, value);
                    return true;
                }
            });
        }

        // The value of the first pair whose key `isKey(const uint8_t* key)` accepts,
        // or null: a linear walk, for the small maps a game keeps by tag. The
        // engine's TMap::operator[] steps by the compiled value size instead.
        template <class Pred>
        uint8_t* Find(Pred&& isKey) const
        {
            uint8_t* found = nullptr;
            ForEach([&](uint8_t* key, uint8_t* value)
            {
                if (!isKey(static_cast<const uint8_t*>(key)))
                    return true;
                found = value;
                return false;
            });
            return found;
        }

    private:
        ScriptSetView m_set;
    };
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

