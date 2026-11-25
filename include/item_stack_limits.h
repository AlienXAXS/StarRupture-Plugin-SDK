#pragma once
#include <plugin_interface.h>
#include <AuItems_classes.hpp>
#include <plugin_map.h>
#include <object_ref.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// Apply and restore item stack sizes.
//
// Raises UAuItemDataBase::MaxStack on the items you hand it and puts every
// value back to vanilla on request. A size only ever raises an item (one whose
// own stack is already bigger keeps it), items that do not stack are left
// alone, and the size is capped at kMaxStackCap. The member offsets come from
// the loader's version map (keys below), so nothing is compiled in.
//
// Needs plugin interface 73 (PluginMap::ResolveField). Game thread only:
// call everything from Tick. Items are held as ObjectRef, checked before every
// write, so an item the game has freed is dropped and never written to.
//
//   ItemStackLimits::Limits g_stacks;
//   // OnPluginLoadHooks, with the other resolves:
//   ItemStackLimits::Resolve(scanner, self);   // keys: ItemStackLimits::Keys()
//   // Tick, once the items are loaded:
//   g_stacks.SetItems(loadedItems);            // std::vector<SDK::UAuItemDataBase*>
//   g_stacks.SetTarget(500);                   // 0 = leave the game's own sizes
//   g_stacks.Apply();                          // every tick: cheap, only writes a change
//   // when the feature turns off, and in Shutdown (on the game thread):
//   g_stacks.RestoreAll();
//
// If either key is missing on a game build, Resolve returns false and
// Limits does nothing (Apply returns 0, Available() is false): list Keys() in
// a PluginMap::Feature so the plugin keeps running with this part off.
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
        // one dropped from the list is restored first. Live items only.
        void SetItems(const std::vector<SDK::UAuItemDataBase*>& items)
        {
            std::vector<Entry> next;
            next.reserve(items.size());
            for (SDK::UAuItemDataBase* item : items)
            {
                if (!item)
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
                if (!maxStack || !type || *type == SDK::ENxItemStackType::DoNotStack)
                    continue;

                if (!e.touched)
                {
                    e.vanilla = *maxStack > 0 ? *maxStack : 1;
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
    };
}
