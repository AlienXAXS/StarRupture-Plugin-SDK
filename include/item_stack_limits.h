#pragma once
#include <plugin_interface.h>
#include <AuItems_classes.hpp>
#include <plugin_map.h>
#include <object_ref.h>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

// Apply and restore item stack sizes.
//
// Raises UAuItemDataBase::MaxStack on the items you hand it and puts every
// value back to vanilla on request. A size only ever raises an item (one whose
// own stack is already bigger keeps it), items that do not stack are left
// alone, and the size is capped at kMaxStackCap.
//
// Both members (keys below) are reflected: the loader reads their offsets from
// the running game's own reflection data, by class and member name, so a build
// with no version-map row for them still resolves. Resolve is false when the
// loader cannot give either member at the size compiled here. The "does not
// stack" value is read by name from the game's ENxItemStackType enum, once;
// when that lookup misses, Apply raises nothing (StackTypeMissed() is then
// true: log it once).
//
// Needs plugin interface 73: plugin_map.h, which this header includes, calls
// the v73 DeclareFeature, and the example below uses it. Game thread only:
// call everything from Tick. Items are held as ObjectRef, checked before every
// write, so an item the game has freed is dropped and never written to.
//
//   ItemStackLimits::Limits g_stacks;
//   static PluginMap::Feature s_stacks{ "stack-limits", "Item stack sizes",
//       ItemStackLimits::Keys(), {} };
//   // OnPluginLoadHooks: declare the feature first, then resolve. A miss then
//   // turns off this part only, not the whole plugin.
//   PluginMap::DeclareFeature(scanner, self, s_stacks);
//   ItemStackLimits::Resolve(scanner, self);
//   // Once, on a Tick after the items are loaded:
//   g_stacks.SetItems(loadedItems);            // std::vector<SDK::UAuItemDataBase*>
//   g_stacks.SetTarget(500);                   // 0 = leave the game's own sizes
//   // Every Tick. On() also turns false while a server's rules deny the
//   // feature, so the sizes go back then:
//   if (s_stacks.On())
//       g_stacks.Apply();                      // cheap, only writes a change
//   else
//       g_stacks.RestoreAll();
//   // In Shutdown (on the game thread):
//   g_stacks.RestoreAll();
//
// With a key unresolved, Limits does nothing (Apply returns 0, Available() is
// false).
namespace ItemStackLimits
{
    constexpr int kMaxStackCap = 9999;
    constexpr int kMinStack    = 1;

    // The stack size an item gets: the size asked for, kept within
    // kMinStack..kMaxStackCap, but never below the item's own vanilla size.
    inline int Compose(int vanilla, int size)
    {
        return (std::max)(vanilla, (std::min)((std::max)(size, kMinStack), kMaxStackCap));
    }

    inline PluginField<int32_t>& MaxStackField()
    {
        static PluginField<int32_t> field = SR_FIELD(int32_t, UAuItemDataBase, MaxStack, 0);
        return field;
    }

    inline PluginField<SDK::ENxItemStackType>& StackingTypeField()
    {
        static PluginField<SDK::ENxItemStackType> field = SR_FIELD(SDK::ENxItemStackType, UAuItemDataBase, StackingType, 0);
        return field;
    }

    // The two map keys, for a PluginMap::Feature.
    inline std::vector<const char*> Keys()
    {
        return { MaxStackField().key, StackingTypeField().key };
    }

    // Asks the loader for both members. False when either did not resolve on
    // this build (the loader has recorded it: list Keys() in a feature).
    inline bool Resolve(IPluginHookScanner* scanner, const IPluginSelf* self)
    {
        return PluginMap::ResolveFields(scanner, self, MaxStackField(), StackingTypeField());
    }

    inline bool Available()
    {
        return MaxStackField().offset >= 0 && StackingTypeField().offset >= 0;
    }

