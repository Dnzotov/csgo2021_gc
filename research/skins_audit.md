# Skins audit (этапы 1–2: код и логи)

Метки: `LOG VERIFIED` / `CODE VERIFIED` / `RUNTIME VERIFIED` / `INFERRED` / `UNKNOWN`.
Git не использовался. Код проекта не менялся. Прогон, на который опирается аудит: 2026-10-08, ~20:45–20:53 (+04:00).

## 1. Что в проекте относится к скинам

| Компонент | Файл | Роль | Метка |
|---|---|---|---|
| Хранилище inventory игрока | `csgo_gc/inventory.cpp/.h`, `csgo_gc/inventory.txt` (в папке игры `csgo_gc\inventory.txt`) | Items (`CSOEconItem`: def_index, attributes 6=paint index, 7=seed, 8=wear), `equipped_state`, `default_equips` | CODE VERIFIED |
| Сборка SO-кэша | `Inventory::BuildCacheSubscription(msg, level, server)` — `inventory.cpp:353` | `server=true` ⇒ в кэш попадают **только экипированные** предметы (`equipped_state_size() != 0`) + persona data + default equips. Не-серверный вариант добавляет GameAccountClient | CODE VERIFIED |
| Клиентский GC | `gc_client.cpp` (`HandleSOCacheRequest`, :221) | По событию `SOCacheRequest` строит SO-кэш (server=true) и кладёт в `HostEvent::NetMessage` | CODE VERIFIED |
| Транспорт client→server | `networking_client.cpp`, `networking_server.cpp`, `steam_hook.cpp` (:470-490, :1198, :1288, :1320) | `ISteamNetworkingMessages` (Steam P2P), канал `NetMessageChannel` | CODE VERIFIED |
| Серверный GC | `gc_server.cpp` (`HandleNetMessage` :228, `ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>` :190, `RemoveUnequippedItems` :160) | Валидирует SO-кэш клиента (owner == steamId, ≤64 предметов), пересылает в `server.dll` как `k_ESOMsg_CacheSubscribed` | CODE VERIFIED |
| Matchmaking (client) | `gc_client.cpp`, `mm_modes.cpp`, `backend_client.cpp` | `MatchmakingStart(9101)` → HTTP в Java backend → `MatchmakingGC2ClientReserve(9107)` | CODE VERIFIED |
| Резервация сервера | `test_accept.cpp` (`BuildQueuedReservationPayload`), `gc_server.cpp`, `reservation_keepalive.h` | Строка `Q<cookie>,<match>,1:[acct][acct]...` → `IVEngineServer::ReserveServerForQueuedGame` | CODE VERIFIED |
| Java backend | `java-backend/.../search/*`, `RosterEntry(accountId, fake)` | Подбор матча, назначение сервера, roster (только account_id) | CODE VERIFIED |
| Protobuf | `protobufs/cstrike15_gcmessages.proto:415` `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` | поля: serverid, direct_udp_ip/port, reservationid, reservation, map, server_address — **полей про предметы нет** | CODE VERIFIED |

Поиск по `java-backend/**/*.java`, `schema.sql`, `backend_client.cpp`, `server_roster.cpp`, `reservation_keepalive.h` слов item/skin/paint/inventory/loadout/weapon — **0 совпадений** (кроме не относящихся). Backend и резервация inventory не несут вообще. CODE VERIFIED.

Вывод по архитектуре: **скины не едут по цепочке `GC → matchmaking → reservation → server`**. Единственный путь предметов на игровой сервер — отдельный: `клиентский csgo_gc → SO-кэш → P2P → ServerGC → server.dll`. Reservation/backend отвечают только за «кто и на какой сервер».

## 2. Релевантные логи

```
file: C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\server_log.txt   (28 740 B, 20:53:25)
purpose: консоль srcds этого прогона (серверная часть csgo_gc, Platform::Print с префиксом [GC])
relevant timestamps: без меток времени; порядок строк — по файлу
relevant events: стр.500 "Client ... connected (89.127.222.175:35534)"; стр.507 "[GC] OnSessionFailed: Timed out attempting to connect";
                 стр.535 "[GC] HandleClientSOCacheUnsubscribe: 76561199010432725"; строк "HandleNetMessage" — 0

file: ...\csgo legacy\csgo\console.log   (4 204 390 B, 20:52:15; накопительный, 47 168 строк)
purpose: консоль клиента csgo.exe, включая [GC]-логи ClientGC (log_output=1)
relevant timestamps: последний запуск — строки 46799 (ClientGC spawned) … 47163 (ClientGC destroyed), t=+0 … +21.8 s от Match Found
relevant events: 47062-47063 backend assigned match m-dc77ac9b, 146.158.123.140:27015;
                 47064-47067 Sending MatchmakingGC2ClientReserve reservationid=293a206f6c6c6548;
                 47097-47098 второй 9107 (connect); 47131-47133 "Connecting to public(146.158.123.140:27015) ... Connected";
                 далее "Ignoring P2P signal from 'steamid:90294376102077467', unknown remote connection #1798891437";
                 строки "NetworkingClient: sending socache" в этом прогоне — отсутствуют

file: ...\csgo legacy\backend_log.txt  (12 682 B, 20:53:42)
purpose: лог Java backend
relevant events: 20:50:32 match m-dc77ac9b создан; 20:50:42 RESERVED 146.158.123.140:27015; 20:50:46 server armed the roster;
                 20:50:51 everybody accepted; нигде нет данных об items (backend их не знает)

file: ...\csgo legacy\csgo_gc\inventory.txt  (349 B, 20:52:15)
purpose: inventory игрока — источник правды для SO-кэша
relevant events: единственный item "2": def_index 507, quality 99, rarity 6, attr 6=38 (paint), 7=41 (seed), 8=0.000001 (wear),
                 equipped_state { "3" "0" } (class 3 / slot 0); default_equips 61 (class 3 / slot 2)

file: ...\csgo legacy\csgo_gc\config.txt  (20:45) — backend_url http://127.0.0.1:8080/, log_output 1
file: ...\csgo legacy\bin\logs\connection_log_27015.txt — только Steam-логон анонимного GS [A:1:3032982555:51777] (20:49:45) и LogOff 20:53:16; к скинам не относится
file: ...\csgo legacy\bin\logs\{configstore,content,stats}_log.txt, csgo\backup_round*.txt — к скинам не относятся (просмотрены по именам/метаданным; backup_round — 21 сен, другой прогон)
file: ...\csgo legacy\gc_log.txt — отсутствует (log_output=1 ⇒ файл не пишется; ср. config.txt)
```

Дополнительный источник: `RESEARCH_FINDINGS.md` §57 (счёт по накопительному `console.log` за прошлые прогоны) — используется только как **прежнее исследование**; ключевые цифры в нём перепроверены ниже, где возможно.
