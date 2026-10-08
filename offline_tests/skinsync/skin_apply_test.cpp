// Phase D offline test with the REAL protobuf types, the REAL item schema (items_game.txt) and the real message framing:
//   ApplyPlan -> Inventory::BuildServerCache -> CMsgSOCacheSubscribed -> GCMessageWrite (exactly what ServerGC::ApplySkins posts to
//   server.dll) -> parsed back the way a receiver reads it.
// Linked against the objects of the DLL build (build_local.bat first), run with the game folder as the working directory
// (it only READS csgo/scripts/items/items_game.txt and csgo_gc/config.txt). What server.dll does with the message cannot be
// tested offline: that part is the live test.
#include "stdafx.h"
#include "config.h"
#include "gc_message.h"
#include "inventory.h"
#include "skin_snapshot.h"

static int g_failed = 0;
static void Expect(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

static SkinSync::Item KnownItem()
{
    SkinSync::Item item;
    item.itemId = (2ull << 32) | 1050166997ull;
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

static const CSOEconItemAttribute *Find(const CSOEconItem &item, uint32_t defIndex)
{
    for (const CSOEconItemAttribute &attribute : item.attribute())
    {
        if (attribute.def_index() == defIndex)
        {
            return &attribute;
        }
    }
    return nullptr;
}

int main()
{
    const uint64_t me = 76561199010432725ull;
    ItemSchema schema;   // items_game.txt of the game folder (cwd)

    // the second item exercises StatTrak, the name and the stickers
    SkinSync::Item rich = KnownItem();
    rich.itemId = (3ull << 32) | 1050166997ull;
    rich.defIndex = 7;
    rich.hasStatTrak = true;
    rich.statTrakCount = 1234;
    rich.statTrakScoreType = 0;
    rich.customName = "My rifle";
    rich.stickers.push_back({ 0, 76, true, 0.25f });
    rich.equipped.push_back({ 2, 0 });

    SkinSync::MatchSnapshots snapshots;
    snapshots.matchId = "m-test";
    snapshots.players.push_back({ 1050166997, me, { KnownItem(), rich } });

    SkinSync::ApplyPlan plan = SkinSync::PlanForPlayer(snapshots, me);
    Expect(plan.playerMatched && plan.items.size() == 2, "the plan has both items of the player");

    CMsgSOCacheSubscribed message;
    const size_t written = Inventory::BuildServerCache(schema, plan, 39, message);
    Expect(written == 2, "two items are written");
    Expect(message.owner_soid().type() == SoIdTypeSteamId && message.owner_soid().id() == me, "owner SOID = the player's SteamID64");

    // the framing the host receives (ServerGC::ApplySkins: GCMessageWrite + PostToHost(HostEvent::Message))
    GCMessageWrite write{ k_ESOMsg_CacheSubscribed, message };
    GCMessageRead read{ 0, write.Data(), write.Size() };
    CMsgSOCacheSubscribed parsed;
    Expect(read.IsValid() && read.IsProtobuf() && read.TypeUnmasked() == k_ESOMsg_CacheSubscribed, "framed as the GC message k_ESOMsg_CacheSubscribed");
    Expect(read.ReadProtobuf(parsed), "the framed message parses back");
    printf("      | message: %u bytes\n", write.Size());

    const CMsgSOCacheSubscribed_SubscribedType *items = nullptr;
    bool persona = false;
    for (const auto &object : parsed.objects())
    {
        if (object.type_id() == SOTypeItem) items = &object;
        if (object.type_id() == SOTypePersonaDataPublic) persona = object.object_data_size() == 1;
    }
    Expect(items && items->object_data_size() == 2, "the item object carries 2 items");
    Expect(persona, "persona data is there (like the SOCache of a client)");

    if (items && items->object_data_size() == 2)
    {
        CSOEconItem knife;
        Expect(knife.ParseFromString(items->object_data(0)), "item 0 parses");
        Expect(knife.id() == KnownItem().itemId && knife.account_id() == 1050166997, "item id and account id");
        Expect(knife.def_index() == 507, "def_index 507");
        Expect(knife.equipped_state_size() == 1 && knife.equipped_state(0).new_class() == 3 && knife.equipped_state(0).new_slot() == 0,
            "equipped_state: class 3 (CT), slot 0 (knife)");
        const CSOEconItemAttribute *kit = Find(knife, ItemSchema::AttributeTexturePrefab);
        const CSOEconItemAttribute *seed = Find(knife, ItemSchema::AttributeTextureSeed);
        const CSOEconItemAttribute *wear = Find(knife, ItemSchema::AttributeTextureWear);
        Expect(kit && schema.AttributeFloat(kit) == 38.0f, "attribute 6 (paint kit) reads back as 38");
        Expect(seed && schema.AttributeFloat(seed) == 41.0f, "attribute 7 (paint seed) reads back as 41");
        Expect(wear && schema.AttributeFloat(wear) > 0.0f && schema.AttributeFloat(wear) < 0.0001f, "attribute 8 (wear) reads back as ~0");
        printf("      | knife: def=%u paint=%.0f seed=%.0f wear=%.9g slot=%u/%u\n", knife.def_index(),
            kit ? schema.AttributeFloat(kit) : -1.0f, seed ? schema.AttributeFloat(seed) : -1.0f,
            wear ? static_cast<double>(schema.AttributeFloat(wear)) : -1.0, knife.equipped_state(0).new_class(),
            knife.equipped_state(0).new_slot());

        CSOEconItem rifle;
        Expect(rifle.ParseFromString(items->object_data(1)), "item 1 parses");
        const CSOEconItemAttribute *kills = Find(rifle, ItemSchema::AttributeKillEater);
        const CSOEconItemAttribute *name = Find(rifle, ItemSchema::AttributeCustomName);
        const CSOEconItemAttribute *sticker = Find(rifle, ItemSchema::AttributeStickerId0);
        const CSOEconItemAttribute *stickerWear = Find(rifle, ItemSchema::AttributeStickerWear0);
        Expect(kills && schema.AttributeUint32(kills) == 1234, "StatTrak count");
        Expect(name && schema.AttributeString(name) == "My rifle", "custom name");
        Expect(sticker && schema.AttributeUint32(sticker) == 76, "sticker id (slot 0)");
        Expect(stickerWear && schema.AttributeFloat(stickerWear) == 0.25f, "sticker wear (slot 0)");
        Expect(rifle.equipped_state_size() == 2, "two equipped slots");
    }

    printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
    return g_failed ? 1 : 0;
}
