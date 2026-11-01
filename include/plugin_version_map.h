#pragma once

// plugin_map.h, plus this plugin's own version-map rows: locators and member
// offsets per game build that it carries itself, for keys the loader's map
// does not have yet (plugin interface 72).
//
// Include it after plugin_interface.h, the Game-SDK headers and the plugin's own
// version_map.generated.h (plugin flavour, written by starrupture-ci
// ci/version-map.mjs), whose VersionMap::kBuiltCL, kFunctions, kFunctionRows,
// kFields and kFieldRows it reads.
//
// In OnPluginLoadHooks:
//   PluginScanRequest req = PluginMap::Request("UCrCrafter::FinishCrafting", pattern, PLUGIN_SCAN_FUNCTION_START);
//   PluginVersionMap::Attach(req);                     // this key's own rows, if any
//   scanner->Resolve(self, &req);
//   PluginVersionMap::ResolveFields(scanner, self, g_craftComponent, g_isOn);
// PluginInit as in plugin_map.h.

#include "plugin_map.h"

#include <array>

static_assert(PLUGIN_GAME_BUILT_CL == 0 || VersionMap::kBuiltCL == PLUGIN_GAME_BUILT_CL,
    "version_map.generated.h is for another game build than the Game-SDK (build_cl.txt): regenerate it");

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

    // Gives the request this plugin's locators for its hookName on every build
    // in its map. A key with no rows leaves the request as it is.
    inline void Attach(PluginScanRequest& request)
    {
        if (const auto* entry = Find(VersionMap::kFunctions, request.hookName))
        {
            request.candidates     = VersionMap::kFunctionRows.data() + entry->first;
            request.candidateCount = entry->count;
        }
    }

    // PluginMap::ResolveField with this plugin's rows for the field's key and
    // its map's kBuiltCL. False = the loader recorded a failure (which refuses
    // the plugin).
    inline bool ResolveField(IPluginHookScanner* scanner, const IPluginSelf* self, PluginFieldSpec& field)
    {
        const PluginFieldCandidate* rows = nullptr;
        uint32_t count = 0;
        if (const auto* entry = Find(VersionMap::kFields, field.key))
        {
            rows  = VersionMap::kFieldRows.data() + entry->first;
            count = entry->count;
        }
        return PluginMap::ResolveField(scanner, self, field, rows, count, VersionMap::kBuiltCL);
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
