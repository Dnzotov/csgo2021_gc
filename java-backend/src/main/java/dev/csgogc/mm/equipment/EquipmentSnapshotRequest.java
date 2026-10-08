package dev.csgogc.mm.equipment;

import jakarta.validation.Valid;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.util.List;

/**
 * Body of POST /api/v1/matchmaking/equipment-snapshot: the base weapons a player picked in its loadout (EquipmentSnapshot,
 * research/equipment_sync_phase0_type43.md): per team and slot the definition of the chosen weapon, the data the game
 * server turns into SO type 43. Not the inventory and not the skins (those travel in the skin snapshot).
 * Everything in it is untrusted client data: sizes and ranges are validated here, the owner is checked against the live
 * search by SearchService.submitEquipmentSnapshot, the weapon rules (items_game.txt) are checked again by the game server.
 */
public record EquipmentSnapshotRequest(
        @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
        @NotNull @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be 1-64 chars of [A-Za-z0-9_.:-]") String requestId,
        /** at most MAX_ENTRIES (2 teams x (6 pistol + 6 rifle slots)); may be empty = nothing chosen */
        @NotNull @Size(max = MAX_ENTRIES) List<@Valid @NotNull Entry> entries) {

    public static final int MAX_ENTRIES = 24;

    /** class 2 = T, 3 = CT; slot 2..7 pistols, 14..19 rifles (the first production version) */
    public record Entry(
            @NotNull @Min(1) @Max(65535) Integer itemDefinition,
            @NotNull @Min(2) @Max(3) Integer classId,
            @NotNull @Min(0) @Max(65535) Integer slotId) {
    }

    /** the first-stage slot ranges, shared with the service check */
    public static boolean allowedSlot(int slotId) {
        return (slotId >= 2 && slotId <= 7) || (slotId >= 14 && slotId <= 19);
    }
}
