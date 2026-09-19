# Offline tests: srcds side of the backend-driven roster

RESEARCH_FINDINGS.md §55. They compile the **real** `server_roster.cpp`, `backend_client.cpp`, `test_accept.cpp` (the
fake driver) and `keyvalue.cpp`, with small stand-ins for the parts of the DLL that need the game (`stdafx.h`, `config.h`,
`funchook.h`). `build.bat` builds both programs into `build\`.

* `controller_test.exe` — decision table of `RosterFeed::Controller` (what srcds does with each answer of the backend:
  arm / replace / postpone / confirm / fall back to the legacy roster). Needs nothing else. `ALL PASSED` expected.
* `roster_e2e.exe <account> <roster size> <wait ms> [legacy]` — the whole chain with a real backend:
  client search → backend holds the assignment → `RosterFeed::Poller` sees the READY match → `Controller` arms the
  roster → `FakeRoster` drives the fake participants to stage 1 → `POST /servers/roster/ready` → the client gets its
  assignment → its first reservation check must be `awaiting=0` (a UDP stand-in for the engine implements 0x21 → 0x25).
  Needs a backend on `127.0.0.1:18090` with api key `test-api`, a Competitive server `127.0.0.1:27555` (map `de_dust2`)
  and a fake search (9 players, Competitive, `de_dust2`); run the backend from a copy of the jar / a separate database,
  not the one you play with. With `legacy` as 4th argument it runs the old srcds-local roster (no backend) instead.

The real engine (`engine.dll`) is not part of it: the game test with srcds started with `-ip 192.168.1.150 -gc_mode ...`
is what proves the reservation against the real engine.
