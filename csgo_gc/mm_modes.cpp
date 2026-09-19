#include "stdafx.h"
#include <algorithm>

#include "mm_modes.h"
#include "keyvalue.h"

namespace MM
{

namespace
{

// requiredPlayers: Competitive 10 / Wingman 4 / Danger Zone 16 / ScrimComp5v5 10 (5v5). Cooperative is left at 0:
// its real human count is not confirmed (gamemodes.txt maxplayers 20/10 are server slots, not matchmaking players),
// so it stays unsupported instead of guessing.
const GameMode s_modes[] = {
    // gameType name            accept requiredPlayers serverGameType   serverGameMode        maxPl mapTable                   defaultMap      supported
    { 4,  "armsrace",     false, 0,  "gungame",      "gungameprogressive", 10, MapTable::ArmsRace,         "ar_shoots",     true  },
    { 5,  "demolition",   false, 0,  "gungame",      "gungametrbomb",      10, MapTable::Demolition,       "de_bank",       true  },
    { 6,  "deathmatch",   false, 0,  "gungame",      "deathmatch",         16, MapTable::CasualDeathmatch, "de_dust2",      true  },
    { 7,  "casual",       false, 0,  "classic",      "casual",             20, MapTable::CasualDeathmatch, "de_dust2",      true  },
    { 8,  "competitive",  true,  10, "classic",      "competitive",        10, MapTable::Competitive,      "de_dust2",      true  },
    { 9,  "cooperative",  true,  0,  "cooperative",  "cooperative",        20, MapTable::Cooperative,      "",              false },
    { 10, "wingman",      true,  4,  "classic",      "scrimcomp2v2",       4,  MapTable::Wingman,          "de_lake",       true  },
    { 11, "scrimcomp5v5", true,  10, "classic",      "scrimcomp5v5",       10, MapTable::Competitive,      "de_dust2",      true  },
    { 12, "skirmish",     false, 0,  "skirmish",     "skirmish",           12, MapTable::Skirmish,         "de_dust2",      true  },
    { 13, "dangerzone",   true,  16, "freeforall",   "survival",           16, MapTable::DangerZone,       "dz_blacksite",  true  },
};

struct MapBit
{
    uint32_t bit;
    const char *map;
};

// bit -> map, in the order the client composer (sub_10288A90) lists them; the group tokens (mg_casualdelta,
// mg_hostage, ...) are plain unions of these bits, so decoding by single bits covers them too.
const MapBit s_casualDeathmatch[] = {
    { 0x2, "de_dust2" }, { 0x80, "de_mirage" }, { 0x10, "de_inferno" }, { 0x1000, "de_cache" },
    { 0x200000, "de_cbble" }, { 0x40, "de_vertigo" }, { 0x4, "de_train" }, { 0x100000, "de_overpass" },
    { 0x20, "de_nuke" }, { 0x400000, "de_canals" }, { 0x80000, "cs_agency" }, { 0x100, "cs_office" },
    { 0x200, "cs_italy" }, { 0x400, "cs_assault" }, { 0x800, "cs_militia" }, { 0x8, "de_ancient" },
    { 0x40000, "de_basalt" }, { 0x10000, "cs_insertion2" }, { 0x2000, "de_grind" }, { 0x4000, "de_mocha" },
};

// competitive / scrimcomp5v5 use the same bits as casual/deathmatch, the extra 0x20000 is
// mg_lobby_mapveto (a flag, not a map)
constexpr uint32_t CompetitiveMapVetoFlag = 0x20000;

const MapBit s_wingman[] = {
    { 0x40, "de_vertigo" }, { 0x10, "de_inferno" }, { 0x200000, "de_cbble" }, { 0x100000, "de_overpass" },
    { 0x4, "de_train" }, { 0x20, "de_shortnuke" }, { 0x8000, "de_shortdust" }, { 0x80, "gd_rialto" },
    { 0x8, "de_lake" }, { 0x10000, "de_guard" }, { 0x40000, "de_elysion" }, { 0x100, "de_calavera" },
    { 0x200, "de_pitstop" }, { 0x400, "de_ravine" }, { 0x800, "de_extraction" },
};

const MapBit s_dangerZone[] = {
    { 0x1, "dz_blacksite" }, { 0x2, "dz_sirocco" }, { 0x10, "dz_county" },
};

const MapBit s_armsRace[] = {
    { 0x1, "ar_shoots" }, { 0x2, "ar_baggage" }, { 0x4, "ar_monastery" }, { 0x8, "de_lake" },
    { 0x2000, "de_stmarc" }, { 0x20, "de_safehouse" }, { 0x80, "ar_lunacy" },
};

const MapBit s_demolition[] = {
    { 0x1, "de_bank" }, { 0x4, "de_sugarcane" }, { 0x8, "de_lake" }, { 0x2000, "de_stmarc" },
    { 0x20, "de_safehouse" }, { 0x8000, "de_shortdust" },
};

template<size_t N>
void Decode(const MapBit (&table)[N], MapSelection &selection)
{
    selection.decoded = true;
    for (const MapBit &entry : table)
    {
        if ((selection.mask & entry.bit) == entry.bit)
        {
            selection.maps.push_back(entry.map);
        }
    }
}

} // namespace

const GameMode *FindGameMode(uint32_t eGame)
{
    for (const GameMode &mode : s_modes)
    {
        if (mode.gameType == eGame)
        {
            return &mode;
        }
    }

    return nullptr;
}

const GameMode *FindGameModeByName(std::string_view name)
{
    for (const GameMode &mode : s_modes)
    {
        if (name == mode.name)
        {
            return &mode;
        }
    }

    return nullptr;
}

MapSelection DecodeMapSelection(const GameMode &mode, uint32_t gameType)
{
    MapSelection selection;
    selection.mask = (gameType >> 8) & 0xFFFFFFu;

    switch (mode.mapTable)
    {
    case MapTable::CasualDeathmatch:
        Decode(s_casualDeathmatch, selection);
        break;

    case MapTable::Competitive:
        selection.flags = selection.mask & CompetitiveMapVetoFlag;
        selection.mask &= ~CompetitiveMapVetoFlag;
        Decode(s_casualDeathmatch, selection);
        break;

    case MapTable::Wingman:
        Decode(s_wingman, selection);
        break;

    case MapTable::DangerZone:
        Decode(s_dangerZone, selection);
        break;

    case MapTable::ArmsRace:
        Decode(s_armsRace, selection);
        break;

    case MapTable::Demolition:
        Decode(s_demolition, selection);
        break;

    case MapTable::Skirmish: // mask bits are skirmish modes (1 << (id - 1)), see DecodeSkirmishSelection
    case MapTable::Cooperative: // mask is the quest id
    case MapTable::None:
        break;
    }

    return selection;
}

std::vector<SkirmishMode> ParseSkirmishModes(const KeyValue *skirmishModesKey)
{
    std::vector<SkirmishMode> modes;
    if (!skirmishModesKey)
    {
        return modes;
    }

    for (const KeyValue &entry : *skirmishModesKey)
    {
        SkirmishMode mode;
        const std::string_view id = entry.Name();
        const std::from_chars_result parsed = std::from_chars(id.data(), id.data() + id.size(), mode.id);
        // the mask has 24 bits, so ids 1..24 can be selected
        if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || mode.id < 1 || mode.id > 24)
        {
            continue;
        }

        mode.name = std::string(entry.GetString("name"));
        mode.gameMode = std::string(entry.GetString("gamemode"));
        if (!mode.name.empty())
        {
            modes.push_back(std::move(mode));
        }
    }

