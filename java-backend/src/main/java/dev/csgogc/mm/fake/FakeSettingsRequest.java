package dev.csgogc.mm.fake;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;

/** Body of PUT /admin/api/fake-settings: what is not sent stays as it is. */
public record FakeSettingsRequest(
        /** the master switch: Fake Players ON / OFF (OFF: matches wait for real players only) */
        Boolean master,
        /** how long a gathering match waits for more real players before its virtual players are decided */
        @Min(0) @Max(600) Integer gatherWindowSeconds) {
}
