#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class KeyValue;

// EquipmentSnapshot (research/equipment_sync_phase0_type43.md): the BASE weapons a player picked in its loadout (USP-S instead of
// P2000, M4A1-S instead of M4A4, ...), as the Java backend and the game server GC exchange it (JSON, snake_case). On the game server
// each entry becomes one CSOEconDefaultEquippedDefinitionInstanceClient (SO type 43) inside the k_ESOMsg_CacheSubscribed that Skin Sync
// already posts -- live-proven to make server.dll hand out the chosen weapon. Plain data, no protobuf and no Steam, like skin_snapshot.h:
// the same code runs in the DLL and in the offline tests (offline_tests/skinsync). Skins, knives, gloves ... are NOT part of it
// (an inventory item with a skin travels in the skin snapshot).
//
// First production version: class 2 (T) / 3 (CT), pistol slots 2..7, rifle slots 14..19.
namespace EquipmentSync
{

constexpr size_t MaxEntries = 24;       // 2 teams x (6 pistol + 6 rifle slots)
constexpr uint32_t ClassT = 2;
constexpr uint32_t ClassCT = 3;

struct Entry
{
    uint32_t defIndex{};    // item_definition of the chosen weapon (items_game.txt "items")
    uint32_t classId{};     // 2 = T, 3 = CT
    uint32_t slotId{};      // items_game.txt "player_loadout_slots": 2..7 secondary0..5, 14..19 rifle0..5
};

struct PlayerSnapshot
{
    uint32_t accountId{};
    uint64_t steamId64{};
    std::vector<Entry> entries;
};

// the snapshots of the REAL players of one match as the game server received them (GET /servers/roster -> equipment_snapshots)
struct MatchSnapshots
{
    std::string matchId;
    std::vector<PlayerSnapshot> players;
    std::vector<uint32_t> missing;      // real players without a snapshot: the mode's normal base loadout applies to them
};

// ---- the rules of items_game.txt, in a compact table (built once, the KeyValue tree is not kept) ----

class LoadoutTable
{
public:
    // reads items_game.txt's "items", "prefabs" and "player_loadout_slots"; false when they are not there
    bool Load(const KeyValue &itemsGame);

    // A legal base-weapon choice: the item exists, is a base item, class is 2 / 3, the slot is a pistol or rifle slot, the
    // item's item_sub_position is exactly that slot and the item is usable by that team (prefab chain included).
    // Knives, grenades, gloves, skinned (non-base) items and anything unknown are refused. `why` names the reason.
    bool Legal(const Entry &entry, std::string &why) const;

    size_t ItemCount() const { return m_items.size(); }

    // the table of the game's own csgo/scripts/items/items_game.txt (relative to the working directory of the game / srcds, like
    // ItemSchema); loaded on first use. nullptr when the file cannot be read -- nothing is then validated and nothing applied.
    static const LoadoutTable *Shared();

private:
    struct Info
    {
        bool baseItem{};
        int slot{ -1 };
        bool terrorists{};
        bool counterTerrorists{};
    };

    std::map<uint32_t, Info> m_items;
};

// ---- the result of checking a list of entries ----

struct Checked
{
    std::vector<Entry> accepted;
    std::vector<std::string> rejected;      // "def=61 class=2 slot=2 : reason" for every entry that was not accepted
};

// Every entry must be legal; a (class, slot) that appears more than once is ambiguous and none of its entries is accepted; at most
// MaxEntries entries are looked at. The order of the accepted entries is the order given.
Checked Check(const std::vector<Entry> &entries, const LoadoutTable &table);

// ---- client side: POST /api/v1/matchmaking/equipment-snapshot ----

// {"account_id":N,"request_id":"...","entries":[...]}  (at most MaxEntries entries are written)
std::string BuildSnapshotBody(uint32_t accountId, const std::string &requestId, const std::vector<Entry> &entries);

// "[{...},{...}]" (also the form the backend puts into GET /servers/roster)
void AppendEntriesJson(std::string &out, const std::vector<Entry> &entries);

// ---- server side: GET /servers/roster -> equipment_snapshots[] / equipment_missing[] ----

// everything that makes two MatchSnapshots different: the poller reports only changes
std::string Signature(const MatchSnapshots &snapshots);

// What the game server applies for one connecting player. The player is the one BeginAuthSession accepted: account = low 32 bits of
// the SteamID64, matched by account id AND (when the backend sent it) the SteamID64 -- never by position, name or entity index.
// Entries are re-checked against items_game.txt here: the backend and the client are not trusted.
struct ApplyPlan
{
    bool playerMatched{};
    uint32_t accountId{};
    uint64_t steamId64{};
    std::string reason;                     // why nothing is applied, empty when something is
    std::vector<Entry> entries;             // accepted entries
    std::vector<std::string> rejected;
};

// `table` may be nullptr (items_game.txt unreadable): the player is matched but nothing is accepted
ApplyPlan PlanForPlayer(const MatchSnapshots &snapshots, uint64_t steamId64, const LoadoutTable *table);

// ---- logging: "def=61 class=3 slot=2" ----
std::string Describe(const Entry &entry);

} // namespace EquipmentSync
