package dev.csgogc.mm.server;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;

/**
 * Body of the create / update calls. state is only used when a server is created (AVAILABLE by default); afterwards
 * it changes through the state calls, so an edit never silently frees a reserved server.
 */
public record GameServerRequest(
        @NotBlank @Size(max = 253) @Pattern(regexp = "[A-Za-z0-9._:-]*", message = "must be an IP address or hostname") String host,
        @NotNull @Min(1) @Max(65535) Integer port,
        @NotBlank @Pattern(regexp = "[A-Za-z0-9_]*", message = "must be a category key such as competitive") String category,
        @Pattern(regexp = "[A-Za-z0-9_]{0,64}", message = "must be a map name such as de_dust2") String map,
        Boolean enabled,
        @Pattern(regexp = "AVAILABLE|BUSY", message = "must be AVAILABLE or BUSY") String state,
        @Min(1) @Max(128) Integer maxPlayers) {
}
