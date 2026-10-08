# Skin sync Phase D — server-side application

Метки: CODE VERIFIED / OFFLINE TESTED / OUTSIDE PROJECT OBSERVABILITY (server.dll) / OPEN.
**Phase D live-tested: PASS** (проверено пользователем в игре: контрольный предмет item_id 9640101589, def_index 507, paint_kit 38, paint_seed 41, wear ≈ 0, слот CT/нож появляется на сервере). Приведённые ниже offline-проверки выполнялись до live-теста; в игровую папку при подготовке ничего не копировалось.

## 1. Исследованный путь `player → item → paint kit → rendered weapon`

- **Как `server.dll` получает предметы игрока (CODE VERIFIED):** у проекта нет доступа к entity/netvar/offset'ам `server.dll`. Единственный механизм, который он использует для инвентаря игрока, — GC-сообщение `k_ESOMsg_CacheSubscribed` (SO-кэш владельца-SteamID), которое проект отдаёт хосту через `ISteamGameCoordinator` (`PostToHost(HostEvent::Message, …)`, `steam_hook.cpp`). Раньше это сообщение приходило от клиента по P2P (`ServerGC::HandleNetMessage` → тот же `PostToHost`). Из `equipped_state` + атрибутов (6 paint kit, 7 seed, 8 wear, 80/81 StatTrak, 111 имя, 113+ стикеры) сам `server.dll` строит предметы игрока и оружие со скином — это штатный путь игры (так работают скины в classic-прогонах, где SOCache по P2P доходил: `RESEARCH_FINDINGS.md` §57).
- **BETA / clean_gc:** каталогов с такими именами в `D:\csgo2021_gc` нет. Найденная рядом копия `C:\Users\Administrator\Downloads\csgo_gc-master` (upstream) использует тот же механизм (`gc_server.cpp`: валидация `k_ESOMsg_CacheSubscribed` и передача хосту) — подтверждает выбор; ничего из неё не копировалось.
- **Вывод:** внедрять скин в weapon entity не нужно и нельзя без хардкодных offset'ов. Правильное место — подать `server.dll` тот же SOCache, но построенный из `MatchSkins()` (данные backend), а не из P2P. Хардкодных offset'ов нет: `offset/source/binary/why required` — не применимо.

## 2. Реализация

```
BeginAuthSession OK (steam_hook.cpp) ──► GCEvent::ClientAuthenticated(steamId)
                                           ServerGC::OnClientAuthenticated → ApplySkins(steamId)
MatchSkins() (Phase C: roster skin_snapshots) ─► SkinSync::PlanForPlayer(steamId)        [skin_snapshot.cpp, без protobuf]
     игрок: account = steamId & 0xFFFFFFFF И steamid64 из snapshot (не индекс ростера)
     предмет принят, если: equipped, def_index≠0, item_id принадлежит аккаунту (ComposeItemId), не default-id, wear 0..1, ≤64
 ─► Inventory::BuildServerCache(schema, plan, level)                                      [inventory.cpp, protobuf + ItemSchema]
     CSOEconItem: id, account_id, def_index, quality, rarity, attr 6/7/8, 80/81, 111, 113+, equipped_state = слоты из snapshot
     + persona data; CMsgSOCacheSubscribed{owner_soid = steamId64}
 ─► GCMessageWrite{k_ESOMsg_CacheSubscribed} ─► PostToHost(HostEvent::Message)             → server.dll
```
- Снапшот, пришедший позже подключения игрока, применяется при доставке (`OnBackendSkins`), один раз на соединение; `HandleClientSOCacheUnsubscribe` (EndAuthSession) сбрасывает состояние игрока.
- P2P/SOCache/NetworkConnect не используются и не менялись. Java backend и протокол не менялись.
- Логи (`[SKIN_SYNC]`): `applying snapshot`, `player matched`, `applying item`, `def_index=`, `paint_kit= paint_seed= paint_wear=`, `equipped_slot=class N slot N`, `item rejected`, `application result=posted … / skipped (…)`.
- `weapon entity found` / `attribute update sent`: **OUTSIDE PROJECT OBSERVABILITY** — их печатает не проект; наблюдать нужно визуально (и, по желанию, консолью srcds).

## 3. Что проверено offline

| Тест | Что покрывает | Результат |
|---|---|---|
| `offline_tests/skinsync/build.bat` (`skinsync_test`) | snapshot → payload → разбор; план применения: матч по account+steamid (игрок не первый в списке), отказ чужому/без snapshot/другому steamid, отбраковка unequipped / чужой item id / default id / wear>1 / без def_index, слот CT=3/0 из snapshot, paint 38 (не 99 соседа), seed 41, wear≈0 | ALL PASSED |
| `offline_tests/skinsync/build_apply.bat` (`skin_apply_test`, реальные protobuf + `items_game.txt`, объекты сборки DLL; работает в папке игры только на чтение) | `BuildServerCache` → `CMsgSOCacheSubscribed` → `GCMessageWrite` (кадр, который уходит в `server.dll`) → разбор: owner SOID = SteamID64, item id/account, def_index 507, equipped_state {3,0}, attr 6/7/8 читаются схемой как 38 / 41 / ~1e-6, StatTrak, имя, стикер, 2 слота, persona data | ALL PASSED |
| `offline_tests/roster` | регрессия | controller / launch-args PASS |
| Java `mvnw test` | 169 тестов (Phase A–C), не менялись | 169/169 (до Phase D) |

Нельзя проверить offline: **принятие SOCache самим `server.dll` и рендер скина** (live-тест). Допущение (INFERRED до live-теста, подтверждено live-тестом): `server.dll` примет подписку, пришедшую сразу после `BeginAuthSession`, так же, как приходящую по P2P несколько секунд спустя; если окажется, что нужна задержка — это единственная настройка, которую придётся менять (место: `ServerGC::OnClientAuthenticated`).

## 4. Файлы

Изменены: `csgo_gc/{skin_snapshot.h,skin_snapshot.cpp,inventory.h,inventory.cpp,gc_shared.h,gc_server.h,gc_server.cpp,steam_hook.cpp}`, `offline_tests/skinsync/skinsync_test.cpp`. Новые: `offline_tests/skinsync/{skin_apply_test.cpp,build_apply.bat}`, этот файл.

## 5. Артефакты и ручная установка (ничего не копировалось)

- DLL: `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll` → в `C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\csgo_gc\csgo_gc.dll` (клиент и srcds используют один файл).
- Backend jar: `D:\csgo2021_gc\java-backend\target\matchmaking-backend.jar` → запускающий скрипт игровой папки берёт `...\csgo legacy\backend\target\matchmaking-backend.jar`.
- В игровой папке стоят прежние csgo_gc.dll (21:49) и jar; backend с Phase A–C нужен новый jar (в старом нет `/skin-snapshot`).
- Что искать в логах: клиент — `[SKIN_SYNC] snapshot created/sent/accepted`; backend — `[SKIN_SYNC] snapshot accepted: account=… items=1`, `snapshot attached to match`; srcds — `snapshot delivered to server`, затем при подключении игрока `applying snapshot … player matched … application result=posted`.
