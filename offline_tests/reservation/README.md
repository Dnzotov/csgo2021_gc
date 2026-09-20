# Offline test: reservation keep-alive of a classic dedicated server

RESEARCH_FINDINGS.md §60. `build.bat` compiles the **real** `csgo_gc\reservation_keepalive.h` (header-only, no game, no DLL
dependencies) together with `keepalive_test.cpp`:

* `keepalive_test.exe` — the lifecycle of the plain `G<cookie>,<cookie>,1:` reservation against a small model of the engine's
  reservation rules (`sv_mmqueue_reservation_timeout` 21 s, empty reserved server drops the cookie after the expiry +
  `sv_hibernate_postgame_delay` 5 s, `Unreserve`). `ALL PASSED` expected. Covers: the reservation is created with the same
  payload as before, the refresh interval (8 s, nothing earlier), one refresh loop only, the cookie never changes, stop stops,
  the idle lease (release, renewal by client activity, re-arm, `0` = unlimited), the engine refusing (give up after 3 in a
  row), Touch from another thread, and the failure of §59 (a one-shot reservation answers `awaiting=127` after 40 s, the
  kept-alive one answers `awaiting=0` after 40 s and 10 min).

What it does **not** cover: the wiring in `ServerGC` / `steam_hook.cpp` (`HostEvent::ReservationKeepAlive`, the per-frame
`Tick`, `BeginAuthSession`/`EndAuthSession` activity) — that needs the game; it is compiled into `csgo_gc.dll` and verified by
the live test in §60. The `-gc_mode` roster (`AcceptTest::FakeRoster`) has its own harness in `..\roster`.

## §61: `client_flow_test.exe` (Training / "Invalid user info")

Модель `CBaseServer::ConnectClient` (сервер с cookie принимает клиента только с тем же `cl_session`) + проверка графа вызовов по
реальным `gc_client.cpp` / `gc_server.cpp` (build.bat копирует их как `*.src`): клиентский процесс не резервирует свой engine,
`StartServerFlow` сохранил Accept-путь и `reservationid`, `ServerGC::ReserveServerForOurCookie` — keep-alive только для
dedicated, listen-сервер не резервируется. Мутация (вернуть `PostToHost(ReserveServerForQueuedGame)` в `StartServerFlow`)
проваливает тест.
