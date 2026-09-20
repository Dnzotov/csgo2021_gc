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

## §63: Accept timeout (`roster_e2e.exe ... timeout`)

`roster_e2e.exe 1050166997 10 40000 timeout` — тот же стенд, но игрок **не принимает**. Запускать против backend с
`--backend.accept-timeout=PT6S --backend.server-release-cooldown=PT4S`. Проверяет цепочку на реальных классах:
всплыл popup (stage 1, awaiting 0) → backend отменяет матч по дедлайну → клиент видит, что его поиск снова SEARCHING/MATCHED
(`BackendClient` продолжает опрос после назначения) → srcds получает `404`, `Controller` вызывает `release`, `FakeRoster::Release`
снимает резервацию (поздний Accept старого матча = `awaiting=127`) → после cooldown тот же поиск получает НОВЫЙ матч, srcds
взводит его заново, Accept проходит, `BackendClient::ReportAccepted` → backend `ACCEPTED`. Ожидаемый финал: `RESULT: OK`.
`controller_test.exe` дополнительно покрывает решения release (нельзя: игрок на сервере / legacy-ростер / backend недоступен).

## §65: party (`roster_e2e.exe ... party`)

`roster_e2e.exe 1050166997 10 40000 party` — процесс играет GC **участника party** (`BackendClient::WatchAccount`), лидер шлёт
поиск через `curl.exe` (нужен в PATH): backend с `--backend.accept-timeout=PT40S` и профилем Fake Players с count 9 (2 real + min(9, 10 − 2) = 10). Проверяет: клиент участника сам нашёл поиск лидера и получил свой Match Found; при одном игроке popup не
появляется; принявший первым не пускается дальше (`awaiting>0`); после отчёта участника без отчёта лидера `READY_TO_CONNECT`
не приходит; после отчётов обоих — приходит. Ожидаемый финал `RESULT: OK`.

## §66: два реальных игрока по отдельности (`roster_e2e.exe ... gather`)

`roster_e2e.exe 1050166997 10 40000 gather` — игрок A ищет через `curl.exe` сразу, процесс играет GC игрока B и ищет **свой**
поиск на 1,5 с позже. Backend: fake-заявка на 9 игроков, `--backend.fake-players.gather-window=PT3S`, `--backend.accept-timeout=PT30S`.
Проверяет: B попал в матч A (`MATCHED`, 2 игрока, без сервера), после окна оба получили assignment (2 real + 8 fake, оба в ростре
srcds), у A один в ростре не даёт popup (`awaiting=1`), принявший первым не пускается дальше, `accepted_players 1/2`, после отчётов
обоих приходит `READY_TO_CONNECT`. Ожидаемый финал `RESULT: OK`. `controller_test.exe` дополнительно проверяет, что ростер с
несколькими реальными игроками нестандартного размера армируется целиком, а не режется до legacy-ростера.

## §67: три реальных игрока и настроенное число fake (`roster_e2e.exe ... three`)

`roster_e2e.exe 1050166997 <размер ростера> 40000 three` — A и C ищут через `curl.exe`, процесс играет GC игрока B. Число fake задаёт
**профиль** Fake Players backend'а (count, не «добор до capacity»): count 7 → ростер 10, count 2 → ростер 5, count 0 → ростер 3.
Backend: `--backend.fake-players.gather-window=PT3S --backend.accept-timeout=PT40S`. Проверяет на реальных классах: все трое в одном матче
(3 real, без сервера, без Match Found до конца окна), в assignment три реальных аккаунта и ровно настроенные fake, srcds взводит ростер
именно этого размера (0x25 `total`), popup только когда все трое на stage 1, один и два Accept не пускают никого, третий пускает
(`awaiting=0`), backend `READY_TO_CONNECT` только после отчётов всех троих. Ожидаемый финал `RESULT: OK`.

## srcds `-backend_ip` / `-backend_port` (`launch_args_test.exe`)

`launch_args_test.exe` (без настоящего backend'а: захватывающий HTTP-сервер на `127.0.0.1:18093`) прогоняет настоящие `launch_args.h`,
`RosterFeed::Poller` и `backend_client`: запрос `GET /api/v1/servers/roster?address=&port=` и `POST /roster/ready` идут с адресом
`-backend_ip`/`-backend_port`, а без них — с `-ip`/`-port`; игровой адрес (`-ip`/`-port`) не меняется. Случаи: с `-backend_*`, без них,
`-backend_port` отличается от `-port`, только один из двух параметров, порядок аргументов, мусор в `-backend_port`. Ожидаемый финал `RESULT: OK`.
Привязка в `gc_server.cpp` (Poller получает `BackendServerAddress()/BackendServerPort()`, fake-участники — `DedicatedServer*`) проверяется
в `offline_tests/reservation/client_flow_test.exe`.
