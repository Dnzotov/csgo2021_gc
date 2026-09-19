package dev.csgogc.mm.search;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;

/** Body of POST /api/v1/matchmaking/cancel. */
public record CancelRequest(
        @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
        /** when sent, only the search started with the same request_id is cancelled (protects a newer search) */
        @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be 1-64 chars of [A-Za-z0-9_.:-]") String requestId) {
}
