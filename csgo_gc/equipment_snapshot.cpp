#include "equipment_snapshot.h"

// keyvalue.h relies on the precompiled header (stdafx.h) for these
#include <cassert>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include "keyvalue.h"

namespace EquipmentSync
{

namespace
{

std::string Lower(std::string_view text)
{
    std::string out{ text };
    for (char &c : out)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool ParseNumber(std::string_view text, uint32_t &out)
{
    if (text.empty() || text.size() > 10)
    {
        return false;
    }
    uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFull)
    {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

// used_by_classes may sit on the item or on any prefab of its chain (the "prefab" value can name several prefabs)
void CollectClasses(const KeyValue &node, const KeyValue *prefabs, int depth, bool &terrorists, bool &counterTerrorists)
{
    if (depth > 8)
    {
        return;
    }
    if (const KeyValue *classes = node.GetSubkey("used_by_classes"))
    {
        for (const KeyValue &entry : *classes)
        {
            const bool on = entry.String() == "1";
            if (entry.Name() == "terrorists")
            {
                terrorists |= on;
            }
            if (entry.Name() == "counter-terrorists")
            {
                counterTerrorists |= on;
            }
        }
    }
    const std::string_view chain = node.GetString("prefab");
    for (size_t p = 0; p < chain.size();)
    {
        const size_t space = chain.find(' ', p);
        const std::string_view one = chain.substr(p, space == std::string_view::npos ? std::string_view::npos : space - p);
        if (!one.empty() && prefabs)
        {
            if (const KeyValue *prefab = prefabs->GetSubkey(one))
            {
                CollectClasses(*prefab, prefabs, depth + 1, terrorists, counterTerrorists);
            }
        }
        p = space == std::string_view::npos ? chain.size() : space + 1;
    }
}

void AppendString(std::string &out, const std::string &text)
{
    out += '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
        }
        else if (c < 0x20)
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
    out += '"';
}

} // namespace

bool LoadoutTable::Load(const KeyValue &itemsGame)
{
    m_items.clear();

    const KeyValue *items = itemsGame.GetSubkey("items");
    const KeyValue *prefabs = itemsGame.GetSubkey("prefabs");
    const KeyValue *slots = itemsGame.GetSubkey("player_loadout_slots");
    if (!items || !slots)
    {
        return false;
    }

    std::map<std::string, int> slotByName;     // "secondary0" -> 2
    const std::string prefix = "loadout_position_";
    for (const KeyValue &slot : *slots)
    {
        const std::string name = Lower(slot.String());
        if (name.compare(0, prefix.size(), prefix) == 0)
        {
            slotByName[name.substr(prefix.size())] = atoi(std::string{ slot.Name() }.c_str());
        }
    }

    for (const KeyValue &item : *items)
    {
        uint32_t defIndex;
        if (!ParseNumber(item.Name(), defIndex))
        {
            continue;   // "default" and friends
        }

        Info info;
        info.baseItem = item.GetString("baseitem") == "1";
        const auto slot = slotByName.find(Lower(item.GetString("item_sub_position")));
        info.slot = slot == slotByName.end() ? -1 : slot->second;
        CollectClasses(item, prefabs, 0, info.terrorists, info.counterTerrorists);
        m_items[defIndex] = info;
    }

    return !m_items.empty() && !slotByName.empty();
}

bool LoadoutTable::Legal(const Entry &entry, std::string &why) const
{
    if (entry.defIndex == 0) { why = "no item_definition"; return false; }
    if (entry.classId != ClassT && entry.classId != ClassCT) { why = "class_id is not 2 (T) / 3 (CT)"; return false; }
    const bool pistol = entry.slotId >= 2 && entry.slotId <= 7;
    const bool rifle = entry.slotId >= 14 && entry.slotId <= 19;
    if (!pistol && !rifle) { why = "slot is neither a pistol (2..7) nor a rifle (14..19) slot"; return false; }

    const auto found = m_items.find(entry.defIndex);
    if (found == m_items.end()) { why = "unknown def_index"; return false; }
    const Info &info = found->second;
    if (!info.baseItem) { why = "not a base item"; return false; }
    if (info.slot != static_cast<int>(entry.slotId)) { why = "item_sub_position puts the item in another slot"; return false; }
    if (entry.classId == ClassT && !info.terrorists) { why = "not usable by terrorists"; return false; }
    if (entry.classId == ClassCT && !info.counterTerrorists) { why = "not usable by counter-terrorists"; return false; }
    return true;
}

const LoadoutTable *LoadoutTable::Shared()
{
    static std::mutex mutex;
    static std::unique_ptr<LoadoutTable> table;

    std::lock_guard lock{ mutex };
    if (!table)
    {
        KeyValue root{ "root" };
        if (!root.ParseFromFile("csgo/scripts/items/items_game.txt"))
        {
            return nullptr;
        }
        const KeyValue *itemsGame = root.GetSubkey("items_game");
        auto loaded = std::make_unique<LoadoutTable>();
        if (!itemsGame || !loaded->Load(*itemsGame))
        {
            return nullptr;
        }
        table = std::move(loaded);
    }
    return table.get();
}

std::string Describe(const Entry &entry)
{
    return "def=" + std::to_string(entry.defIndex) + " class=" + std::to_string(entry.classId) + " slot=" + std::to_string(entry.slotId);
}

Checked Check(const std::vector<Entry> &entries, const LoadoutTable &table)
{
    Checked result;

    std::set<std::pair<uint32_t, uint32_t>> ambiguous;
    {
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (size_t i = 0; i < entries.size() && i < MaxEntries; i++)
        {
            if (!seen.insert({ entries[i].classId, entries[i].slotId }).second)
            {
                ambiguous.insert({ entries[i].classId, entries[i].slotId });
            }
        }
    }

    for (size_t i = 0; i < entries.size(); i++)
    {
        const Entry &entry = entries[i];
        std::string why;
        if (i >= MaxEntries)
        {
            why = "more than the maximum of entries";
        }
        else if (ambiguous.count({ entry.classId, entry.slotId }))
        {
            why = "(class, slot) appears more than once";
        }
        else if (table.Legal(entry, why))
        {
            result.accepted.push_back(entry);
            continue;
        }
        result.rejected.push_back(Describe(entry) + " : " + why);
    }

    return result;
}

void AppendEntriesJson(std::string &out, const std::vector<Entry> &entries)
{
    out += '[';
    size_t written = 0;
    for (const Entry &entry : entries)
    {
        if (written >= MaxEntries)
        {
            break;
        }
        out += (written ? "," : "");
        out += "{\"item_definition\":" + std::to_string(entry.defIndex) + ",\"class_id\":" + std::to_string(entry.classId)
            + ",\"slot_id\":" + std::to_string(entry.slotId) + "}";
        written++;
    }
    out += ']';
}

std::string BuildSnapshotBody(uint32_t accountId, const std::string &requestId, const std::vector<Entry> &entries)
{
    std::string body = "{\"account_id\":" + std::to_string(accountId) + ",\"request_id\":";
    AppendString(body, requestId);
    body += ",\"entries\":";
    AppendEntriesJson(body, entries);
    body += '}';
    return body;
}

std::string Signature(const MatchSnapshots &snapshots)
{
    std::string text = snapshots.matchId + "|";
    for (const PlayerSnapshot &player : snapshots.players)
    {
        text += std::to_string(player.accountId) + ":" + std::to_string(player.steamId64) + ":";
        AppendEntriesJson(text, player.entries);
        text += ";";
    }
    text += "|";
    for (uint32_t id : snapshots.missing)
    {
        text += std::to_string(id) + ",";
    }
    return text;
}

ApplyPlan PlanForPlayer(const MatchSnapshots &snapshots, uint64_t steamId64, const LoadoutTable *table)
{
    ApplyPlan plan;
    plan.steamId64 = steamId64;
    plan.accountId = static_cast<uint32_t>(steamId64 & 0xFFFFFFFFull);

    if (snapshots.matchId.empty())
    {
        plan.reason = "no match equipment on this server";
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
        plan.reason = "no equipment snapshot for this player (the mode's normal base loadout)";
        return plan;
    }

    plan.playerMatched = true;
    if (!table)
    {
        plan.reason = "items_game.txt could not be read: nothing can be validated";
        return plan;
    }

    Checked checked = Check(found->entries, *table);
    plan.entries = std::move(checked.accepted);
    plan.rejected = std::move(checked.rejected);
    if (plan.entries.empty())
    {
        plan.reason = found->entries.empty() ? "the snapshot has no entries" : "no entry of the snapshot is acceptable";
    }
    return plan;
}

} // namespace EquipmentSync
