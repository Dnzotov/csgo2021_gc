// Offline test of the client / server halves of the skin sync (research/backend_skin_sync_design.md):
//   client snapshot -> serialized payload (the body of POST /matchmaking/skin-snapshot)
//   serialized items -> the object the game server GC works with (the body of GET /servers/roster -> skin_snapshots)
// The REAL skin_snapshot.cpp and backend_client.cpp (parser); no network, no game, no protobuf.
#include "stdafx.h"
#include "config.h"
#include "backend_client.h"
#include "skin_snapshot.h"

#include <cstdarg>
#include <cmath>

void Platform::Print(const char *format, ...)
{
    va_list ap; va_start(ap, format); printf("      | "); vprintf(format, ap); va_end(ap);
}
std::string Platform::CommandLine() { return {}; }
const GCConfig &GetConfig() { static GCConfig c; return c; }
GCConfig::GCConfig() {}

static int g_failed = 0;
static void Expect(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

using namespace SkinSync;

// the test item of the project: item 2 (high id 2), Karambit (507), paint 38, seed 41, wear 0.000001, CT knife slot
static Item KnownItem()
{
    Item item;
    item.itemId = (2ull << 32) | 1050166997ull;   // Inventory ComposeItemId(account 1050166997, high id 2): the "item 2" of inventory.txt
    item.defIndex = 507;
    item.quality = 99;
    item.rarity = 6;
    item.hasPaint = true;
    item.paintKit = 38;
    item.paintSeed = 41;
    item.paintWear = 0.000001f;
    item.equipped.push_back({ 3, 0 });
    return item;
}

// every field set: stattrak, a custom name that needs escaping, two stickers, two slots
static Item RichItem()
{
    Item item = KnownItem();
    item.itemId = 5000000000ull;      // above 2^32
    item.defIndex = 7;
    item.hasStatTrak = true;
    item.statTrakCount = 1234;
    item.statTrakScoreType = 0;
    item.customName = "My \"best\" \\ rifle";
    item.stickers.push_back({ 0, 76, true, 0.25f });
    item.stickers.push_back({ 3, 1209, false, 0.0f });
    item.equipped.push_back({ 2, 0 });
    return item;
}

static bool SameItem(const Item &a, const Item &b)
{
    if (a.itemId != b.itemId || a.defIndex != b.defIndex || a.quality != b.quality || a.rarity != b.rarity
        || a.hasPaint != b.hasPaint || a.hasStatTrak != b.hasStatTrak || a.customName != b.customName
        || a.stickers.size() != b.stickers.size() || a.equipped.size() != b.equipped.size())
    {
        return false;
    }
    if (a.hasPaint && (a.paintKit != b.paintKit || a.paintSeed != b.paintSeed || a.paintWear != b.paintWear))
    {
        return false;
    }
    if (a.hasStatTrak && (a.statTrakCount != b.statTrakCount || a.statTrakScoreType != b.statTrakScoreType))
    {
        return false;
    }
    for (size_t i = 0; i < a.stickers.size(); i++)
    {
        if (a.stickers[i].slot != b.stickers[i].slot || a.stickers[i].id != b.stickers[i].id
            || a.stickers[i].hasWear != b.stickers[i].hasWear || (a.stickers[i].hasWear && a.stickers[i].wear != b.stickers[i].wear))
        {
            return false;
        }
    }
    for (size_t i = 0; i < a.equipped.size(); i++)
    {
        if (a.equipped[i].classId != b.equipped[i].classId || a.equipped[i].slotId != b.equipped[i].slotId)
        {
            return false;
        }
    }
    return true;
}

// the roster body the backend answers with, for the given items of one real player
static std::string RosterBody(const std::vector<Item> &items, const char *missing = "[1000]")
{
    std::string body = "{\"match_id\":\"m-test\",\"mode\":\"competitive\",\"status\":\"READY\",\"map\":\"de_vertigo\","
        "\"accept_required\":true,\"required_players\":3,\"players\":[{\"account_id\":1050166997,\"fake\":false},"
        "{\"account_id\":1000,\"fake\":false},{\"account_id\":4199878657,\"fake\":true}],"
        "\"skin_snapshots\":[{\"account_id\":1050166997,\"steam_id64\":\"76561199010432725\","
        "\"received_at\":\"2026-10-08T17:52:15Z\",\"items\":";
    AppendItemsJson(body, items);
    body += "}],\"skin_missing\":";
    body += missing;
    body += "}";
    return body;
}

int main()
{
    printf("client snapshot -> payload\n");
    {
        const std::string body = BuildSnapshotBody(1050166997, "7656-1791-1", { KnownItem() });
        printf("      | %s\n", body.c_str());
        Expect(body.find("\"account_id\":1050166997") != std::string::npos, "account_id in the payload");
        Expect(body.find("\"request_id\":\"7656-1791-1\"") != std::string::npos, "request_id in the payload");
        Expect(body.find("\"item_id\":9640101589") != std::string::npos, "item_id (high id 2 of account 1050166997)");
        Expect(body.find("\"def_index\":507") != std::string::npos, "def_index 507");
        Expect(body.find("\"paint_kit\":38") != std::string::npos, "paint_kit 38");
        Expect(body.find("\"paint_seed\":41") != std::string::npos, "paint_seed 41");
        Expect(body.find("\"paint_wear\":1.00000001e-06") != std::string::npos || body.find("\"paint_wear\":9.99999997e-07") != std::string::npos
                || body.find("\"paint_wear\":1e-06") != std::string::npos, "paint_wear is written as a JSON number");
        Expect(body.find("\"equipped\":[{\"class_id\":3,\"slot_id\":0}]") != std::string::npos, "equipped class 3 slot 0");
        Expect(body.find("stattrak") == std::string::npos && body.find("stickers") == std::string::npos,
            "absent fields are not written");
    }

    printf("payload escaping, empty snapshot\n");
    {
        const std::string body = BuildSnapshotBody(1, "r", { RichItem() });
        Expect(body.find("\"custom_name\":\"My \\\"best\\\" \\\\ rifle\"") != std::string::npos, "quotes and backslash escaped");
        Expect(BuildSnapshotBody(1, "r", {}).find("\"items\":[]") != std::string::npos, "no equipped item = items:[] (not 'missing')");
    }

    printf("serialized -> server object (roundtrip)\n");
    {
        const std::vector<Item> sent = { KnownItem(), RichItem() };
        BackendClient::ServerRoster roster;
        const bool parsed = BackendClient::ParseServerRoster(RosterBody(sent), roster);
        Expect(parsed, "roster with skin_snapshots parses");
        Expect(roster.players.size() == 3, "roster players unchanged by the new fields");
        Expect(roster.skinSnapshots.size() == 1 && roster.skinSnapshots[0].accountId == 1050166997, "snapshot of account 1050166997");
        Expect(roster.skinSnapshots.size() == 1 && roster.skinSnapshots[0].steamId64 == 76561199010432725ull, "steam_id64");
        Expect(roster.skinMissing.size() == 1 && roster.skinMissing[0] == 1000, "account 1000 has no snapshot");
        Expect(roster.skinSnapshots.size() == 1 && roster.skinSnapshots[0].items.size() == 2, "both items arrive");
        if (roster.skinSnapshots.size() == 1 && roster.skinSnapshots[0].items.size() == 2)
        {
            Expect(SameItem(roster.skinSnapshots[0].items[0], sent[0]), "item 2 (def 507, paint 38, seed 41, wear) survives the round trip");
            Expect(SameItem(roster.skinSnapshots[0].items[1], sent[1]), "stattrak, name, stickers, two slots survive the round trip");
            printf("      | server sees: %s\n", Describe(roster.skinSnapshots[0].items[0]).c_str());
        }
    }

    printf("old backend / no skin fields\n");
    {
        BackendClient::ServerRoster roster;
        const bool parsed = BackendClient::ParseServerRoster("{\"match_id\":\"m-1\",\"mode\":\"competitive\",\"status\":\"READY\","
            "\"required_players\":1,\"players\":[{\"account_id\":5,\"fake\":false}]}", roster);
        Expect(parsed && roster.skinSnapshots.empty() && roster.skinMissing.empty(), "a roster without skin fields is still a roster");
    }

    printf("limits\n");
    {
        std::vector<Item> many(SkinSync::MaxItems + 10, KnownItem());
        std::string items;
        AppendItemsJson(items, many);
        size_t count = 0;
        for (size_t p = items.find("\"item_id\""); p != std::string::npos; p = items.find("\"item_id\"", p + 1)) count++;
        Expect(count == SkinSync::MaxItems, "at most 64 items are written");
    }

    printf("change detection (the poller reports only changes)\n");
    {
        MatchSnapshots a;
        a.matchId = "m-1";
        a.players.push_back({ 7, 76561197960265735ull, { KnownItem() } });
        MatchSnapshots b = a;
        Expect(Signature(a) == Signature(b), "same snapshots = same signature");
        b.players[0].items[0].paintKit = 39;
        Expect(Signature(a) != Signature(b), "another paint kit = another signature");
        b = a;
        b.missing.push_back(9);
        Expect(Signature(a) != Signature(b), "a player without snapshot changes the signature");
    }

    printf("Phase D: snapshot -> player matching -> item / slot matching -> application plan\n");
    {
        const uint64_t me = 76561199010432725ull;      // account 1050166997
        Item other = KnownItem();
        other.itemId = (7ull << 32) | 1000ull;
        other.paintKit = 99;

        MatchSnapshots snapshots;
        snapshots.matchId = "m-test";
        // the player is NOT first in the list: matching is by account / steamid, never by position
        snapshots.players.push_back({ 1000, 76561197960266728ull, { other } });
        snapshots.players.push_back({ 1050166997, me, { KnownItem() } });
        snapshots.missing.push_back(1001);

        ApplyPlan plan = PlanForPlayer(snapshots, me);
        Expect(plan.playerMatched && plan.accountId == 1050166997, "the connecting player is matched by account id");
        Expect(plan.items.size() == 1, "one item is applied");
        if (plan.items.size() == 1)
        {
            const Item &applied = plan.items[0];
            Expect(applied.defIndex == 507, "def_index 507");
            Expect(applied.hasPaint && applied.paintKit == 38, "paint_kit 38 (not the other player's 99)");
            Expect(applied.paintSeed == 41, "paint_seed 41");
            Expect(applied.paintWear < 0.0001f && applied.paintWear >= 0.0f, "paint_wear ~0");
            Expect(applied.equipped.size() == 1 && applied.equipped[0].classId == 3 && applied.equipped[0].slotId == 0,
                "equipped slot: CT (3) / knife (0) comes from the snapshot");
        }

        Expect(!PlanForPlayer(snapshots, 76561197960265728ull + 1001).playerMatched, "a player without a snapshot (missing) gets nothing");
        Expect(!PlanForPlayer(snapshots, 76561197960265728ull + 1050166997ull + (1ull << 32)).playerMatched,
            "same account, another SteamID64 (instance): not the snapshot's owner");
        Expect(!PlanForPlayer(MatchSnapshots{}, me).playerMatched, "no match snapshots yet: nothing to apply");

        // item checks
        Item unequipped = KnownItem(); unequipped.equipped.clear();
        Item foreign = KnownItem(); foreign.itemId = (3ull << 32) | 1000ull;
        Item defaultId = KnownItem(); defaultId.itemId = (0xFull << 60) | 1050166997ull;
        Item badWear = KnownItem(); badWear.paintWear = 2.0f;
        Item noDef = KnownItem(); noDef.defIndex = 0;
        MatchSnapshots bad;
        bad.matchId = "m-test";
        bad.players.push_back({ 1050166997, me, { unequipped, foreign, defaultId, badWear, noDef, KnownItem() } });
        ApplyPlan filtered = PlanForPlayer(bad, me);
        Expect(filtered.items.size() == 1 && filtered.rejected.size() == 5,
            "unequipped / foreign / default-id / wear > 1 / no def_index are rejected, the valid item stays");

        MatchSnapshots onlyBad;
        onlyBad.matchId = "m-test";
        onlyBad.players.push_back({ 1050166997, me, { unequipped } });
        ApplyPlan none = PlanForPlayer(onlyBad, me);
        Expect(none.playerMatched && none.items.empty() && !none.reason.empty(), "matched player, nothing applicable: reason given");
    }

    printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
    return g_failed ? 1 : 0;
}
