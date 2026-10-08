# Skins data flow (фактический путь данных и место потери)

```
inventory.txt (csgo_gc\)                         [YES, CODE/FILE VERIFIED]
   │  Inventory::Load
   ▼
ClientGC (csgo.exe) m_inventory                  [YES, CODE VERIFIED]
   │
   │  ── ветка MATCHMAKING (skins НЕ участвуют, так и задумано) ──
   │  MatchmakingStart 9101 ─► Java backend (HTTP, account_id/mode/maps)
   │  backend ─► сервер 146.158.123.140:27015, match m-dc77ac9b
   │  GC ─► клиент: MatchmakingGC2ClientReserve 9107 (x2)
   │  клиент ─► srcds: UDP 0x21/0x25 (stage 1,2)  awaiting=0 total=10   [LOG VERIFIED]
   │  srcds: Q-резервация ReserveServerForQueuedGame result: 1          [LOG VERIFIED]
   │  клиент: "Connecting to public(146.158.123.140:27015)... Connected" [LOG VERIFIED]
   │
   │  ── ветка INVENTORY (единственный путь предметов) ──
   ▼
srcds engine: BeginAuthSession(ticket, steamID)  ──hook──► NetworkingServer::ClientConnected
   │   SendMessageToUser(k_EMsgNetworkConnect + ticket) по ISteamNetworkingMessages
   │   (сервер — АНОНИМНЫЙ GS A:1:3032982555:51777 = steamid:90294376102077467,
   │    слушает 192.168.1.150:27015, публичный IP 146.158.123.140)       [LOG VERIFIED]
   ▼
✗✗✗  P2P-сессия сервер→клиент НЕ устанавливается  ✗✗✗
     srcds:  "[GC] OnSessionFailed: Timed out attempting to connect"     (server_log:507)  [LOG VERIFIED]
     клиент: "Ignoring P2P signal from 'steamid:90294376102077467',
              unknown remote connection #1798891437"                      (console.log, после Connected) [LOG VERIFIED]
   ╳
NetworkingClient::HandleMessage(k_EMsgNetworkConnect)  — не вызван      ("sending socache" = 0)       [LOG VERIFIED]
ClientGC::HandleSOCacheRequest                          — не вызван      [INFERRED из отсутствия строки]
NetworkingServer::ReceiveMessage / ServerGC::HandleNetMessage — не вызван ("HandleNetMessage" = 0)    [LOG VERIFIED]
   ╳
server.dll не получает k_ESOMsg_CacheSubscribed (items игрока)            [INFERRED]
   ╳
weapon entity без skin / skin не реплицируется                            [UNKNOWN, не наблюдалось]
```

## Где теряются данные

**Компонент:** Steam P2P транспорт (`ISteamNetworkingMessages`) между **srcds → csgo.exe**, ещё до какой-либо логики csgo_gc. Данные skin (item 2) не «искажаются» — они **ни разу не отправляются**, потому что клиент не получает `k_EMsgNetworkConnect`, по которому начинает отправку SO-кэша.

## Подтверждение по нескольким прогонам (перепроверено по существующему `console.log`)

Последний прогон (20:50): 0 × `sending socache`, 1 × `OnSessionFailed` на сервере, `Ignoring P2P signal …` на клиенте.
Накопительная статистика по `console.log` (47 168 строк, все запуски): `sending socache` — 6 вхождений, `Ignoring P2P signal` — десятки. Прежнее исследование (`RESEARCH_FINDINGS.md` §57.5) насчитало 24 подключения к srcds, из них **5 с рабочим SO-кэшем — все classic-серверы без `-gc_mode`**, остальные без inventory. Счёт 5/24 я не пересчитывал построчно (LOG VERIFIED частично: итоговые счётчики совпадают по порядку величины, 6 против 5 — разница в одном вхождении, возможно из-за последующих прогонов/listen-server).

## Что «тестовый» прогон НЕ доказывает

- Что именно ломает P2P (см. SKINS_ROOT_CAUSE.md, «Что не доказано»).
- Что skin после успешной доставки SO-кэша будет виден — этот участок (D/E) ни разу не наблюдался в имеющихся логах.
