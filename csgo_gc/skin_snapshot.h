#pragma once

#include <cstdint>
#include <string>
#include <vector>

// EquippedSkinSnapshot (research/backend_skin_sync_design.md): what a player has equipped, as the Java backend and the game
// server GC exchange it (JSON, snake_case). Plain data, no protobuf and no Steam: the same code runs in the DLL and in the
// offline tests (offline_tests/skinsync). Only EQUIPPED items are ever part of a snapshot.
namespace SkinSync
{

constexpr size_t MaxItems = 64;       // the game server side limit of the project (gc_server.cpp MaxServerSOCacheItems)
constexpr size_t MaxStickers = 6;

struct Sticker
{
    uint32_t slot{};
    uint32_t id{};
    bool hasWear{};
    float wear{};
};

struct Equipped
{
    uint32_t classId{};   // CSOEconItemEquipped.new_class (2 = T, 3 = CT, 0 = any)
    uint32_t slotId{};
};

struct Item
{
    uint64_t itemId{};
    uint32_t defIndex{};
    uint32_t quality{};
    uint32_t rarity{};

    bool hasPaint{};          // attributes 6 / 7 / 8
    uint32_t paintKit{};
    uint32_t paintSeed{};
    float paintWear{};

    bool hasStatTrak{};       // attributes 80 / 81
    uint32_t statTrakCount{};
    uint32_t statTrakScoreType{};

    std::string customName;
    std::vector<Sticker> stickers;
    std::vector<Equipped> equipped;
};

struct PlayerSnapshot
{
    uint32_t accountId{};
    uint64_t steamId64{};
    std::vector<Item> items;
};

// ---- client side: POST /api/v1/matchmaking/skin-snapshot ----

// {"account_id":N,"request_id":"...","items":[...]}  (at most MaxItems items are written)
std::string BuildSnapshotBody(uint32_t accountId, const std::string &requestId, const std::vector<Item> &items);

// "[{...},{...}]" (also the form the backend puts into GET /servers/roster)
void AppendItemsJson(std::string &out, const std::vector<Item> &items);

// ---- server side: GET /servers/roster -> skin_snapshots[] / skin_missing[] ----

// the snapshots of the REAL players of one match as the game server received them
struct MatchSnapshots
{
    std::string matchId;
    std::vector<PlayerSnapshot> players;
    std::vector<uint32_t> missing;     // real players of the match without a snapshot (no custom skins for them)
};

// everything that makes two MatchSnapshots different (match, players, every item field): the poller reports only changes
std::string Signature(const MatchSnapshots &snapshots);

// ---- server side, Phase D: which snapshot belongs to the player that connects, and what of it is applied ----

// What the game server applies for one connecting player. Pure data (no protobuf): the player matching, the item checks and the
// equipped-slot / paint extraction live here so the offline test can run them; Inventory::BuildServerCache turns the plan into
// the SOCache the game (server.dll) reads its items from.
struct ApplyPlan
{
    bool playerMatched{};          // the snapshot of this very player is there
    uint32_t accountId{};
    uint64_t steamId64{};
    std::string reason;            // why nothing is applied (no snapshot, ...), empty when something is
    std::vector<Item> items;       // accepted items: equipped, own, sane, at most MaxItems
    std::vector<std::string> rejected; // "item=... : reason" for every item that was not accepted
};

// The player that connected as `steamId64` (BeginAuthSession): account = low 32 bits, never an index of the roster. The snapshot
// is matched by account id AND, when the backend sent it, by SteamID64. An item is accepted only if it is equipped (the slot comes
// from the snapshot), carries a def_index, belongs to the account (Inventory ComposeItemId: low 32 bits = account id, not a default
// item id), and its paint wear is within 0..1.
ApplyPlan PlanForPlayer(const MatchSnapshots &snapshots, uint64_t steamId64);

// ---- logging: "item=2 def=507 paint=38 seed=41 wear=0.000001" (safe identifiers only) ----
std::string Describe(const Item &item);

} // namespace SkinSync
