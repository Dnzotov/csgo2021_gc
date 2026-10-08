# Root Cause

Прогон: 2026-10-08 20:50–20:53, матч `m-dc77ac9b`, srcds `146.158.123.140:27015` (слушает `192.168.1.150`), игрок SteamID64 `76561199010432725` / AccountID `1050166997`, тестовый предмет item `2` (def 507, paint 38, seed 41, wear ~0).
Метки: LOG / CODE / RUNTIME VERIFIED, INFERRED, UNKNOWN.

## Symptom

Skin выбран и лежит в inventory, matchmaking → резервация → подключение проходят, но в матче оружие/нож выглядит как дефолтное.

## Expected

Сервер получает от игрока SO-кэш с экипированными предметами (`CMsgSOCacheSubscribed`), `server.dll` читает `equipped_state` + атрибуты 6/7/8 и ставит paint kit на оружие.

## Actual

SO-кэш игрока на сервер **не доходит**. Скин не теряется и не портится по пути — он ни разу не отправляется.

## Data flow

См. `skins_data_flow.md`. Коротко: `inventory.txt` → ClientGC → (matchmaking/reservation скины не несут, так и задумано) → srcds `BeginAuthSession` → P2P `k_EMsgNetworkConnect` srcds→клиент → клиент отвечает SO-кэшем → `ServerGC::HandleNetMessage` → `server.dll`.

## Where data is lost

На шаге «P2P-сессия сервер↔клиент» (`ISteamNetworkingMessages`, `networking_server.cpp:66-83` ↔ `networking_client.cpp:64-97`), раньше любой логики skin/inventory/weapon. CASE A, B – исключены; CASE D, E – не достигнуты (UNKNOWN); фактически «C», но в звене P2P, а не в reservation/backend.

## Evidence

| Утверждение | Источник | Метка |
|---|---|---|
| Skin есть в inventory и экипирован | `csgo_gc\inventory.txt` (item 2, attr 6/7/8, `equipped_state {3:0}`) | FILE/CODE VERIFIED |
| `BuildCacheSubscription(server=true)` включит этот предмет | `inventory.cpp:353-372` | CODE VERIFIED |
| Reservation/backend предметов не несут и не должны | proto `cstrike15_gcmessages.proto:415`; `RosterEntry(accountId,fake)`; grep по Java/backend_client = 0 совпадений; payload `Q…1:[acct]…` | CODE+LOG VERIFIED |
| Matchmaking и резервация работают | console.log 47062–47133, server_log (`result: 1`, `awaiting=0 total=10`, `Connected to …:27015`) | LOG VERIFIED |
| Сервер не получил SO-кэш | server_log: `HandleNetMessage` — 0 вхождений; единственный путь вызова `steam_hook.cpp:1320` | LOG+CODE VERIFIED |
| Клиент не получил `NetworkConnect` | console.log (последний запуск): `sending socache` — 0 | LOG VERIFIED |
| P2P-сессия не поднялась | server_log:507 `OnSessionFailed: Timed out attempting to connect`; console.log: `Ignoring P2P signal from 'steamid:90294376102077467', unknown remote connection #1798891437` (steamid = identity анонимного GS из server_log) | LOG VERIFIED |
| Код сам предупреждает об этом режиме | `networking_server.cpp:80-81` (FIXME про таймаут и потерю socache) | CODE VERIFIED |
| Сервер — анонимный GS, слушает LAN-адрес `192.168.1.150` (`-ip`), публичный IP 146.158.123.140, клиент подключается по публичному | server_log (`INSECURE`, `listens on 192.168.1.150:27015`, `Public IP is 146.158.123.140`), console.log 47131 | LOG VERIFIED |
| Повторяемость: 5 рабочих SO-кэшей — все на classic-srcds без `-gc_mode`; остальные подключения без inventory | `RESEARCH_FINDINGS.md` §57.5 (прежний подсчёт; итоги по `console.log` частично перепроверены: 6 `sending socache` в файле) | LOG VERIFIED (частично) |

## Root cause

**Доказано:** доставка skin-данных на сервер зависит от прямой Steam-P2P сессии srcds→клиент; в этом (и в большинстве прошлых) прогонов с Accept/`-gc_mode`-сервером она не устанавливается (srcds: timeout, клиент: unknown remote connection), поэтому клиент не отправляет SO-кэш, `server.dll` не получает предметы и создаёт оружие по умолчанию (последнее — INFERRED).

**Не доказано (честно):** *почему* P2P не поднимается. Кандидаты (из `RESEARCH_FINDINGS.md` §57.7, оба **HYPOTHESIS/HIGH CONFIDENCE, не RUNTIME VERIFIED**):
- H1: `-ip 192.168.1.150` → engine передаёт этот адрес в `SteamGameServer_InitSafe(unIP)`; Steam-сессия с клиентом (клиент подключается по публичному 146.158.123.140) не складывается. Контрольный прогон «classic + `-ip`» (T-A) не проводился. Команда запуска srcds в логах не сохранена; наличие `-ip` выведено из строки `listens on 192.168.1.150` (адрес без `-ip` = loopback, `config.cpp:101`).
- H4: неудачный тайминг/NAT hairpin/ICE (сервер и клиент за одним внешним адресом?). Расположение клиента (89.127.222.175 с точки зрения srcds ≠ 146.158.123.140) и топология сети — UNKNOWN.
Отсутствует диагностика на нужном звене: успешный `ClientConnected` ничего не печатает, `OnSessionRequest` на клиенте ничего не печатает.

