# Backend skin sync — design (Phase A–C; Phase D не затрагивается)

Метки: CODE VERIFIED (прочитан код), LOG VERIFIED, DESIGN (решение этого документа), OPEN (препятствие/вопрос).
P2P/SOCache не используется и не меняется. Phase A–D реализованы; **Phase D live-tested: PASS** (скин контрольного предмета появляется в игре, см. `skin_application_phase_d.md`).

## 1. Existing architecture (CODE VERIFIED)

```
csgo.exe (ClientGC)                    Java backend (Spring Boot, SQLite)                 srcds (ServerGC, -gc_mode)
 MatchmakingStart 9101
  └ BackendClient::SearchStarted ────► POST /api/v1/matchmaking/search {account_id,request_id,...}
                                        SearchService.start → place → matchmaker (tick ~1 s)
 worker polls GET /matchmaking/search/{request_id}   match: FORMING → FULL → (server RESERVED) → READY → ACCEPTING → ACCEPTED → ENDED/CANCELLED
                                                                                    ◄──── GET /api/v1/servers/roster?address&port (poll 1/s, RosterFeed::Poller)
                                        rosterForServer() = MatchRoster{match_id,players[{account_id,fake}],...}
                                                                                    ──── POST /servers/roster/ready (roster armed) ──►
 assignment → 9107 → UDP 0x21/0x25 → Accept → POST /matchmaking/accepted → READY_TO_CONNECT → connect
```
- **Идентификация игрока:** `account_id` (uint32, `ISteamUser::GetSteamID() & 0xFFFFFFFF`) в теле JSON; `request_id` = `"<steamid64>-<unix ms>-<counter>"` создаёт клиент (`backend_client.cpp` SearchStarted). Backend деривирует `steam_id64` из account_id (`SearchView.steamId64`).
- **Аутентификация:** единственный общий секрет `X-Api-Key` (`ApiKeyFilter`, роль `ROLE_API`) для всех GC и всех srcds. Per-player аутентификации нет (OPEN, см. §9).
- **Матч-сессия:** таблицы `matchmaking_search` (одна живая на account, уникальный индекс) и `matchmaking_match`; `SearchRecord.matchId/requestId/status`; ростер = реальные `account_id` из `searches.listLiveByMatch/listByMatch` + fake (`0xFA4E0000+n`). Всё состояние matchmaking — серверное, таймеры в `SearchService.expire()` (stale/accept timeout/assigned timeout/server TTL/orphan).
- **Server GC ↔ backend:** srcds ходит на backend сам (HTTP, тот же `BackendClient` с тем же ключом), раз в секунду забирает `GET /servers/roster` и арми́т резервацию; ответ — единственный канал backend → srcds (CODE VERIFIED: `server_roster.cpp Poller::Run`). Он работает только для srcds с `-gc_mode` (accept-режимы). Classic srcds без `-gc_mode` roster не опрашивает (OPEN, §11).
- **Протокол:** backend ↔ GC — HTTP/JSON snake_case (`spring.jackson.property-naming-strategy=SNAKE_CASE`), не protobuf. Protobuf (`protobufs/*.proto`) — только GC ↔ игра. Проект уже содержит самописный JSON-парсер в `backend_client.cpp`.
- **Данные об экипировке на клиенте:** `Inventory::m_items` (`ItemMap = unordered_map<uint64_t, CSOEconItem>`), `CSOEconItem{id,def_index,quality,rarity,custom_name,attribute[],equipped_state[]}`; атрибуты в `ItemSchema::Attribute`: 6 paint kit, 7 seed, 8 wear, 80/81 StatTrak, 111 custom name, 113+4n.. стикеры (id, wear, scale, rotation). Сейчас через сеть эти данные уходят только в P2P SOCache.

### Поля (FIELD / SOURCE / TYPE / CLIENT / SERVER / SERIALIZED)

