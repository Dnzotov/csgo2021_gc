package dev.csgogc.mm.skin;

import jakarta.validation.Valid;
import jakarta.validation.constraints.DecimalMax;
import jakarta.validation.constraints.DecimalMin;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotEmpty;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.util.List;

/**
 * Body of POST /api/v1/matchmaking/skin-snapshot: what the GC of a player has equipped (EquippedSkinSnapshot,
 * research/backend_skin_sync_design.md). Only equipped items, the same rule the SOCache sent to a game server always had.
 * Everything in it is untrusted client data: sizes, ranges and strings are validated here, the owner is checked against
 * the live search by SearchService.submitSkinSnapshot.
 */
public record SkinSnapshotRequest(
        @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
        @NotNull @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be 1-64 chars of [A-Za-z0-9_.:-]") String requestId,
        /** at most MAX_ITEMS (the game server side limit of the project), may be empty = nothing custom equipped */
        @NotNull @Size(max = MAX_ITEMS) List<@Valid @NotNull Item> items) {

    public static final int MAX_ITEMS = 64;
    public static final int MAX_STICKERS = 6;

    public record Item(
            @NotNull @Min(1) Long itemId,
            @NotNull @Min(0) @Max(65535) Integer defIndex,
            @Min(0) @Max(255) Integer quality,
            @Min(0) @Max(255) Integer rarity,
            /** paint index (attribute 6); absent for items without a finish */
            @Min(0) @Max(100000) Integer paintKit,
            @Min(0) @Max(1000000) Integer paintSeed,
            @DecimalMin("0.0") @DecimalMax("1.0") Double paintWear,
            @Valid StatTrak stattrak,
            @Size(max = 64) @Pattern(regexp = "[^\\p{Cntrl}]*", message = "must not contain control characters") String customName,
            @Size(max = MAX_STICKERS) List<@Valid @NotNull Sticker> stickers,
            @NotEmpty @Size(max = 8) List<@Valid @NotNull Equipped> equipped) {
    }

    public record StatTrak(@NotNull @Min(0) @Max(0xFFFFFFFFL) Long count, @Min(0) @Max(65535) Integer scoreType) {
    }

    public record Sticker(
            @NotNull @Min(0) @Max(MAX_STICKERS - 1) Integer slot,
            @NotNull @Min(0) @Max(1000000) Integer id,
            @DecimalMin("0.0") @DecimalMax("1.0") Double wear) {
    }

    /** class 0 = any/noteam, 2 = T, 3 = CT (CSOEconItemEquipped.new_class); slot as in the game's loadout */
    public record Equipped(@NotNull @Min(0) @Max(7) Integer classId, @NotNull @Min(0) @Max(65535) Integer slotId) {
    }
}
