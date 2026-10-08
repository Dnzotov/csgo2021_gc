# Equipment Sync — research (read-only; ничего не реализовано)

Метки: **CODE** (проект `<repo>`), **SRC** (утёкший исходник `<cstrike15_src>`, возраст относительно сборки 1352 неизвестен — тела некоторых функций вырезаны «Removed for partner depot»), **DATA** (`csgo\scripts\items\items_game.txt` игры), **BIN** (строки/дескрипторы в `server.dll`/`client.dll`), **UNKNOWN**.
Менялся только этот файл. Игра/srcds не запускались, сборок и тестов не было, Git не использовался, файлы между `D:\` и GitHub-клоном не копировались.

## 0. Главный вывод

В Skin Sync выбранные *предметы с инвентаря* уже синхронизируются (их `equipped_state` = класс+слот). **Не синхронизируется второй вид выбора — «базовое оружие» без предмета в инвентаре**: клиент хранит его как `m_defaultEquips` — объекты `CSOEconDefaultEquippedDefinitionInstanceClient{item_definition, class_id, slot_id}` (SO type **43**). Именно так выбираются USP-S вместо P2000, M4A1-S вместо M4A4, CZ75/Tec-9/Five-SeveN в общем слоте и т. п. Phase D (`Inventory::BuildServerCache`) type 43 не отправляет, поэтому сервер использует базовое оружие слота (`mp_ct_default_secondary = weapon_hkp2000` → P2000). Прежний P2P-путь (`Inventory::BuildCacheSubscription(server=true)`) type 43 отправлял.
Следовательно Equipment Sync = **переносить `m_defaultEquips` игрока (+ уже переносимые equipped_state) и добавлять type 43 в то же `CacheSubscribed`, которое строит `ApplySkins`** — либо, если `server.dll` не примет type 43 внутри `CacheSubscribed`, отдельным инкрементальным `k_ESOMsg_UpdateMultiple`. Что именно примет `server.dll`, надо доказать live-экспериментом (см. §5, §7).

## 1. Что подтверждено

### 1.1 Клиентская сторона (выбор игрока)

| Утверждение | Источник |
|---|---|
| Игра шлёт выбор слота как `k_EMsgGCAdjustItemEquippedState` (`CMsgAdjustItemEquippedState{item_id,new_class,new_slot,swap}`) | `csgo_gc/gc_client.cpp` `ClientGC::AdjustItemEquippedState` (:390), `protobufs/base_gcmessages.proto:209` — CODE |
| Для **предмета инвентаря** `Inventory::EquipItem` добавляет `CSOEconItemEquipped{new_class,new_slot}` в `item.equipped_state` | `csgo_gc/inventory.cpp` `Inventory::EquipItem` (ветка `else`, `item.add_equipped_state()`) — CODE |
| Для **базового оружия** item_id = `def_index \| ItemIdDefaultItemMask (0xF<<60)`; `EquipItem` создаёт `CSOEconDefaultEquippedDefinitionInstanceClient{account_id,item_definition=def,class_id,slot_id}` в `m_defaultEquips` | `inventory.cpp` `IsDefaultItemId` (:29), `EquipItem` ветка default (~:635-650), `gc_const_csgo.h:12` — CODE |
| Снятие/замена слота идёт через `UnequipItem(class,slot)`: чистит `equipped_state` у предметов и **стирает** default equip слота (отправляя update с `item_definition=0`) | `inventory.cpp` `Inventory::UnequipItem(uint32_t,uint32_t,...)` (~:1590) — CODE |
| Хранится в `inventory.txt`: `items{...equipped_state{class:slot}}` и `default_equips{<def>{class_id,slot_id}}`; у тестового аккаунта `default_equips: 61 → class 3, slot 2` (USP-S в слоте secondary0 за CT) | `csgo_gc\inventory.txt` (игровая папка, только чтение), `Inventory::ReadFromFile/WriteToFile` (:170-195, :296-310) — CODE |
| В SO-кэш клиента для сервера type 43 попадал: `BuildCacheSubscription` пишет `m_defaultEquips` в объект `SOTypeDefaultEquippedDefinitionInstanceClient` **для обоих значений `server`** | `inventory.cpp:398-406`, `gc_const_csgo.h:74` — CODE |
| **Фактический выбор ≠ список инвентаря**: выбор = `equipped_state` предметов ∪ `m_defaultEquips`; `m_items` без `equipped_state` — лишь доступные предметы | `Inventory::CollectEquippedSkins` (`:` фильтр `equipped_state_size()`) vs `m_defaultEquips` — CODE |

### 1.2 Нумерация классов и слотов

- `class_id` = номер команды: 2 = T, 3 = CT, 0 = без команды (спрей/муз.кит/монета) — используется в `inventory.txt` (class 3) и в игре `GetItemInLoadout(GetTeamNumber(), slot)` (`cs_player.cpp:6093`, SRC).
- `slot_id` = индекс `LOADOUT_POSITION_*`, таблица `"player_loadout_slots"` в `items_game.txt:383` (DATA): 0 MELEE, 1 C4, 2–7 SECONDARY0–5, 8–13 SMG0–5, 14–19 RIFLE0–5, 20–25 HEAVY0–5, 26–31 GRENADE0–5, 32–37 EQUIPMENT0–5, 44–53 MISC0–9, 55 FLAIR0; `ItemSchema::LoadoutSlotGraffiti = 56` (`item_schema.h:179`, CODE). Это согласуется с тестовым default equip (61 → слот 2 = SECONDARY0).
- Соответствие предмет→слот: `item_sub_position` (`secondary0`, `rifle1`, `melee`…) и `used_by_classes` (в prefab: `"counter-terrorists" "1"` / `"terrorists" "1"`), общий слот у альтернатив — `item_shares_equip_slot` (DATA). Примеры DATA: слот SECONDARY0 — glock(4, T), hkp2000(32, CT), usp_silencer(61, CT); RIFLE1 — ak47(7), m4a1(16), m4a1_silencer(60); SECONDARY3 — fiveseven(3), tec9(30), cz75a(63); SECONDARY4 — deagle(1), revolver(64); MELEE — knife(42), knife_t(59). 44 записи `baseitem=1` (вкл. перчатки 5028/5029 и модели 5036/5037).
- Проект сейчас **не разбирает** `item_sub_position`/`used_by_classes` (в `item_schema.cpp` нет их разбора) — для валидации придётся добавить чтение (DATA есть) — CODE (отсутствие).

### 1.3 Серверная сторона (как игра использует выбор)

- Инвентарь игрока на сервере — `CCSInventoryManager::GetInventoryForPlayer(CSteamID)` → `CCSPlayerInventory`, привязка к игроку по SteamID (`cstrike15_item_inventory.cpp:601-628`, `CCSPlayer::Inventory()`; SRC). Это тот же ключ, что использует Phase D (owner SOID = SteamID64).
- Разрешение слота: `CCSPlayerInventory::GetItemInLoadout(team, slot)` (`cstrike15_item_inventory.cpp:1425`, SRC): (1) предмет инвентаря из `m_LoadoutItems[team][slot]` — **с проверкой, что слот предмета совпадает** («защита от обмана backend»); (2) иначе `FindDefaultEquippedDefinitionItemBySlot(team, slot)` — выбранный базовый def (если совпадает с базовым предметом слота — берётся базовый); (3) иначе базовый предмет команды. То есть **type 43 — штатный источник выбора базового оружия на сервере** (SRC).
- Type 43 обрабатывается в `CPlayerInventory::SOCreated`/`SOUpdated` (**incremental**-события): `AddEconDefaultEquippedDefinition` → `SetDefaultEquippedDefinitionItemBySlot(class, slot, def)` (`econ_item_inventory.cpp:1294, 1338-1341, 1423-1430`, SRC). Для `Subscribed/Resubscribed` в `SOCreated` стоит ранний `return` («everything in one place»), а сам `CPlayerInventory::SOCacheSubscribed` в утёкшем коде **вырезан** — как именно подписка применяет объекты type 43, из исходника не видно (UNKNOWN).
- Оружие выдаётся по **имени класса** (`mp_ct_default_secondary weapon_hkp2000`, server_log.txt) → `CCSPlayer::GiveNamedItem(name, …, pScriptItem=NULL)` вызывает `FindMatchingWeaponsForTeamLoadout(name, team, …)` и использует найденный econ-предмет, иначе генерирует базовый по имени (`cs_player.cpp:12386-12430`, SRC). Тело `FindMatchingWeaponsForTeamLoadout` **вырезано** (UNKNOWN), но смысл (loadout-предмет вместо базового) очевиден. Покупка: `HandleCommand_Buy_Internal` берёт `Inventory()->GetItemInLoadoutFilteredByProhibition(team, slot)` по слоту покупаемого оружия (`cs_player.cpp:7925`), в соревновательном режиме кэширует def слота при первой покупке (`CQMMPlayerData_t::m_mapLoadoutSlotToItem`, `cs_player.cpp:7945-7985`) и учитывает `InventoryRetrievedFromSteamAtLeastOnce()` — **инвентарь должен быть у сервера до первой покупки/спавна**.
- BIN: `server.dll` содержит дескриптор `CSOEconDefaultEquippedDefinitionInstanceClient` (8 вхождений строки, как и `client.dll`) — тип известен серверу. Что `server.dll` *подписывает* type 43, это не доказывает.

### 1.4 Что уже сделано Skin Sync (CODE)

`Inventory::CollectEquippedSkins` → `BackendClient::SetSkinSnapshot/QueueSkinSnapshot` → `POST /api/v1/matchmaking/skin-snapshot` (`SearchApiController.skinSnapshot`) → `SearchService.submitSkinSnapshot` (проверка живого поиска + `request_id`) → `SkinSnapshotStore` → `SearchService.rosterForServer` (`skin_snapshots`, `skin_missing` по `account_id` ростера) → `ParseRosterResponse` → `RosterFeed::Poller` skin-callback → `GCEvent::BackendSkins` → `ServerGC::OnBackendSkins`/`MatchSkins()` → `BeginAuthSession` hook → `GCEvent::ClientAuthenticated` → `ServerGC::ApplySkins` → `SkinSync::PlanForPlayer` → `Inventory::BuildServerCache` → `GCMessageWrite{k_ESOMsg_CacheSubscribed}` → `PostToHost`. Live-тест PASS (скин ножа).
Пробел для Equipment: `BuildServerCache` пишет только type 1 (+ persona type 2), **без type 43**; `PlanForPlayer` при отсутствии применимых предметов даёт пустой план, и `ApplySkins` тогда не шлёт ничего (`plan.items.empty()` → skipped) — игрок, у которого есть только default equips, не получил бы ничего.

## 2. Где клиент хранит выбранное снаряжение и когда читать

`Inventory::m_items[*].equipped_state` и `Inventory::m_defaultEquips` (`inventory.h`, private) — в памяти `ClientGC::m_inventory`, обновляются на `k_EMsgGCAdjustItemEquippedState` (в любой момент, включая очередь поиска). Читать безопасно на GC-потоке в `OnMatchmakingStart` (как Skin Sync, `gc_client.cpp` перед `SearchStarted`). Если игрок меняет loadout *после* старта поиска, snapshot устареет — решение в §4/§7.
Различия: «доступное» = `m_items`; «выбранное» = `equipped_state` ∪ `m_defaultEquips`; «реально в матче» = результат `GetItemInLoadout` + правила режима (`mp_*_default_*`, `mp_weapons_allow_*`, `mp_buy_*`, ограничения покупок) — вторая половина не синхронизируется и не должна.

## 3. Как данные попадут в backend (предложение)

Отдельный `equipment_snapshot`, **не меняя `skin_snapshot`**:
```json
POST /api/v1/matchmaking/equipment-snapshot
{"account_id":1050166997,"request_id":"…",
 "defaults":[{"def_index":61,"class_id":3,"slot_id":2}, …]}