| Field | Source | Type | Client-side | Server-side (srcds GC) | Serialized сейчас |
|---|---|---|---|---|---|
| item_id | `CSOEconItem.id` | u64 | да | нет | только P2P SOCache (не работает) |
| def_index | `CSOEconItem.def_index` | u32 | да | нет | то же |
| paint_kit | attr 6 (float→uint) | u32 | да | нет | то же |
| paint_seed | attr 7 | u32 | да | нет | то же |
| paint_wear | attr 8 | float | да | нет | то же |
| stattrak | attr 80 (+81 type) | u32 count + флаг | да (если есть) | нет | то же |
| custom_name | `custom_name` / attr 111 | string | да | нет | то же |
| stickers | attr 113..(slot*4: id,wear,scale,rotation) | slot,id,wear | да (если есть) | нет | то же |
| equipped state | `equipped_state[]` {class, slot} | u32,u32 | да | нет | то же |
| quality, rarity | `CSOEconItem` | u32 | да | нет | то же |
Какие реально использованы: в тестовом inventory.txt (item 2: def 507, paint 38, seed 41, wear 0.000001, equipped {3:0}) есть paint/seed/wear/equipped; StatTrak/стикеры/имя поддерживаются схемой, в данных теста их нет — поля в snapshot необязательные.

## 2. Proposed architecture (DESIGN)

```
ClientGC (csgo.exe): Inventory ─► EquippedSkinSnapshot (только экипированные)
   │  при каждом MatchmakingStart (до/сразу после регистрации поиска)  + при усыновлении поиска party-member
   ▼
BackendClient worker: POST /api/v1/matchmaking/skin-snapshot {account_id, request_id, items[]}   (+ retry 3x на сетевой/5xx сбой)
   ▼
Backend: SkinSnapshotStore (in-memory, временное состояние matchmaking), принять ТОЛЬКО если у account есть живой поиск с этим request_id
   ▼  (матч формируется как раньше — semantics matchmaking/Accept/reservation не меняются)
SearchService.rosterForServer(): MatchRoster += skin_snapshots[]  для РЕАЛЬНЫХ игроков ростера
   ▼  существующий канал GET /servers/roster (расширен, а не новый транспорт)
srcds Poller → GCEvent::BackendSkins → ServerGC::OnBackendSkins → m_matchSkins[account] = items   ("snapshot delivered to server")
   ▼ (Phase D, НЕ в этом этапе) server.dll → weapon entity
```

## 3. Data model (DESIGN)

`EquippedSkinSnapshot` (JSON, snake_case; поля необязательные, кроме id/def):
```json
{"account_id":1050166997,"request_id":"7656…-1791…-1",
 "items":[{"item_id":2,"def_index":507,"quality":99,"rarity":6,
           "paint_kit":38,"paint_seed":41,"paint_wear":0.000001,
           "stattrak":{"count":123,"score_type":0},"custom_name":"...",
           "stickers":[{"slot":0,"id":1,"wear":0.0}],
           "equipped":[{"class_id":3,"slot_id":0}]}]}
```
Ограничения (Jakarta Validation, как у остальных DTO): ≤ 64 предмета (серверный лимит проекта `MaxServerSOCacheItems=64`), ≤ 6 стикеров на предмет, `custom_name` ≤ 64 символа и без управляющих символов, числа в допустимых диапазонах (`paint_wear` 0..1, `account_id` 1..0xFFFFFFFF), не принимаются предметы без `equipped` (в snapshot идут только экипированные — то же правило, что у SOCache: `RemoveUnequippedItems`).
В ответе srcds каждому реальному игроку соответствует `{account_id, steam_id64, received_at, items[]}`; для fake-игроков записей нет.

## 4. Lifecycle (DESIGN)

| Момент | Действие |
|---|---|
| `MatchmakingStart` на клиенте | GC строит snapshot из текущего inventory (свежий), сохраняет в `BackendClient`; сразу после успешной регистрации поиска worker отправляет его (повтор при перерегистрации, когда backend «потерял» поиск) |
| party member усыновляет поиск лидера | GC строит snapshot из своего inventory и отправляет при усыновлении |
| приём на backend | проверка: живой поиск account с тем же `request_id`; иначе отказ; повторная отправка заменяет прежний (последний выигрывает) |
| матч формируется | ничего не меняется; на `GET /servers/roster` snapshot'ы собираются по `account_id` ростера |
| Accept / reservation / connect | не затрагиваются |
| удаление | `purgeSkinSnapshots()` в `expire()` каждый тик и после `cancel`: snapshot удаляется, если его `request_id` больше не соответствует живому поиску аккаунта И его матч не живой (`ENDED/CANCELLED`/нет матча); плюс абсолютный TTL (`backend.skin-snapshot-ttl`, по умолчанию 6 ч) на случай зависших записей |
Когда отправлять: до подтверждения матча — snapshot уходит при старте поиска (десятки секунд до Match Found), что раньше, чем srcds забирает ростер (матч заполнен → сервер зарезервирован → srcds получает ростер ≈ через 3–15 с). Это не меняет accept-семантику (отдельная от неё отправка), ack — в логе клиента.

