package dev.csgogc.mm.search;

import jakarta.validation.Valid;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.util.List;

/**
 * Body of POST /api/v1/matchmaking/search. Only account_id and game_type are needed: the category is derived from
 * game_type &amp; 0xF. mode / game_mode are optional and, when sent, must agree with it.
 *
 * <p>{@code variants} is only for Skirmish (eGame 12): the client selects skirmish modes, one entry per selected mode
 * (name from items_game.txt, its srcds game_mode and the maps of its map group). See {@link SearchVariant}.
 */
public record SearchRequest(
        @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
        @NotNull @Min(0) @Max(0xFFFFFFFFL) Long gameType,
        @Pattern(regexp = "[A-Za-z0-9_]{1,32}", message = "must be a category key such as competitive") String mode,
        @Pattern(regexp = "[A-Za-z0-9_]{1,32}", message = "must be a srcds game_mode name") String gameMode,
        @Size(max = 32) List<@Pattern(regexp = "[A-Za-z0-9_]{1,64}", message = "must be a map name") String> maps,
        @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be 1-64 chars of [A-Za-z0-9_.:-]") String requestId,
        @Size(max = 16) List<@Valid @NotNull Variant> variants,
        /**
         * The other members of the party: the lobby leader's MatchmakingStart carries every member of the lobby in
         * {@code account_ids}, the members' own clients send nothing. The backend gives each of them a search of their own
         * (they find it through GET /matchmaking/account/{id}) and keeps the whole party in ONE match, so every real player
         * gets the assignment, is in the srcds roster and has to accept (RESEARCH_FINDINGS.md #65). The leader itself may be
         * listed, it is ignored.
         */
        @Size(max = 15) List<@NotNull @Min(1) @Max(0xFFFFFFFFL) Long> partyAccountIds) {

    /** a skirmish mode the player selected */
    public record Variant(
            @NotBlank @Pattern(regexp = "[A-Za-z0-9_]{1,48}", message = "must be a skirmish mode name such as armsrace") String name,
            @NotBlank @Pattern(regexp = "[A-Za-z0-9_]{1,32}", message = "must be a srcds game_mode name") String gameMode,
            @Size(max = 32) List<@Pattern(regexp = "[A-Za-z0-9_]{1,64}", message = "must be a map name") String> maps) {
    }
}
