# Skins — серверная цепочка (этапы 6–7)

| Шаг | Что происходит (по коду) | Лог этого прогона | Метка |
|---|---|---|---|
| player connects | engine: `Client "…" connected (89.127.222.175:35534)` | server_log:500 | LOG VERIFIED |
| player authenticated | `ISteamGameServer::BeginAuthSession` → хук `steam_hook.cpp:470` → `NetworkingServer::ClientConnected` (добавляет в `m_clients`, шлёт `k_EMsgNetworkConnect`+ticket по P2P). Сервер: анонимный GS, «INSECURE… Authentication and VAC not requested» | `Initializing Steam libraries for INSECURE Internet server`, `Assigned anonymous gameserver Steam ID` | LOG VERIFIED (анонимность); факт вызова `ClientConnected` — INFERRED (успешный путь ничего не печатает; `OnSessionFailed` возможен только после первой отправки) |
| player data loaded | `ServerGC` не имеет собственного хранилища игроков; данные приходят только как SO-кэш от клиента (`HandleNetMessage`) | нет `HandleNetMessage` | CODE+LOG VERIFIED |
| inventory loaded | `ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>` → `PostToHost(HostEvent::Message, k_ESOMsg_CacheSubscribed)` → `server.dll` читает как «ответ GC» | не выполнялся | CODE VERIFIED; факт не выполнения — LOG VERIFIED |
| loadout resolved | внутри `server.dll` (бинарник Valve): определяет слот/класс по `equipped_state` предмета и `default_equips` | не наблюдаемо | UNKNOWN (исходники server.dll нет; дизассемблирование не делалось) |
| weapon created | `server.dll` | нет логов entity | UNKNOWN |
| skin applied | `server.dll` ставит `m_nFallbackPaintKit/Seed/Wear` из атрибутов 6/7/8 | нет логов | UNKNOWN |
| fallback default weapon | при отсутствии предмета — дефолтное оружие (поведение движка) | — | INFERRED |

Дополнительно по server_log (LOG VERIFIED):
- `[GC] ServerGC::HandleMessage: unhandled protobuf message k_EMsgGCCStrike15_v2_GiftsLeaderboardRequest` — `server.dll` к игроку уже «на связи» через GC-канал, но это не SO-кэш.
- `[GC] HandleClientSOCacheUnsubscribe: 76561199010432725` (стр.535) — вызывается из `EndAuthSession`; ничего не говорит о том, получал ли сервер подписку.
- `Initialized low level socket/threading support`, `SteamDatagramServer_Init succeeded`, `Set SteamNetworkingSockets P2P_STUN_ServerList to ''` затем `'162.254.195.66:3478'` — Steam-сеть сервера поднята.

## Сервер vs оригинальный CS:GO 2021 (этап 7)

- **Оригинал (INFERRED, из структуры кода):** srcds логинится в Steam GC, GC подписывает сервер на SO-кэш игрока при `BeginAuthSession`; `server.dll` получает `CMsgSOCacheSubscribed` через `ISteamGameCoordinator::RetrieveMessage`. Ни reservation, ни matchmaking предметов не несут.
- **csgo_gc:** GC нет ⇒ `csgo_gc.dll` на клиенте и на сервере имитируют GC; роль «GC → server SO-кэш» выполняет клиент, пушащий кэш по P2P (`gc_server.cpp:228`, комментарий `networking_server.cpp:80-81` прямо предупреждает: *«it's not uncommon for the connection to time out, in which case the player's socache never gets to the server»*). CODE VERIFIED.
- **Точка расхождения:** в оригинале канал сервер↔GC всегда доступен; у нас доставка зависит от прямой P2P-сессии сервер↔клиент, которая в данной конфигурации не поднимается.
- Бинарники (`server.dll`, strings, symbols) для сверки порядка загрузки loadout не анализировались — **UNKNOWN**; для найденной точки потери это и не требуется.
