package dev.csgogc.mm.fake;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.util.List;

/** Body of POST / PUT /admin/api/fake-searches. */
public record FakeSearchRequest(
        @NotBlank @Pattern(regexp = "[A-Za-z0-9_]{1,32}", message = "must be a category key such as competitive") String mode,
        @NotNull @Min(1) @Max(64) Integer players,
        @Size(max = 32) List<@Pattern(regexp = "[A-Za-z0-9_]{1,64}", message = "must be a map name") String> maps,
        /** POST only: start searching at once (default true) */
        Boolean enabled) {
}
