// Equipment Sync, offline test: client snapshot -> validation (items_game.txt) -> JSON -> roster -> per-player plan -> SO type 43
// inside the CacheSubscribed that Skin Sync builds. Evidence level 1/2 (serialization, framing). Whether server.dll hands out the
// chosen weapon was proven live for the static experiment (USP-S, account 1050166997); a live test of the dynamic path is the
// user's. Run from the game folder (reads csgo/scripts/items/items_game.txt only); see build_equipment.bat.
#include "stdafx.h"
#include "config.h"
#include "backend_client.h"
#include "equipment_snapshot.h"
#include "gc_message.h"
#include "inventory.h"
#include "keyvalue.h"
#include "skin_snapshot.h"

#include <set>

static int g_failed = 0;
static void Expect(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

using EquipmentSync::Entry;

static bool Legal(const EquipmentSync::LoadoutTable &table, uint32_t def, uint32_t classId, uint32_t slot)
{
    std::string why;
    return table.Legal({ def, classId, slot }, why);
}

static std::string RosterBody(const std::string &equipmentSnapshots, const char *missing)
{
    return "{\"match_id\":\"m-test\",\"mode\":\"competitive\",\"status\":\"READY\",\"map\":\"de_vertigo\","
        "\"accept_required\":true,\"required_players\":3,\"players\":[{\"account_id\":1050166997,\"fake\":false},"
        "{\"account_id\":1000,\"fake\":false},{\"account_id\":4199878657,\"fake\":true}],"
        "\"skin_snapshots\":[],\"skin_missing\":[1050166997,1000],\"equipment_snapshots\":[" + equipmentSnapshots
        + "],\"equipment_missing\":" + missing + "}";
}

static const CMsgSOCacheSubscribed_SubscribedType *FindType43(const CMsgSOCacheSubscribed &message)
{
    for (const auto &object : message.objects())
    {
        if (object.type_id() == SOTypeDefaultEquippedDefinitionInstanceClient)
        {
            return &object;
        }
    }
    return nullptr;
}

int main()
{
    const uint64_t meSteam = 76561199010432725ull;       // the project's test account 1050166997
    const uint32_t me = static_cast<uint32_t>(meSteam & 0xFFFFFFFFull);
    const uint64_t otherSteam = 76561197960265728ull + 1000;
    const uint32_t other = 1000;

    printf("loadout table (items_game.txt)\n");
    EquipmentSync::LoadoutTable table;
    {
        KeyValue root{ "root" };
        Expect(root.ParseFromFile("csgo/scripts/items/items_game.txt"), "items_game.txt readable (run from the game folder)");
        const KeyValue *itemsGame = root.GetSubkey("items_game");
        Expect(itemsGame && table.Load(*itemsGame) && table.ItemCount() > 100, "the table is built from items, prefabs and player_loadout_slots");
    }
    Expect(EquipmentSync::LoadoutTable::Shared() != nullptr, "the shared table loads from the relative game path");

    printf("1. validation: what is legal\n");
    Expect(Legal(table, 61, 3, 2), "USP-S for CT, secondary0");
    Expect(Legal(table, 32, 3, 2), "P2000 for CT, secondary0");
    Expect(Legal(table, 4, 2, 2), "Glock for T, secondary0");
    Expect(Legal(table, 60, 3, 15), "M4A1-S for CT, rifle1");
    Expect(Legal(table, 16, 3, 15), "M4A4 for CT, rifle1");
    Expect(Legal(table, 7, 2, 15), "AK-47 for T, rifle1");
    Expect(Legal(table, 63, 2, 5) && Legal(table, 63, 3, 5), "CZ75 for both teams, secondary3");
    Expect(Legal(table, 9, 3, 18) && Legal(table, 9, 2, 18), "AWP in its rifle slot");

    printf("2. validation: what is refused\n");
    Expect(!Legal(table, 61, 2, 2), "USP-S for T");
    Expect(!Legal(table, 32, 2, 2), "P2000 for T");
    Expect(!Legal(table, 7, 3, 15), "AK-47 for CT");
    Expect(!Legal(table, 7, 2, 2), "AK-47 in a pistol slot");
    Expect(!Legal(table, 9, 3, 2), "AWP in a pistol slot");
    Expect(!Legal(table, 61, 3, 3), "USP-S in the wrong slot (item_sub_position)");
    Expect(!Legal(table, 507, 3, 0) && !Legal(table, 507, 3, 2), "a knife through type 43");
    Expect(!Legal(table, 999999, 3, 2), "unknown def_index");
    Expect(!Legal(table, 0, 3, 2), "def_index 0");
    Expect(!Legal(table, 61, 0, 2), "class_id 0");
    Expect(!Legal(table, 61, 4, 2), "class_id 4");
    Expect(!Legal(table, 61, 3, 0) && !Legal(table, 61, 3, 8) && !Legal(table, 61, 3, 20), "slots outside 2..7 / 14..19");

    printf("3. snapshot: Check()\n");
    {
        EquipmentSync::Checked ok = EquipmentSync::Check({ { 61, 3, 2 }, { 60, 3, 15 }, { 4, 2, 2 }, { 7, 2, 15 } }, table);
        Expect(ok.accepted.size() == 4 && ok.rejected.empty(), "several class/slot entries are all accepted, in the given order");
        Expect(ok.accepted[0].defIndex == 61 && ok.accepted[3].defIndex == 7, "order kept");

        EquipmentSync::Checked dup = EquipmentSync::Check({ { 61, 3, 2 }, { 32, 3, 2 }, { 60, 3, 15 } }, table);
        Expect(dup.accepted.size() == 1 && dup.accepted[0].defIndex == 60 && dup.rejected.size() == 2,
            "a duplicated (class, slot) is ambiguous: neither entry is applied, the others stay");

        EquipmentSync::Checked mixed = EquipmentSync::Check({ { 61, 3, 2 }, { 61, 2, 2 }, { 9, 3, 4 }, { 999999, 3, 14 }, { 4, 0, 2 }, { 60, 3, 3 } }, table);
        Expect(mixed.accepted.size() == 1 && mixed.rejected.size() == 5, "wrong team / wrong slot / unknown / class 0 are rejected one by one");

        std::vector<Entry> tooMany;
        for (uint32_t i = 0; i < EquipmentSync::MaxEntries + 4; i++)
        {
            tooMany.push_back({ 61, 3, 2 + (i % 6) });
        }
        Expect(EquipmentSync::Check(tooMany, table).accepted.empty(), "a snapshot with duplicates beyond the maximum gives nothing ambiguous");
        Expect(EquipmentSync::Check({}, table).accepted.empty(), "an empty snapshot is fine and accepts nothing");
    }

    printf("4. client payload and the server-side parser\n");
    const std::vector<Entry> mine = { { 61, 3, 2 }, { 60, 3, 15 }, { 4, 2, 2 } };
    {
        const std::string body = EquipmentSync::BuildSnapshotBody(me, "r1", mine);
        Expect(body.find("\"account_id\":1050166997") != std::string::npos && body.find("\"request_id\":\"r1\"") != std::string::npos
            && body.find("\"item_definition\":61,\"class_id\":3,\"slot_id\":2") != std::string::npos, "payload: account, request_id, entry");
        Expect(EquipmentSync::BuildSnapshotBody(me, "r\"1", {}).find("\"request_id\":\"r\\\"1\"") != std::string::npos, "request_id is escaped");
        Expect(EquipmentSync::BuildSnapshotBody(me, "r1", {}).find("\"entries\":[]") != std::string::npos, "an empty snapshot is entries:[]");

        std::string entriesJson;
        EquipmentSync::AppendEntriesJson(entriesJson, mine);
        BackendClient::ServerRoster roster;
        const std::string snapshots = "{\"account_id\":1050166997,\"steam_id64\":\"76561199010432725\",\"received_at\":\"2026-10-08T17:52:15Z\",\"entries\":" + entriesJson + "}";
        Expect(BackendClient::ParseServerRoster(RosterBody(snapshots, "[1000]"), roster), "roster with equipment_snapshots parses");
        Expect(roster.equipmentSnapshots.size() == 1 && roster.equipmentSnapshots[0].accountId == me
            && roster.equipmentSnapshots[0].steamId64 == meSteam, "snapshot keyed by account_id + steam_id64");
        Expect(roster.equipmentSnapshots[0].entries.size() == 3 && roster.equipmentSnapshots[0].entries[0].defIndex == 61
            && roster.equipmentSnapshots[0].entries[0].classId == 3 && roster.equipmentSnapshots[0].entries[0].slotId == 2
            && roster.equipmentSnapshots[0].entries[2].defIndex == 4 && roster.equipmentSnapshots[0].entries[2].classId == 2, "entries survive");
        Expect(roster.equipmentMissing.size() == 1 && roster.equipmentMissing[0] == 1000, "equipment_missing names the player without a snapshot");
        Expect(roster.players.size() == 3 && roster.skinMissing.size() == 2, "players and the skin fields are unaffected");

        BackendClient::ServerRoster old;
        Expect(BackendClient::ParseServerRoster("{\"match_id\":\"m-1\",\"mode\":\"competitive\",\"status\":\"READY\","
            "\"required_players\":1,\"players\":[{\"account_id\":5,\"fake\":false}]}", old) && old.equipmentSnapshots.empty(),
            "an older backend (no equipment fields) is still a roster");
    }

    printf("5. account isolation\n");
    EquipmentSync::MatchSnapshots match;
    match.matchId = "m-test";
    match.players.push_back({ other, otherSteam, { { 32, 3, 2 }, { 16, 3, 15 } } });   // B listed first: P2000 + M4A4
    match.players.push_back({ me, meSteam, mine });                                      // A: USP-S + M4A1-S + T Glock
    match.missing = { 77 };
    {
        EquipmentSync::ApplyPlan a = EquipmentSync::PlanForPlayer(match, meSteam, &table);
        Expect(a.playerMatched && a.entries.size() == 3 && a.entries[0].defIndex == 61 && a.entries[1].defIndex == 60, "player A gets A's entries (not first in the list)");
        EquipmentSync::ApplyPlan b = EquipmentSync::PlanForPlayer(match, otherSteam, &table);
        Expect(b.playerMatched && b.entries.size() == 2 && b.entries[0].defIndex == 32 && b.entries[1].defIndex == 16, "player B gets B's entries");
        bool aHasB = false, bHasA = false;
        for (const Entry &e : a.entries) aHasB |= (e.defIndex == 32 || e.defIndex == 16);
        for (const Entry &e : b.entries) bHasA |= (e.defIndex == 61 || e.defIndex == 60 || e.defIndex == 4);
        Expect(!aHasB && !bHasA, "no entry of one account ever reaches the other");
        EquipmentSync::ApplyPlan stranger = EquipmentSync::PlanForPlayer(match, 76561197960265728ull + 55, &table);
        Expect(!stranger.playerMatched && stranger.entries.empty(), "a player without a snapshot gets nothing (the mode's normal loadout)");
        EquipmentSync::ApplyPlan missing = EquipmentSync::PlanForPlayer(match, 76561197960265728ull + 77, &table);
        Expect(!missing.playerMatched && missing.entries.empty(), "a player named in equipment_missing gets nothing");
        EquipmentSync::ApplyPlan otherInstance = EquipmentSync::PlanForPlayer(match, meSteam + (1ull << 32), &table);
        Expect(!otherInstance.playerMatched, "same account id, another SteamID64 (instance): not the owner of the snapshot");
        EquipmentSync::MatchSnapshots none;
        Expect(!EquipmentSync::PlanForPlayer(none, meSteam, &table).playerMatched, "no match equipment yet: nothing");
        EquipmentSync::ApplyPlan noTable = EquipmentSync::PlanForPlayer(match, meSteam, nullptr);
        Expect(noTable.playerMatched && noTable.entries.empty(), "items_game.txt unreadable: matched, nothing accepted");

        EquipmentSync::MatchSnapshots tampered = match;
        tampered.players[1].entries = { { 61, 2, 2 }, { 507, 3, 0 }, { 60, 3, 15 } };   // a hostile snapshot: T USP-S, a knife, one good entry
        EquipmentSync::ApplyPlan t = EquipmentSync::PlanForPlayer(tampered, meSteam, &table);
        Expect(t.entries.size() == 1 && t.entries[0].defIndex == 60 && t.rejected.size() == 2, "the server re-validates: only the legal entry survives");

        Expect(EquipmentSync::Signature(match) == EquipmentSync::Signature(match), "same snapshots = same signature");
        EquipmentSync::MatchSnapshots changed = match;
        changed.players[1].entries[0].defIndex = 32;
        Expect(EquipmentSync::Signature(changed) != EquipmentSync::Signature(match), "another weapon = another signature");
    }

    printf("6. CacheSubscribed: Skin Sync untouched, type 43 additive\n");
    {
        SkinSync::Item knife;
        knife.itemId = (2ull << 32) | me;
        knife.defIndex = 507; knife.quality = 99; knife.rarity = 6;
        knife.hasPaint = true; knife.paintKit = 38; knife.paintSeed = 41; knife.paintWear = 0.000001f;
        knife.equipped.push_back({ 3, 0 });
        SkinSync::MatchSnapshots skins;
        skins.matchId = "m-test";
        skins.players.push_back({ me, meSteam, { knife } });
        const SkinSync::ApplyPlan skinPlan = SkinSync::PlanForPlayer(skins, meSteam);
        ItemSchema schema;
        CMsgSOCacheSubscribed base;
        Inventory::BuildServerCache(schema, skinPlan, 39, base);
        const std::string baseBytes = base.SerializeAsString();

        // no equipment: the message stays exactly as Skin Sync built it
        CMsgSOCacheSubscribed untouched = base;
        EquipmentSync::ApplyPlan nothing = EquipmentSync::PlanForPlayer(match, 76561197960265728ull + 55, &table);
        Expect(Inventory::AppendEquipmentCache(nothing, untouched) == 0 && untouched.SerializeAsString() == baseBytes,
            "no equipment: nothing appended, message byte-identical");
        EquipmentSync::MatchSnapshots empty = match;
        empty.players[1].entries.clear();
        Expect(Inventory::AppendEquipmentCache(EquipmentSync::PlanForPlayer(empty, meSteam, &table), untouched) == 0 && untouched.SerializeAsString() == baseBytes,
            "an empty equipment snapshot: nothing appended");

        // equipment: one more SO object, the Skin Sync objects unchanged
        CMsgSOCacheSubscribed withEquipment = base;
        const EquipmentSync::ApplyPlan plan = EquipmentSync::PlanForPlayer(match, meSteam, &table);
        Expect(Inventory::AppendEquipmentCache(plan, withEquipment) == 3, "three type 43 entries written");
        Expect(withEquipment.objects_size() == base.objects_size() + 1, "exactly one more SO object type");
        bool identical = withEquipment.owner_soid().id() == base.owner_soid().id() && withEquipment.version() == base.version();
        for (int i = 0; i < base.objects_size(); i++)
        {
            identical &= withEquipment.objects(i).SerializeAsString() == base.objects(i).SerializeAsString();
        }
        Expect(identical, "owner, version and the Skin Sync objects (type 1 / type 2) are byte-identical");
        Expect(withEquipment.objects(withEquipment.objects_size() - 1).type_id() == 43, "the new object is type 43, appended last");

        GCMessageWrite write{ k_ESOMsg_CacheSubscribed, withEquipment };
        GCMessageRead read{ 0, write.Data(), write.Size() };
        CMsgSOCacheSubscribed parsed;
        Expect(read.IsValid() && read.IsProtobuf() && read.TypeUnmasked() == k_ESOMsg_CacheSubscribed && read.ReadProtobuf(parsed),
            "the framed k_ESOMsg_CacheSubscribed parses back");
        const auto *t43 = FindType43(parsed);
        Expect(t43 && t43->object_data_size() == 3, "type 43 object with one object_data per entry");
        std::set<std::tuple<uint32_t, uint32_t, uint32_t>> keys;
        bool fieldsOk = t43 != nullptr;
        for (int i = 0; t43 && i < t43->object_data_size(); i++)
        {
            CSOEconDefaultEquippedDefinitionInstanceClient equip;
            fieldsOk &= equip.ParseFromString(t43->object_data(i));
            fieldsOk &= equip.account_id() == me && equip.item_definition() == plan.entries[i].defIndex
                && equip.class_id() == plan.entries[i].classId && equip.slot_id() == plan.entries[i].slotId;
            keys.insert({ equip.account_id(), equip.class_id(), equip.slot_id() });
            printf("      | type43[%d]: account %u def %u class %u slot %u\n", i, equip.account_id(), equip.item_definition(), equip.class_id(), equip.slot_id());
        }
        Expect(fieldsOk, "account_id / item_definition / class_id / slot_id read back exactly");
        Expect(keys.size() == 3, "key fields (account_id, class_id, slot_id) are unique");

        // the other player's message carries only the other player's entries and account
        SkinSync::ApplyPlan emptySkin = SkinSync::PlanForPlayer(SkinSync::MatchSnapshots{}, otherSteam);
        CMsgSOCacheSubscribed forB;
        Inventory::BuildServerCache(schema, emptySkin, 39, forB);       // equipment-only player: no skin items
        const EquipmentSync::ApplyPlan planB = EquipmentSync::PlanForPlayer(match, otherSteam, &table);
        Expect(Inventory::AppendEquipmentCache(planB, forB) == 2, "equipment-only player (no skins): type 43 on top of the empty item cache");
        const auto *t43b = FindType43(forB);
        bool bOk = t43b && t43b->object_data_size() == 2 && forB.owner_soid().id() == otherSteam;
        for (int i = 0; t43b && i < t43b->object_data_size(); i++)
        {
            CSOEconDefaultEquippedDefinitionInstanceClient equip;
            equip.ParseFromString(t43b->object_data(i));
            bOk &= equip.account_id() == other && (equip.item_definition() == 32 || equip.item_definition() == 16);
        }
        Expect(bOk, "B's message: B's owner SOID, B's account in every entry, only B's weapons");

        // a plan that was not matched never writes anything, whatever it holds
        EquipmentSync::ApplyPlan forged;
        forged.accountId = me;
        forged.entries = { { 61, 3, 2 } };
        CMsgSOCacheSubscribed untouched2 = base;
        Expect(Inventory::AppendEquipmentCache(forged, untouched2) == 0 && untouched2.SerializeAsString() == baseBytes, "an unmatched plan writes nothing");
    }

    printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
    printf("Whether server.dll hands out the chosen weapon for the dynamic path is a live test.\n");
    return g_failed ? 1 : 0;
}
