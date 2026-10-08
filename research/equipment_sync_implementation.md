# Equipment Sync — динамическая реализация (type 43 из реального loadout игрока)

Основано на live-проверке статического эксперимента (type 43 `{account 1050166997, def 61, class 3, slot 2}` в `k_ESOMsg_CacheSubscribed` → `server.dll` выдал USP-S). Эксперимент `-gc_experiment_type43`, `equip_experiment.h` и его тесты удалены. Skin Sync (Phase A–D), `BuildServerCache`, авторизация, reservation и доставка roster не менялись по смыслу.

## Поток данных
```
ClientGC (Inventory::CollectDefaultEquips = m_defaultEquips)
  → EquipmentSync::Check (items_game.txt)            equipment_snapshot.*
  → BackendClient::SetEquipmentSnapshot → POST /api/v1/matchmaking/equipment-snapshot {account_id, request_id, entries[]}
  → Java: EquipmentSnapshotStore (по account_id, привязка к request_id) → GET /servers/roster: equipment_snapshots / equipment_missing
  → RosterFeed::Poller (EquipmentHandler) → ServerGC::TakePendingEquipment / m_matchEquipment
  → BeginAuthSession → ClientAuthenticated → ServerGC::ApplySkins
      SkinSync::PlanForPlayer + EquipmentSync::PlanForPlayer(m_matchEquipment, steamId, items_game table)
      Inventory::BuildServerCache (как раньше) → Inventory::AppendEquipmentCache (type 43) → GCMessageWrite → PostToHost
```
Источник на клиенте: `m_defaultEquips` — именно то, что создаёт экран loadout (`Inventory::EquipItem` для default-item id) и что сохраняется в `inventory.txt` (`default_equips`). Предмет со скином (обычный `CSOEconItem` с `equipped_state`) остаётся в Skin Sync и в type 43 не попадает: `EquipItem` снимает default-equip этого слота, когда в него ставится предмет.

## Что отправляется
`(item_definition, class_id, slot_id)`; class 2 = T, 3 = CT; слоты 2..7 (пистолеты) и 14..19 (винтовки). Правила (клиент, повторно сервер; backend проверяет только структуру, у него нет `items_game.txt`): предмет существует, `baseitem`, `item_sub_position` = slot, `used_by_classes` разрешает команду, class ∈ {2,3}, slot в допустимых диапазонах, `(class, slot)` не повторяется (дубликат неоднозначен: не применяется ни одна из записей), ≤ 24 записей. Конкретных предметов в коде нет — только таблица из `items_game.txt` (`EquipmentSync::LoadoutTable`).

## Когда отправляется snapshot
Тот же момент, что у Skin Sync: `SyncEquipmentSnapshot()` читает loadout перед `SearchStarted` (и в конструкторе `ClientGC` для участника party), а `BackendClient` шлёт POST сразу после успешной регистрации поиска, при повторной регистрации (рестарт backend) и при усыновлении поиска лидера party — для каждого реального участника отдельно. Надёжного события «loadout изменился» в клиентском коде нет, hook не придумывался: гарантия — loadout читается при каждом старте поиска; смена loadout во время поиска доходит при следующей (пере)регистрации. Polling нет.

## Backend
`equipment/EquipmentSnapshotRequest`, `EquipmentSnapshotStore`, `SearchService.submitEquipmentSnapshot / purgeEquipmentSnapshots`, `SearchApiController` (`POST /matchmaking/equipment-snapshot`), `SecurityConfig` (X-Api-Key как у skin-snapshot). 404 — нет живого поиска, 409 — другой `request_id` (устаревший/чужой), 400 — malformed/слишком большой/неверный slot/дубликат. Жизненный цикл как у skin-snapshot: замена новым, удаление при cancel, expiry, конце матча, TTL 6 ч. Roster: `equipment_snapshots[{account_id, steam_id64, received_at, entries[]}]`, `equipment_missing[account_id]`; ключ — account_id, виртуальные игроки пропускаются.

