package dev.csgogc.mm.search;

import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

/**
 * Timers of the matchmaker: expiry of stale searches / assigned searches / server reservations (so a player is never
 * stuck in the queue if the GC or the game crashed) and the matcher, which keeps looking for a free server or for more
 * players even when nothing new arrives.
 */
@Component
public class SearchReaper {

    private final SearchService searches;

    public SearchReaper(SearchService searches) {
        this.searches = searches;
    }

    @Scheduled(fixedDelayString = "${backend.matcher-interval:PT1S}", initialDelayString = "${backend.matcher-interval:PT1S}")
    void run() {
        searches.tick();
    }
}