## Proposed fix

Минимально и без вмешательства в matchmaking/reservation/inventory-код:

1. **Шаг 0 — диагностика (только логи, безопасные идентификаторы):**
   - `NetworkingServer::ClientConnected`: лог steamid + результат `SendMessageToUser` + `GetSessionConnectionInfo` (state, end reason, `m_szEndDebug`).
   - `NetworkingClient::OnSessionRequest/Update`: лог «session request from <gs steamid>», «got NetworkConnect».
   - `ServerGC::HandleNetMessage` и `ClientGC::HandleSOCacheRequest`: лог числа/ID экипированных предметов (itemid, def_index, paint) и размера.
   - В `HandleNetMessage` после валидации — лог «forwarded to server.dll».
2. **Шаг 1 — разделяющие тесты (без правок кода, одинаковые DLL):** T-A (classic + `-ip`), T-C (`-gc_mode` без `-ip`), T-D (`-ip` + `+ip_steam 0.0.0.0`) по §57.8. Успех = `sending socache` на клиенте и `HandleNetMessage … bytes` на srcds.
3. **Шаг 2 — исправление по итогу:**
   - H1 подтверждена ⇒ отвязать адрес fake/roster от `-ip` (параметры `-backend_ip/-backend_port` уже есть, §69; нужен ещё адрес для UDP-проб), srcds запускать без `-ip` либо с `+ip_steam 0.0.0.0` — затрагивает `config.cpp`/`launch_args.h`, не протокол.
   - P2P недостижим по сети (NAT) ⇒ **резервный транспорт SO-кэша мимо P2P** (проверяемый, не костыль): клиентский GC при Match Found/accept отправляет equipped-SO-кэш (`BuildCacheSubscription(server=true)`) в backend (`POST …/matchmaking/inventory`), srcds забирает его по SteamID из `ClientConnected` через уже существующий backend-клиент и подаёт в тот же `ServerGC` путь `PostToHost(k_ESOMsg_CacheSubscribed)`. Это сохраняет «Inventory → GC → Matchmaking → Server → Weapon → Skin» и использует ту же валидацию. Делать **только** если шаг 1 покажет, что P2P недостижим.
4. Не делать: client-side подмену, hardcoded skin, изменение 0x21/0x25 и 9107.

## Files affected

Шаг 0: `csgo_gc/networking_server.cpp`, `csgo_gc/networking_client.cpp`, `csgo_gc/gc_server.cpp`, `csgo_gc/gc_client.cpp`.
Шаг 2 (H1): `csgo_gc/config.cpp`, `csgo_gc/launch_args.h`, `README.md`/`java-backend/README.md` (команды запуска).
Шаг 2 (резервный транспорт): `csgo_gc/backend_client.cpp/.h`, `csgo_gc/gc_client.cpp`, `csgo_gc/gc_server.cpp`, `java-backend/.../search/*` (+ новая таблица/endpoint), `schema.sql`, тесты.

## Risk

- Логи: безопасные ID только (steamid, itemid, def_index, paint) — ни ticket, ни токенов, ни api key.
- Смена запуска srcds без `-ip` может сломать адресацию fake-проб (UDP 0x21 на LAN-адрес) и доступ backend к roster — их нельзя менять без тестов (`offline_tests/roster`, `reservation`).
- Резервный транспорт: доверие данным клиента (уже так в P2P-дизайне; валидация owner SOID/лимит сохраняются), нагрузка на backend, окно гонки «игрок подключился раньше, чем кэш загружен».
- `keep-alive` резервации сейчас снимается по `HandleNetMessage` (`OnMatchStarted`); при замене транспорта этот триггер надо перепривязать (§57.10).

## Verification plan

1. Шаг 0: пересобрать, повторить матч, убедиться в логах: server `ClientConnected steamid=…`, `session state/endReason`; client `session request`, `sending socache`; server `HandleNetMessage …, N bytes`, `forwarded`.
2. Тест T-A/T-C/T-D (§57.8) — таблица исходов.
3. Один игрок, один известный предмет (item 2 / paint 38 seed 41): в логах `itemid=2 def=507 paint=38` на клиенте и на сервере совпадают.
4. В матче: нож визуально Fade (paint 38), `cl_` проверка через `ent_text`/`weapon_debug`-аналог недоступна — проверять визуально и вторым клиентом (критерий «другой игрок видит»).
5. Регресс: `offline_tests/roster` и `offline_tests/reservation` проходят; Accept-flow и 9107 не изменены.

## Ограничения этого исследования

- Шаги D/E (создание weapon, репликация) в логах не наблюдались — UNKNOWN; `server.dll` не дизассемблировался.
- Остаются только T-слот 3/0 в `inventory.txt` (экипирован CT-нож); для T-стороны нож не экипирован — отдельное замечание на этапе проверки.
- Fake-игроки (`fa4e0001…`) инвентаря не имеют — влияние на «другой игрок видит» не изучалось.