```
- Только базовое оружие (`m_defaultEquips`), не предметы инвентаря: предметы остаются в `skin_snapshot` (там уже есть их `equipped`), поэтому дубликатов нет и контракт Skin Sync не меняется.
- Точка интеграции на клиенте: рядом с `BackendClient::SetSkinSnapshot` (`gc_client.cpp` `OnMatchmakingStart` + конструктор для party-member), новый `Inventory::CollectDefaultEquips()` (чтение `m_defaultEquips`), job `equipment-snapshot` в `backend_client.cpp` (тот же воркер, retry, лог `[EQUIP_SYNC]`).
- Backend: новый DTO `EquipmentSnapshotRequest` (валидация: ≤ 64 записей, `class_id ∈ {0,2,3}`, `slot_id` в 0..55, `def_index` > 0), `EquipmentSnapshotStore` (in-memory, жизненный цикл ровно как `SkinSnapshotStore`: привязка к `request_id`, `purge…`), проверка «живой поиск + `request_id`» как в `SearchService.submitSkinSnapshot`. Whitelist `def_index`↔`slot`↔`class` — по DATA (`baseitem=1`, `item_sub_position`, `used_by_classes`), загружаемому из `items_game.txt` на стороне srcds/клиента (backend items_game не имеет).

Альтернатива (меньше API, но смешивает контракты): добавить в существующий `skin-snapshot` необязательное поле `defaults[]` — **не рекомендуется** по требованию «не менять Skin Sync».

## 4. Roster

Минимум: в `GET /servers/roster` добавить `equipment_snapshots:[{account_id, steam_id64, defaults:[…]}]` и `equipment_missing:[account_id…]` (по `account_id`, не по индексу), по образцу `skin_snapshots/skin_missing`. Старый srcds их игнорирует, новый — старый backend без полей (парсер уже терпимо относится к отсутствию, см. `ParseRosterResponse`, проверено тестом `old backend / no skin fields`). Доставка на сервер: расширить `RosterFeed::Poller` отдельным callback (как skin-callback), чтобы решение `Controller` не затрагивалось.

## 5. Как сервер должен применить выбор

**Рекомендуемый механизм (кандидат A, требует live-подтверждения):** добавить в то же `CMsgSOCacheSubscribed`, которое строит `ApplySkins`, объект `SOTypeDefaultEquippedDefinitionInstanceClient` (type 43, по одному `object_data` на запись; `account_id=<аккаунт>`, `item_definition`, `class_id`, `slot_id`), ровно как это делал `Inventory::BuildCacheSubscription` (`inventory.cpp:398-406`). Порядок вызовов тот же: `BeginAuthSession` → `ClientAuthenticated` → `ApplySkins` (одно сообщение: type 1 + type 2 + type 43) → `PostToHost`. Один момент доставки = тот же, что доказано live для скинов; гонки «второго» сообщения нет.
**Запасной (кандидат B):** если `server.dll` проигнорирует type 43 в подписке — сразу после `CacheSubscribed` послать `k_ESOMsg_UpdateMultiple` с теми же объектами type 43 (формат уже есть: `Inventory::AddToMultipleObjects`, `ClientGC::AdjustItemEquippedState` отправляет именно его серверу), т. к. `CPlayerInventory::SOUpdated` type 43 обрабатывает явно (SRC :1423).
**Доказать нельзя без live:** какой из двух путей реально принимает `server.dll` сборки 1352 (SRC вырезан в нужном месте). Нельзя утверждать, что `CacheSubscribed` «автоматически подходит»: для type 1 это доказано live (Skin Sync), для type 43 — нет.
**Не использовать:** консольные команды, `give`, подмену оружия, фиксированный список (`mp_*_default_*`), патчи `server.dll` — в этом нет нужды: игра сама разрешает слот через инвентарь (§1.3).
**Перезапись стандартной выдачей:** выдача идёт после спавна через `GiveNamedItem`→loadout; инвентарь должен прийти **до первого спавна и до первой покупки** (иначе кэш `m_mapLoadoutSlotToItem` в соревновательном режиме зафиксирует базовый def). Поэтому применять в `ClientAuthenticated` (до entity), как Skin Sync; повторное применение для уже подключённого игрока допустимо один раз (`m_skinsApplied`-подобный флаг), смена после первого спавна не гарантируется.

## 6. Что можно переиспользовать и что нет

| Переиспользовать | Нельзя / надо менять |
|---|---|
| `request_id`-привязка и lifecycle store (`SearchService.purgeSkinSnapshots`), `QueueSkinSnapshot`-воркер с retry, парсер ростера, `Poller` skin-callback-схема, `ClientAuthenticated` хук, `PostToHost`-доставка, ownership-проверка по account | `PlanForPlayer` отбрасывает «игрока без предметов» и `ApplySkins` не шлёт пустой план — для Equipment нужен отдельный план (defaults могут быть при `items.empty()`) |
| Формат ошибок/логирования (`[SKIN_SYNC]` → `[EQUIP_SYNC]`) | Проверка «item_id принадлежит аккаунту» неприменима — default equips не имеют item_id; нужна проверка по таблице def↔slot↔class |
| Offline-тесты: `skinsync_test.cpp`, `skin_apply_test.cpp` как образец (реальные protobuf + `items_game.txt`) | `SkinSnapshotRequest.Item.equipped` `@NotEmpty` и `ApplyPlan`-правила — для предметов; defaults — отдельные DTO |

## 7. Неизвестное и риски

1. **Принимает ли `server.dll` type 43 в `k_ESOMsg_CacheSubscribed`** — UNKNOWN (SRC вырезан, BIN доказывает лишь знание типа). Определяется одним live-экспериментом (§8, этап 3).
2. Точная семантика `FindMatchingWeaponsForTeamLoadout` (вырезана) и порядок выдачи стартового пистолета — UNKNOWN; влияет на то, будет ли USP-S выдан как стартовый (а не только при покупке).
3. Старый/новый билд: SRC старше 1352 — возможны отличия (проверяем live).
4. Loadout, изменённый **после** `MatchmakingStart`, не попадёт в snapshot (нужно переотправлять при `AdjustItemEquippedState` во время живого поиска — предлагается: `SetEquipmentSnapshot` повторно + `QueueEquipmentSnapshot`, если `m_activeSearch.active`).
5. Валидация def↔slot↔class: в проекте нет разбора `item_sub_position/used_by_classes` — потребуется (DATA есть); без неё злонамеренный клиент может прислать оружие не того слота (игра частично защищает предметы инвентаря, но для default equips защита в SRC не видна).
6. Соревновательный кэш покупок (`CQMMPlayerData_t`) — снаряжение должно быть на сервере до первой покупки; поздно пришедший snapshot не исправит кэш.
7. Классы/слоты вне оружия (перчатки 5028/5029, агенты 5036/5037, грейды/снаряжение, C4): включены в таблицу DATA, но поведение сервера не изучено — **не входят** в первый этап.
8. Нет доказательств, что при отсутствии snapshot игра ведёт себя безопасно кроме как «базовые предметы слота» (`GetItemInLoadout` fallback, SRC :1432-1450) — это и есть требуемый явный fallback: без snapshot **ничего не отправляется**, сервер выдаёт базовое оружие режима; никакой «USP-S по умолчанию».

## 8. План реализации

1. **Клиент:** `Inventory::CollectDefaultEquips()`; `SkinSync`-подобный `EquipSync` (отдельные `equipment_snapshot.h/.cpp`: структура, JSON, `PlanForEquipment`); `BackendClient::SetEquipmentSnapshot` + job `equipment-snapshot`; повторная отправка при изменении loadout во время поиска.
2. **Backend:** DTO + валидация, `EquipmentSnapshotStore`, endpoint `POST /api/v1/matchmaking/equipment-snapshot`, `SecurityConfig`, `rosterForServer` (`equipment_snapshots/equipment_missing`), purge; **Skin Sync не трогаем**.
3. **Сервер (эксперимент A):** `Poller` equipment-callback → `GCEvent::BackendEquipment` → `ServerGC::OnBackendEquipment`/`MatchEquipment()`; в `ApplySkins` (или соседнем `ApplyLoadout` на том же `ClientAuthenticated`) добавить type 43 в то же `CacheSubscribed`; полная валидация (§7.5); логи `[EQUIP_SYNC]`. Live: тестовый аккаунт уже имеет `default_equips: 61 → class 3 slot 2` → ожидается USP-S у CT-игрока, P2000 у другого игрока без такого выбора.
4. Если A не сработал — **B** (`k_ESOMsg_UpdateMultiple` после подписки), тот же код сборки.
5. Расширение: T-слоты, rifle (M4A1-S), общие слоты (CZ75/Tec-9/Five-SeveN), затем остальное.

## 9. План тестирования

Offline (как в Phase D): план `PlanForEquipment` (матч по account/SteamID64, отбраковка неизвестного def, чужого slot/class, ≤64, повторы), реальные protobuf + `items_game.txt` (`build_apply.bat`-образец): `BuildServerCache` → type 43 читается обратно как `{def 61, class 3, slot 2}`; JSON roundtrip; Java `MockMvc`: приём, 404/409/400, агрегация по `account_id`, cleanup (cancel/expire/match end), `equipment_missing`; регрессия `SkinSnapshotTest`. Live: два клиента (или клиент + бот-аккаунт) с разным выбором (USP-S vs P2000, M4A1-S vs M4A4) — каждому игроку свой пистолет в начале раунда и в меню покупки; затем смена выбора в очереди поиска; отсутствие snapshot → базовое оружие режима.

## 10. Критерии готовности

1. В логе srcds для каждого игрока: `[EQUIP_SYNC] player matched`, перечень `class/slot/def`, `application result=posted (type 43 ×N)`.
2. Два игрока с разным выбором получают **разное** оружие в одинаковом слоте/команде (визуально и по `weapon_*` в `cl_showpos`/консоли сервера).
3. Игрок без snapshot получает базовое оружие режима, не «чужой» loadout.
4. Skin Sync не регрессирует (нож 507/paint 38 по-прежнему отображается; `SkinSnapshotTest` и `skin_apply_test` проходят).
5. Выбранное предмет-оружие со скином (type 1) и выбранное базовое оружие (type 43) в одном матче не конфликтуют (слот с предметом инвентаря имеет приоритет в `GetItemInLoadout`).

## Итог

- **Можно ли на существующей архитектуре:** да — транспорт, ростер, хуки и доставка `CacheSubscribed` готовы; нужен второй snapshot с default equips и расширение построения SOCache.
- **Подтверждённый механизм:** выбор базового оружия хранится как type 43 и штатно учитывается `GetItemInLoadout` (SRC); подтверждён live только путь type 1 в `CacheSubscribed`. Лучший кандидат — type 43 в том же `CacheSubscribed`; запасной — тот же объект в `k_ESOMsg_UpdateMultiple`.
- **Решить до кодирования:** (1) принимает ли `server.dll` type 43 в подписке (эксперимент); (2) отдельный endpoint или поле — рекомендую отдельный `equipment-snapshot`; (3) источник таблицы def↔slot↔class для валидации (разбор `items_game.txt` на srcds); (4) политика переотправки при смене loadout во время поиска; (5) что делать, если snapshot пришёл после первого спавна (рекомендация: применить один раз, но не гарантировать).
- **Минимальный первый этап:** только базовые пистолеты/винтовки CT и T из `m_defaultEquips` (`class_id` 2/3, `slot_id` 2–7 и 14–19), без перчаток/агентов/снаряжения: клиентский сбор → `equipment-snapshot` → roster → type 43 в `CacheSubscribed` → live-проверка пары USP-S / P2000 на двух разных аккаунтах.
