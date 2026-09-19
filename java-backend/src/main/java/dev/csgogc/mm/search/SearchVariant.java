package dev.csgogc.mm.search;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * One way a search can be satisfied: a server of {@code category} whose map is one of {@code maps} (no maps = any map).
 * A plain search has exactly one (its own category and maps). A Skirmish ("War Games") search carries one variant per
 * skirmish mode the player ticked (Arms Race, Demolition, Retakes, ...), each with the maps of its map group, because
 * the client selects skirmish MODES, not maps (RESEARCH_FINDINGS.md #54).
 *
 * @param name     what the client selected, e.g. "armsrace" (items_game.txt skirmish_modes)
 * @param category the backend category that serves it (armsrace, demolition or skirmish)
 */
public record SearchVariant(String name, String category, List<String> maps) {

    /** database form: name:category:map,map;name:category:map */
    static String encode(List<SearchVariant> variants) {
        List<String> parts = new ArrayList<>();
        for (SearchVariant v : variants) {
            parts.add(v.name() + ":" + v.category() + ":" + String.join(",", v.maps()));
        }
        return String.join(";", parts);
    }

    static List<SearchVariant> decode(String text) {
        if (text == null || text.isBlank()) {
            return List.of();
        }
        List<SearchVariant> variants = new ArrayList<>();
        for (String part : text.split(";")) {
            String[] fields = part.split(":", -1);
            if (fields.length != 3) {
                continue;
            }
            List<String> maps = fields[2].isEmpty() ? List.of() : Arrays.asList(fields[2].split(","));
            variants.add(new SearchVariant(fields[0], fields[1], maps));
        }
        return List.copyOf(variants);
    }
}