    std::sort(modes.begin(), modes.end(), [](const SkirmishMode &a, const SkirmishMode &b) { return a.id < b.id; });
    return modes;
}

std::vector<std::string> SkirmishMapGroupMaps(const KeyValue *gamemodes, std::string_view modeName)
{
    std::vector<std::string> maps;
    if (!gamemodes || gamemodes->SubkeyCount() == 0)
    {
        return maps;
    }

    const KeyValue *groups = gamemodes->begin()->GetSubkey("mapgroups");
    if (!groups)
    {
        return maps;
    }

    const std::string groupName = "mg_skirmish_" + std::string(modeName);
    const KeyValue *group = groups->GetSubkey(groupName);
    const KeyValue *groupMaps = group ? group->GetSubkey("maps") : nullptr;
    if (groupMaps)
    {
        for (const KeyValue &map : *groupMaps)
        {
            maps.emplace_back(map.Name());
        }
    }

    return maps;
}

std::vector<SkirmishVariant> DecodeSkirmishSelection(uint32_t mask, const std::vector<SkirmishMode> &modes,
    const KeyValue *gamemodes, uint32_t *unknownBits)
{
    std::vector<SkirmishVariant> variants;
    uint32_t known = 0;
    for (const SkirmishMode &mode : modes)
    {
        const uint32_t bit = 1u << (mode.id - 1);
        known |= bit;
        if (mask & bit)
        {
            variants.push_back({ mode.name, mode.gameMode, SkirmishMapGroupMaps(gamemodes, mode.name) });
        }
    }

    if (unknownBits)
    {
        *unknownBits = mask & ~known;
    }

    return variants;
}

std::string FormatSkirmish(const std::vector<SkirmishVariant> &variants)
{
    std::string result;
    for (const SkirmishVariant &variant : variants)
    {
        if (!result.empty())
        {
            result += ",";
        }

        result += variant.name + "(" + std::to_string(variant.maps.size()) + " maps)";
    }

    return result.empty() ? "(none)" : result;
}

std::string FormatMaps(const MapSelection &selection)
{
    if (selection.maps.empty())
    {
        return "(none)";
    }

    std::string result;
    for (const std::string &map : selection.maps)
    {
        if (!result.empty())
        {
            result += ",";
        }

        result += map;
    }

    return result;
}

std::string PickMap(const GameMode &mode, const MapSelection &selection)
{
    if (!selection.maps.empty())
    {
        return selection.maps.front();
    }

    return mode.defaultMap;
}

} // namespace MM