## 5. Authentication (DESIGN + OPEN)

**Факт:** сейчас GC доказывает backend'у не личность игрока, а владение общим `X-Api-Key`. Любой, у кого есть ключ, может послать `account_id` другого игрока — это же верно для `/search`, `/cancel`, `/accepted`. Требование «authenticated player = snapshot owner» **существующей системой не обеспечивается**.
**Что делает этот этап (без изобретения второй аутентификации):** snapshot принимается только для аккаунта, у которого есть живой поиск с таким же `request_id` (тот же уровень проверки, что у `/accepted`: `searches.accept(accountId, requestId)`), `request_id` включает SteamID64 владельца и миллисекундный штамп; запись одна на аккаунт; содержимое ограничено схемой; чужой account без живого поиска → 404, чужой request_id → 409. Это закрывает случайные и посторонние записи, но не защищает от держателя ключа, угадавшего чужой `request_id` (он виден тому же backend'у/админке).
**Предложение (отдельный этап, не реализуется здесь):** per-player токен — GC один раз подписывает challenge сессионным Steam-тикетом, backend проверяет его через Steam Web API/`ISteamUser::BeginAuthSession` аналог и выдаёт токен, привязанный к `account_id`. До этого snapshot — недоверенные данные клиента (как и SOCache по P2P): сервер должен применять их с теми же ограничениями (≤64 предмета, только экипированные).

## 6. Transport (DESIGN)

Client GC → backend: HTTP/JSON через существующий worker `BackendClient` (`Job{"skin-snapshot", "/api/v1/matchmaking/skin-snapshot", body}`), заголовок `X-Api-Key`, тот же стиль `/api/v1/matchmaking/...`; не блокирует GC; повтор при `status==0 || >=500` до 3 раз (как `accepted`). Protobuf не вводится: канал backend↔GC весь JSON.
Endpoint: `POST /api/v1/matchmaking/skin-snapshot` (в `SearchApiController`, рядом с `/accepted`), ответ `{ "stored":true, "items":N, "match_id":null|"m-…" }`.

## 7. Match aggregation (DESIGN)

`SearchService.rosterForServer` уже строит ростер матча на сервере; для каждого **реального** участника берёт snapshot из `SkinSnapshotStore` по `account_id` и кладёт в `MatchRoster.skinSnapshots`. Один snapshot на игрока; чужие — исключены (ключ — account_id ростера); fake — не имеют. Отсутствие snapshot у реального игрока → в поле `skin_missing` перечисляется `account_id` (прозрачность для srcds/логов).

## 8. Server delivery (DESIGN)

Существующий `GET /servers/roster` (srcds `RosterFeed::Poller`, 1/с) расширяется полями `skin_snapshots[]` и `skin_missing[]`. Новый транспорт не создаётся. Изменение набора snapshot'ов (поздно пришёл/обновился) отдельно от ростера доставляется `Poller` через второй callback (`GCEvent::BackendSkins`), не вызывая повторного решения `Controller` (его таблица решений зависит только от ростера и не меняется). `ServerGC::OnBackendSkins` сохраняет `m_matchSkins` (match_id → account_id → items) и пишет `[SKIN_SYNC] snapshot delivered to server`. Очистка: при смене/исчезновении матча (`NoMatch`, другой `match_id`).

## 9. Missing snapshot / failure handling (DESIGN)

| Ситуация | Поведение |
|---|---|
| игрок не прислал snapshot | матч не ломается; в roster его нет в `skin_snapshots`, он в `skin_missing`; на следующем этапе (Phase D) такой игрок использует default/без кастомных скинов |
| backend недоступен при отправке | worker повторяет (3 попытки), лог `[SKIN_SYNC] snapshot send failed`; matchmaking идёт как раньше (поиск не зависит от snapshot) |
| поиск ещё не зарегистрирован (404 `no live search`) | snapshot отправляется только после успешной регистрации, при перерегистрации — снова |
| malformed snapshot | 400, не хранится; лог на backend; клиент не повторяет 4xx |
| duplicate snapshot | заменяет прежний, «последний выигрывает» (`received_at` обновляется) |
| stale snapshot (старый request_id) | отклоняется 409 (поиск аккаунта уже другой) |
| игрок вышел из очереди / expired / matched cancelled | snapshot удаляется `purge` на тике (≤ 1 с) |
| reservation failure / match dissolve | игроки возвращаются в поиск, их snapshot остаётся (поиск живой); при `ENDED/CANCELLED` без живого поиска — удаляется |
| перезапуск backend | in-memory snapshot'ы теряются; клиент пере-регистрирует поиск (404 → Register) и snapshot отправляется снова |

## 10. Security (DESIGN)

Доверенные: `account_id` ростера (из БД backend); не доверенные: всё содержимое snapshot (поля предметов) и, строго говоря, `account_id` отправителя (см. §5). Проверяется: живой поиск + `request_id`, размеры/диапазоны/длины строк, отсутствие управляющих символов в `custom_name`, лимит предметов. Не логируются: ключ API, тикеты, токены — логируются только `account_id`, `match_id`, число предметов, `item_id/def_index/paint_kit/seed/wear`.

## 11. Obstacles и ограничения (не обходятся самовольно)

1. **Нет per-player authentication** (§5). Принято решение: тот же уровень, что `/accepted`; предложен per-player токен.
2. **Classic srcds без `-gc_mode` не опрашивает roster** → snapshot'ы доставляются только на srcds с `-gc_mode` (accept-режимы competitive/wingman/dangerzone). Для остальных режимов нужен тот же poll (или `GET /servers/skins`) — предложение на следующий этап, чтобы не менять поведение classic-серверов сейчас.
3. **Точка Phase D** (`server.dll` → entity) не наблюдаема проектом (`OUTSIDE PROJECT OBSERVABILITY`); в этом этапе реализуется только хранение в `ServerGC`.

## 12. Порядок реализации и тестов

1. Client snapshot model: `skin_snapshot.h/.cpp` (структуры + JSON) и `Inventory::CollectEquippedSkins()`.
2. Client → backend: `BackendClient::SetSkinSnapshot`, job `skin-snapshot` после регистрации поиска/усыновления; вызов из `ClientGC`.
3. Java: пакет `dev.csgogc.mm.skin` (DTO, `SkinSnapshotStore`), endpoint в `SearchApiController`, `SearchService.submitSkinSnapshot/purge`, `MatchRoster.skinSnapshots/skinMissing`.
4. Matchmaking attachment: `rosterForServer`.
5. Server delivery: парсер в `backend_client.cpp`, `Poller` skin-callback, `GCEvent::BackendSkins`, `ServerGC::OnBackendSkins`.
6. Тесты: Java (MockMvc: приём, чужой/устаревший request_id отклонён, агрегация в роcтере, cleanup по cancel/expire/dissolve/ended); C++ offline (`offline_tests/skinsync`: serialize → parse roundtrip). Игра не запускается.

---

# Implementation status (Phase A–C done, Phase D NOT implemented)

| Part | Status | Where |
|---|---|---|
| Client snapshot model | DONE | `csgo_gc/skin_snapshot.h/.cpp`, `Inventory::CollectEquippedSkins()` (`inventory.h/.cpp`) |
| Client → backend | DONE | `BackendClient::SetSkinSnapshot` + worker job `skin-snapshot` (`backend_client.h/.cpp`), calls in `ClientGC` (`gc_client.cpp`: MatchmakingStart and party-member adoption) |
| Backend endpoint / store | DONE | `POST /api/v1/matchmaking/skin-snapshot` (`SearchApiController`), `dev.csgogc.mm.skin.{SkinSnapshotRequest,SkinSnapshotStore}`, `SearchService.submitSkinSnapshot / purgeSkinSnapshots`, `SecurityConfig` (API key, like `/accepted`) |
| Match aggregation | DONE | `SearchService.rosterForServer` → `MatchRoster.skinSnapshots / skinMissing` (real players only) |
| Backend → Server GC | DONE | existing `GET /servers/roster` extended; `RosterFeed::Poller` skin callback → `GCEvent::BackendSkins` → `ServerGC::OnBackendSkins` (stores `MatchSkins()`) |
| Skin application to weapons | NOT IMPLEMENTED (Phase D) | — |
| P2P SOCache | NOT USED, not changed | — |

Tests run (no game launched): Java `mvnw test` 169 tests, 0 failures (10 new in `SkinSnapshotTest`); C++ offline `offline_tests/skinsync` all pass; `offline_tests/roster` controller/launch-args tests still pass; the DLL builds (`Build\release\csgo_gc\csgo_gc.dll`), not installed into the game.

Deviations from the proposed design: the absolute age limit of a snapshot is a constant (6 h), not a `backend.*` property; `skin_missing` is an array of account ids. Open: per-player authentication (§5), classic srcds without `-gc_mode` (§11).
