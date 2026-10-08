#include "skin_snapshot.h"

#include <cstdio>

namespace SkinSync
{

namespace
{

void AppendString(std::string &out, const std::string &text)
{
    out += '"';
    for (unsigned char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (c < 0x20)
            {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

// a float that survives a text round trip, '.' decimal separator whatever the locale of the host is
std::string FloatText(float value)
{
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    for (char *c = buffer; *c; c++)
    {
        if (*c == ',')
        {
            *c = '.';
        }
    }
    return buffer;
}

void AppendItem(std::string &out, const Item &item)
{
    out += "{\"item_id\":" + std::to_string(item.itemId);
    out += ",\"def_index\":" + std::to_string(item.defIndex);
    out += ",\"quality\":" + std::to_string(item.quality);
    out += ",\"rarity\":" + std::to_string(item.rarity);
    if (item.hasPaint)
    {
        out += ",\"paint_kit\":" + std::to_string(item.paintKit);
        out += ",\"paint_seed\":" + std::to_string(item.paintSeed);
        out += ",\"paint_wear\":" + FloatText(item.paintWear);
    }
    if (item.hasStatTrak)
    {
        out += ",\"stattrak\":{\"count\":" + std::to_string(item.statTrakCount) + ",\"score_type\":"
            + std::to_string(item.statTrakScoreType) + "}";
    }
    if (!item.customName.empty())
    {
        out += ",\"custom_name\":";
        AppendString(out, item.customName);
    }
    if (!item.stickers.empty())
    {
        out += ",\"stickers\":[";
        for (size_t i = 0; i < item.stickers.size() && i < MaxStickers; i++)
        {
            const Sticker &s = item.stickers[i];
            out += (i ? "," : "");
            out += "{\"slot\":" + std::to_string(s.slot) + ",\"id\":" + std::to_string(s.id);
            if (s.hasWear)
            {
                out += ",\"wear\":" + FloatText(s.wear);
            }
            out += "}";
        }
        out += "]";
    }
    out += ",\"equipped\":[";
    for (size_t i = 0; i < item.equipped.size(); i++)
    {
        out += (i ? "," : "");
        out += "{\"class_id\":" + std::to_string(item.equipped[i].classId) + ",\"slot_id\":"
            + std::to_string(item.equipped[i].slotId) + "}";
    }
    out += "]}";
}

} // namespace

void AppendItemsJson(std::string &out, const std::vector<Item> &items)
{
    out += '[';
    size_t written = 0;
    for (const Item &item : items)
    {
        if (written >= MaxItems)
        {
            break;
        }
        out += (written ? "," : "");
        AppendItem(out, item);
        written++;
    }
    out += ']';
}

std::string BuildSnapshotBody(uint32_t accountId, const std::string &requestId, const std::vector<Item> &items)
{
    std::string body = "{\"account_id\":" + std::to_string(accountId) + ",\"request_id\":";
    AppendString(body, requestId);
    body += ",\"items\":";
    AppendItemsJson(body, items);
    body += '}';
    return body;
}

std::string Signature(const MatchSnapshots &snapshots)
{
    std::string text = snapshots.matchId + "|";
    for (const PlayerSnapshot &player : snapshots.players)
    {
        text += std::to_string(player.accountId) + ":";
        AppendItemsJson(text, player.items);
        text += ";";
    }
    text += "|";
    for (uint32_t id : snapshots.missing)
    {
        text += std::to_string(id) + ",";
    }
    return text;
}

ApplyPlan PlanForPlayer(const MatchSnapshots &snapshots, uint64_t steamId64)
{
    ApplyPlan plan;
    plan.steamId64 = steamId64;
    plan.accountId = static_cast<uint32_t>(steamId64 & 0xFFFFFFFFull);

    if (snapshots.matchId.empty())
    {
        plan.reason = "no match snapshots on this server";
        return plan;
    }

    const PlayerSnapshot *found = nullptr;
    for (const PlayerSnapshot &player : snapshots.players)
    {
        if (player.accountId == plan.accountId && (player.steamId64 == 0 || player.steamId64 == steamId64))
        {
            found = &player;
            break;
        }
    }

    if (!found)
    {
        plan.reason = "no snapshot for this player (default skins)";
        return plan;
    }

    plan.playerMatched = true;
    constexpr uint64_t DefaultItemMask = 0xFull << 60;   // gc_const_csgo.h ItemIdDefaultItemMask

    for (const Item &item : found->items)
    {
        auto reject = [&](const char *why) { plan.rejected.push_back(Describe(item) + " : " + why); };

        if (plan.items.size() >= MaxItems)
        {
            reject("more than the maximum of items");
        }
        else if (item.equipped.empty())
        {
            reject("not equipped");
        }
        else if (item.defIndex == 0)
        {
            reject("no def_index");
        }
        else if ((item.itemId & 0xFFFFFFFFull) != plan.accountId || (item.itemId & DefaultItemMask) == DefaultItemMask)
        {
            reject("item id does not belong to the account");
        }
        else if (item.hasPaint && !(item.paintWear >= 0.0f && item.paintWear <= 1.0f))
        {
            reject("paint wear out of range");
        }
        else
        {
            plan.items.push_back(item);
        }
    }

    if (plan.items.empty())
    {
        plan.reason = "the snapshot has no applicable item";
    }

    return plan;
}

std::string Describe(const Item &item)
{
    std::string text = "item=" + std::to_string(item.itemId) + " def=" + std::to_string(item.defIndex);
    if (item.hasPaint)
    {
        text += " paint=" + std::to_string(item.paintKit) + " seed=" + std::to_string(item.paintSeed) + " wear="
            + FloatText(item.paintWear);
    }
    return text;
}

} // namespace SkinSync