## Сервер
`RosterFeed::Poller` получил отдельный `EquipmentHandler`; если в одном ответе изменились и скины, и equipment, equipment отдаётся без собственного события, а событие `BackendSkins` забирает оба (одно применение с обоими, без гонки). Если изменился только equipment — событие `GCEvent::BackendEquipment`. Игрок сопоставляется по account_id (младшие 32 бита аутентифицированного SteamID64) **и** SteamID64 из snapshot (если он есть) — как в Skin Sync. Применяется один раз на соединение (`m_skinsApplied`), до первого spawn: тот же `CacheSubscribed`, который Skin Sync отправляет сразу после `BeginAuthSession`.
Игрок без skin-snapshot, но с equipment: `BuildServerCache` с пустым списком предметов (владелец + persona, как у игрока без предметов) + type 43. Игрок без equipment: сообщение побайтно как раньше. Нет snapshot → ничего не добавляется → штатный базовый loadout режима (P2000 для CT и т.д.).

## Логи ([EQUIP_SYNC])
Клиент: `snapshot created / sent / accepted`, `loadout entry not sent: …`. Backend: `snapshot received`, `snapshot stored`, `roster equipment attached to match`, `snapshot removed`. srcds: `roster equipment delivered …`, `player matched`, `entry rejected`, `applying default equip account= def= class= slot=`, `type 43 appended account= entries=`, `application result=posted: … (the game applies them, not confirmed here)`. Лог утверждает только отправку; применение сервером видно в игре.

## Тесты
- `offline_tests/skinsync/build_equipment.bat` (`equipment_sync_test`, требует собранный `build_local.bat` и читает `items_game.txt` из папки игры): валидатор (разрешённые/запрещённые комбинации), `Check`, JSON, парсинг roster, изоляция аккаунтов, повторная валидация на сервере, `CacheSubscribed` (Skin Sync объекты byte-identical, без equipment сообщение не меняется, type 43 разбирается обратно, ключи уникальны).
- Регрессии: `skinsync\build.bat`, `skinsync\build_apply.bat`, `roster\build.bat` (+ `controller_test`, `launch_args_test`); Java `EquipmentSnapshotTest` (9) + прежние (178 всего).

## Live-проверка (ручная)
Установка: `Build\release\csgo_gc\csgo_gc.dll` → `…\csgo legacy\csgo_gc\csgo_gc.dll` (клиент и srcds), `java-backend\target\matchmaking-backend.jar` → `…\csgo legacy\backend\target\` (backend перезапустить). srcds — **без** `-gc_experiment_type43`.
1. CT: в loadout выбрать USP-S → поиск → матч. В srcds: `[EQUIP_SYNC] … applying default equip … def=61 class=3 slot=2`, `type 43 appended`, `application result=posted`; в игре USP-S, нож со скином на месте.
2. Сменить на P2000 (или «по умолчанию» — убрать выбор) → новый поиск → P2000 (проверка, что берётся фактический loadout, а не прошлый).
3. CT винтовка M4A1-S, затем M4A4; T: пистолет (Glock/Tec-9/CZ75) и винтовка (AK-47 и т.д.) — каждый с отдельным поиском/стороной.
4. Без выбора вовсе (чистый loadout): ничего не добавляется, обычный P2000.
5. Изоляция двух игроков (A: USP-S/M4A1-S, B: P2000/M4A4) возможна только с двумя реальными аккаунтами; offline она доказана (`equipment_sync_test` §5); при одном аккаунте проверяется логом srcds: `players_with_snapshot`, `account=` в каждой строке и `account_id` в `type 43 appended`.
6. Нож со скином продолжает работать (Skin Sync, строки `[SKIN_SYNC]` как раньше).

## Откат
Вернуть прежние DLL (`Build\rollback_phaseD\csgo_gc.dll`, Skin Sync Phase D) и jar; старый backend просто не знает `equipment-snapshot` (клиент получит 404/401 и продолжит — «matchmaking is not affected»), а новый backend со старой DLL работает как раньше (equipment_missing для всех).
