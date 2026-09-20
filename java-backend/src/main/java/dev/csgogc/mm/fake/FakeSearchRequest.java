package dev.csgogc.mm.fake;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.util.List;

/** Body of POST / PUT /admin/api/fake-searches: one Fake Players profile. */
public record FakeSearchRequest(
        @NotBlank @Pattern(regexp = "[A-Za-z0-9_]{1,32}", message = "must be a category key such as competitive") String mode,
        /** how many virtual players a match of the mode gets; 0 = the match starts with its real players only */
        @NotNull @Min(0) @Max(64) Integer players,
        @Size(max = 32) List<@Pattern(regexp = "[A-Za-z0-9_]{1,64}", message = "must be a map name") String> maps,
        /** POST only: is the profile on (default true) */
        Boolean enabled,
        /** the profile with the higher priority wins when several fit the match (default 0) */
        @Min(-1000) @Max(1000) Integer priority,
        /** only this game server (registry id) serves the matches of the profile; null = any server of the mode */
        Long serverId) {
}