    class Limits
    {
    public:
        // The items to manage (every item data asset you found). Replaces the
        // list: an item already touched keeps its recorded vanilla value, and
        // one dropped from the list is restored first. An item listed more than
        // once is tracked once. Live items only.
        void SetItems(const std::vector<SDK::UAuItemDataBase*>& items)
        {
            std::vector<Entry> next;
            next.reserve(items.size());
            std::unordered_set<const SDK::UAuItemDataBase*> seen;
            for (SDK::UAuItemDataBase* item : items)
            {
                if (!item || !seen.insert(item).second)
                    continue;
                Entry e;
                e.item.Set(item);
                next.push_back(e);
            }
            for (Entry& old : m_entries)
            {
                if (!old.touched)
                    continue;
                bool kept = false;
                for (Entry& e : next)
                    if (e.item == old.item) { e = old; kept = true; break; }
                if (!kept)
                    Restore(old);
            }
            m_entries = std::move(next);
        }

        // The size to apply. 0 (or less) means none: Apply then restores.
        void SetTarget(int size) { m_target = size; }
        int  Target() const { return m_target; }

        // Writes the composed size to each live item whose value differs and
        // returns how many it changed. A target of 0 restores instead. An item
        // that does not stack, or whose reference is dead, is skipped.
        int Apply()
        {
            if (!Available())
                return 0;
            if (m_target <= 0)
                return RestoreAll();

            int changed = 0;
            for (Entry& e : m_entries)
            {
                SDK::UAuItemDataBase* item = e.item.Get();
                if (!item)
                {
                    e.touched = false;   // freed with its value: nothing to give back
                    continue;
                }
                int32_t* maxStack = MaxStackField().In(item);
                const SDK::ENxItemStackType* type = StackingTypeField().In(item);
                if (!maxStack || !type || !LookUpDoNotStack() || static_cast<int64_t>(*type) == m_doNotStack)
                    continue;

                if (!e.touched)
                {
                    e.vanilla = *maxStack;
                    e.touched = true;
                }
                const int want = Compose(e.vanilla, m_target);
                if (*maxStack != want)
                {
                    *maxStack = want;
                    ++changed;
                }
            }
            return changed;
        }

        // Puts every touched live item back to its recorded vanilla stack and
        // returns how many it changed. Call when the feature turns off and in
        // Shutdown.
        int RestoreAll()
        {
            int changed = 0;
            for (Entry& e : m_entries)
                if (Restore(e))
                    ++changed;
            return changed;
        }

        // True once the game's "does not stack" value could not be read: Apply
        // then raises nothing.
        bool StackTypeMissed() const { return m_askedDoNotStack && !m_foundDoNotStack; }

        // For a log line.
        int Tracked() const { return static_cast<int>(m_entries.size()); }
        int Raised() const
        {
            int n = 0;
            for (const Entry& e : m_entries)
                if (e.touched && e.item.Get()) { const int32_t* v = MaxStackField().In(e.item.Get()); if (v && *v != e.vanilla) ++n; }
            return n;
        }

    private:
        struct Entry
        {
            ObjectRef<SDK::UAuItemDataBase> item;
            int32_t vanilla = 1;
            bool    touched = false;
        };

        // Asked once, on the first live item: the lookup walks every object.
        bool LookUpDoNotStack()
        {
            if (!m_askedDoNotStack)
            {
                m_askedDoNotStack = true;
                m_foundDoNotStack = PluginMap::EnumValue("ENxItemStackType", "DoNotStack", m_doNotStack)
                    == PluginMap::EnumLookup::Found;
            }
            return m_foundDoNotStack;
        }

        static bool Restore(Entry& e)
        {
            if (!e.touched)
                return false;
            e.touched = false;
            SDK::UAuItemDataBase* item = e.item.Get();
            int32_t* maxStack = item ? MaxStackField().In(item) : nullptr;
            if (!maxStack || *maxStack == e.vanilla)
                return false;
            *maxStack = e.vanilla;
            return true;
        }

        std::vector<Entry> m_entries;
        int                m_target = 0;
        int64_t            m_doNotStack = 0;
        bool               m_askedDoNotStack = false;
        bool               m_foundDoNotStack = false;
    };
}
