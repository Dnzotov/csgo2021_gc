# Skins pipeline — стадии и статус (этапы 3–4)

Тестовый предмет (реальные значения из `csgo_gc\inventory.txt`): **item id `2`, def_index `507` (Karambit), paint index `38`, seed `41`, wear `0.000001`, rarity 6, quality 99, экипирован class 3 (CT) slot 0.** Игрок: SteamID64 `76561199010432725`, AccountID `1050166997` (LOG VERIFIED: console.log 47063, server_log 535).

## Таблица критерия готовности

| # | Вопрос | Ответ | Метка | Доказательство |
|---|---|---|---|---|
| 1 | Skin exists before matchmaking | **YES** | CODE+FILE VERIFIED | `inventory.txt`: item 2, attr 6/7/8 заданы, `equipped_state` есть. Что клиентский UI показывает этот предмет в инвентаре — не наблюдалось (UNKNOWN) |
| 2 | Skin reaches GC (клиентский csgo_gc держит его) | **YES** | CODE VERIFIED | `Inventory` загружает файл; `BuildCacheSubscription(server=true)` включит item 2 (есть `equipped_state`). В логе прогона самой сборки кэша нет (лог не пишет) |
| 3 | Skin included in reservation | **NO — by design** | CODE VERIFIED + LOG VERIFIED | `MatchmakingGC2ClientReserve` не имеет полей предметов (proto:415); `Q…`-payload = `1:[acct]…` (server_log: `Q293a206f6c6c6548,…,1:[3e9846d5][fa4e0001]…`); backend `RosterEntry(accountId, fake)`. Резервация предметов не несёт и в оригинальном дизайне не должна — **это не точка потери** |
| 4 | Skin reaches server | **NO** | LOG VERIFIED | server_log: ни одной `HandleNetMessage: 76561199010432725, N bytes`; на клиенте нет `NetworkingClient: sending socache` (0 вхождений в последнем запуске); для сравнения в успешной сессии размер сообщения = 141 B (`RESEARCH_FINDINGS.md` §57.5, прежний прогон) |
| 5 | Server knows skin | **NO** | LOG VERIFIED / INFERRED | не пришёл SO-кэш ⇒ `server.dll` не получил `k_ESOMsg_CacheSubscribed` (путь в коде единственный). Прямого лога `server.dll` об отсутствии предметов нет |
| 6 | Weapon entity receives skin | **UNKNOWN** | — | Нет логов создания entity / `m_nFallbackPaintKit`. Без шага 5 получить skin нечем (INFERRED) |
| 7 | Client renders skin | **UNKNOWN** | — | Нет наблюдения/скриншота; пользователь сообщает, что скинов нет |

## Дифференциация CASE A–E

| Case | Статус |
|---|---|
| A. Skin нет в inventory/GC | **ОТВЕРГНУТ** (inventory.txt содержит экипированный предмет) |
| B. Skin есть в GC, но не попал в reservation | **Неприменим**: reservation не должна нести предметы (proto, backend) |
| C. Не доходит reservation → backend → server | **Неприменим** тем же аргументом; сама резервация/ростер работают (LOG VERIFIED: `ReserveServerForQueuedGame result: 1`, `awaiting=0 total=10`, `Connected to 146.158.123.140:27015`) |
| **Фактический разрыв** | **Между «player connection» и «player data / inventory loaded»**: P2P-канал сервер↔клиент не поднимается, SO-кэш не передаётся. Ближе всего к «C», но в звене `server ↔ client P2P`, а не в reservation |
| D. Сервер получил данные, но weapon без skin | **Не проверяется** — до D данные не доходят |
| E. Replication к клиенту | **Не проверяется** — по той же причине |

## Аудит GC-протокола (этап 4)

- `MatchmakingGC2ClientReserve` (9107): поля serverid, direct_udp_ip/port, reservationid, reservation (`…GC2ServerReserve`), map, server_address. Логи отправки (console.log 47065/47098) содержат только reservationid, map, адрес. Игроков/предметов/loadout нет. CODE+LOG VERIFIED.
- JSON backend↔GC: `{"account_id","game_type","mode","game_mode","maps","request_id"}` (console.log 47054); ответ — сервер, карта, match id, roster (account ids). Потери полей при JSON/Go: **неприменимо — backend на Java (Spring), не на Go; в репозитории Go-кода нет** (запрос в ТЗ описывал Go, фактический стек — C++ DLL + Java). CODE VERIFIED.
- Сериализация SO-кэша: protobuf `CMsgSOCacheSubscribed` (`gc_const_csgo.h`, `gc_message.cpp`). Фильтры при передаче: (1) на клиенте — только экипированные (`inventory.cpp:365`), (2) на сервере — `RemoveUnequippedItems` повторно + лимит 64 + проверка owner SOID. Whitelist по полям атрибутов нет — atribute 6/7/8 проходят как есть. CODE VERIFIED. Поля StatTrak/sticker/custom name/music kit/gloves: в `inventory.txt` примере их нет — UNKNOWN, что `inventory.cpp` их поддерживает; не проверялось, чтобы не расширять scope.
