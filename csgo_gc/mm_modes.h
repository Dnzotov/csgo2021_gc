#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class KeyValue;

// Central table of the CS:GO 2021 matchmaking game modes (eGame = MatchmakingStart.game_type & 0xF) and the
// decoding of the map selection the client packs into the same field: game_type = eGame | (mapMask << 8).
// Everything is taken from confirmed data, see RESEARCH_FINDINGS.md #38.4/#41.4 (accept modes, client.dll
// sub_103EF770), #44.5 (map tables, client.dll sub_10288A90) and csgo/gamemodes.txt (server modes, maxplayers).
//
// Four different numbers must not be confused:
//   * serverMaxPlayers  -- srcds maxplayers of the mode (gamemodes.txt), a server property
//   * requiredPlayers   -- humans the matchmaking has to gather before a match may start (accept modes only)
//   * roster size       -- number of AccountID tokens in the queued ('Q') reservation, == requiredPlayers
//   * accepted players  -- how many roster entries reached reservation stage 2 (the 0x25 total - awaiting)
//
// TEST ONLY interim home of this logic: it moves to the Java matchmaking backend (see #44/#45), the GC keeps
// only the native client integration.
namespace MM
{

// which client-side map-token table (client.dll sub_10288A90 switch group) a mode uses
enum class MapTable
{
    None,             // no decodable selection
    CasualDeathmatch, // case 6/7: shared table
    Competitive,      // case 8/0xB: shared table (+ mapveto flag)
    Wingman,          // case 0xA
    DangerZone,       // case 0xD
    ArmsRace,         // case 4
    Demolition,       // case 5
    Skirmish,         // case 0xC: mask = selected skirmish MODES (1 << (id - 1), ids of items_game.txt), not maps
    Cooperative,      // case 9: mask = quest id
};

struct GameMode
{
    uint32_t gameType;         // eGame
    const char *name;          // log / config name
    bool acceptRequired;       // client.dll sub_103EF770: {8,9,10,11,13}
    uint32_t requiredPlayers;  // accept modes: roster size (0 = not applicable / unverified)
    const char *serverGameType; // srcds "game_type" of the mode (client.dll QueueConnect KV switch / gamemodes.txt)
    const char *serverGameMode; // srcds "game_mode"
    uint32_t serverMaxPlayers;  // gamemodes.txt maxplayers, reference only
    MapTable mapTable;
    const char *defaultMap;     // used when the selection can not be decoded
    bool supported;             // handled by the current GC test flow
};

const GameMode *FindGameMode(uint32_t eGame);
const GameMode *FindGameModeByName(std::string_view name);

struct MapSelection
{
    uint32_t mask{};            // (game_type >> 8) & 0xFFFFFF
    uint32_t flags{};           // non-map bits of the mask (competitive mapveto)
    bool decoded{};             // false: the table does not describe maps for this mode
    std::vector<std::string> maps;
};

MapSelection DecodeMapSelection(const GameMode &mode, uint32_t gameType);

// ---- Skirmish ("War Games"): Arms Race, Demolition, Flying Scoutsman, Retakes, ... (RESEARCH_FINDINGS.md #54) ----
// The client does not select maps there but skirmish modes: for every ticked mode the composer (client.dll
// sub_10288A90 case 0xC) ORs 1 << (id - 1) into the mask, id = the "skirmish_modes" entry of items_game.txt whose name
// is the part of the map group token after "mg_skirmish_" (armsrace = 10 -> 0x200, demolition = 11 -> 0x400, retakes =
// 12 -> 0x800, ...). In the current UI Arms Race and Demolition exist ONLY in this form: the client never sends
// eGame 4 / 5 for them. The maps of a mode are the map group mg_skirmish_<name> of csgo/gamemodes.txt.
struct SkirmishMode
{
    uint32_t id{};
    std::string name;       // "armsrace", "demolition", "retakes", ...
    std::string gameMode;   // srcds game_mode of the mode: "gungameprogressive", "gungametrbomb", "casual", ...
};

struct SkirmishVariant
{
    std::string name;
    std::string gameMode;
    std::vector<std::string> maps; // the maps of mg_skirmish_<name>, empty if gamemodes.txt could not be read
};

// items_game.txt "skirmish_modes" (children named by id, keys "name" / "gamemode")
std::vector<SkirmishMode> ParseSkirmishModes(const KeyValue *skirmishModesKey);

// the maps of the group mg_skirmish_<modeName>: csgo/gamemodes.txt (parsed root, its first child is the file table)
// -> "mapgroups" -> group -> "maps". Empty if there is no such group.
std::vector<std::string> SkirmishMapGroupMaps(const KeyValue *gamemodes, std::string_view modeName);

// the modes selected by a mask, in id order. unknownBits (optional) receives the mask bits no known mode owns.
std::vector<SkirmishVariant> DecodeSkirmishSelection(uint32_t mask, const std::vector<SkirmishMode> &modes,
    const KeyValue *gamemodes, uint32_t *unknownBits = nullptr);

// "armsrace(7 maps),demolition(6 maps)"
std::string FormatSkirmish(const std::vector<SkirmishVariant> &variants);

// "de_mirage,de_inferno" / "(none)"
std::string FormatMaps(const MapSelection &selection);

// the map to advertise in the 9107 for a selection: the first selected map in table order, else the mode default
std::string PickMap(const GameMode &mode, const MapSelection &selection);

} // namespace MM
