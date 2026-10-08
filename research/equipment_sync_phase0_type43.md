# Equipment Sync — Phase 0: SO type 43 внутри `k_ESOMsg_CacheSubscribed`

Production-код **не менялся**. Добавлены только тестовые файлы `offline_tests/skinsync/{type43_experiment_test.cpp, build_type43.bat}` и этот документ. Игра/srcds не запускались, Git не использовался, GitHub-клон не трогался.

## 1. Три уровня доказательств

| Уровень | Что доказывает | Статус |
|---|---|---|
| 1. Сериализация | type 43 корректно формируется и лежит в `CacheSubscribed` рядом с объектами Skin Sync | **PASS offline** (`type43_experiment_test`) |
| 2. Доставка | сообщение уходит по существующему серверному пути (`PostToHost` → `server.dll`) | не проверено: путь тот же, что у Skin Sync (доказан live), но для сообщения с type 43 — только live |
| 3. Применение | `server.dll` принимает type 43 и штатно разрешает выбранное базовое оружие | **UNKNOWN — только live** |

Уровень 3 не объявляется пройденным на основании уровней 1–2.

## 2. Подтверждённые структуры type 43

- Определение: `protobufs/base_gcmessages.proto:425` `CSOEconDefaultEquippedDefinitionInstanceClient { account_id=1, item_definition=2, class_id=3, slot_id=4 }`, все `optional uint32`; id типа `SOTypeDefaultEquippedDefinitionInstanceClient = 43` (`csgo_gc/gc_const_csgo.h:74`).
- **Совпадение с `server.dll` (BIN, этот этап):** в `csgo\bin\server.dll` (смещение 0x7e23b0) лежит встроенный `DescriptorProto` этого сообщения; разобран вручную по байтам: поля `account_id`=1, `item_definition`=2, `class_id`=3, `slot_id`=4, все тип 13 (`uint32`), label optional; у `account_id`, `class_id`, `slot_id` стоит опция `key_field = true` (`80 a6 1d 01`), у `item_definition` — нет. То есть **ключ объекта = (account_id, class_id, slot_id)**, значение = `item_definition`; `account_id` обязателен для идентичности объекта. Это идентично схеме репозитория.
- В `server.dll` есть C++-класс (RTTI `.?AVCSOEconDefaultEquippedDefinitionInstanceClient@@`, 0xa4541c) рядом с `CSOEconItem`/`CSOEconGameAccountClient` — серверная сборка содержит shared-object-класс для этого типа, а не только proto. Это сильнее, чем «тип известен», но всё ещё не доказывает, что подписка его применяет.
- SRC (утёкший код): `k_EEconTypeDefaultEquippedDefinitionInstanceClient = 43` (`econ_item_constants.h:47`); `CPlayerInventory::SOCreated/SOUpdated` → `SetDefaultEquippedDefinitionItemBySlot(class, slot, def)`; `CCSPlayerInventory::GetItemInLoadout` использует `FindDefaultEquippedDefinitionItemBySlot` (см. `equipment_sync_research.md` §1.3). Тело `SOCacheSubscribed` в SRC вырезано.
- Таблица слотов целевой версии (DATA, `csgo\scripts\items\items_game.txt` `player_loadout_slots`) сверена тестом: `secondary0..5 = 2..7`, `rifle0..5 = 14..19`, `melee = 0`. `class_id` 2 = T, 3 = CT (`used_by_classes: terrorists / counter-terrorists`, цепочка prefab учтена).
- Реальные записи (вывод теста): USP-S `61` — slot 2, только CT; P2000 `32` — slot 2, CT; Glock `4` — slot 2, T; M4A1-S `60` / M4A4 `16` — slot 15, CT; AK-47 `7` — slot 15, T; CZ75 `63` — slot 5 (T и CT); Tec-9 `30` — T, Five-SeveN `3` — CT (общий slot 5).
- В `inventory.txt` тестового аккаунта уже есть `default_equips: 61 → class 3, slot 2`.

## 3. Где формируется `CacheSubscribed`

`csgo_gc/gc_server.cpp` `ServerGC::ApplySkins` (≈ :642-700): `SkinSync::PlanForPlayer` → `m_skinSchema` → `Inventory::BuildServerCache(*m_skinSchema, plan, GetConfig().Level(), message)` (:689) → `GCMessageWrite write{ k_ESOMsg_CacheSubscribed, message }` (:696) → `PostToHost(HostEvent::Message, …)` (:697). `Inventory::BuildServerCache` (`inventory.cpp:513`) сейчас добавляет объект type 1 (предметы) и type 2 (persona data), type 43 нет. Раньше type 43 писал `Inventory::BuildCacheSubscription` (`inventory.cpp:398-406`, P2P-путь).
Вызывается из `OnClientAuthenticated` (`BeginAuthSession` → `GCEvent::ClientAuthenticated`) и из `OnBackendSkins` для уже подключённых игроков.

## 4. Можно ли добавить type 43 без изменения Skin Sync

**Да.** Тест строит `CacheSubscribed` ровно как production (`BuildServerCache`), затем **добавляет третий объект** (`add_objects()` с `type_id = 43`) и проверяет, что объекты type 1/2 **байт-в-байт совпадают** с версией без type 43 (PASS). Расширение аддитивно: Skin Sync-код, DTO и контракт остаются прежними; меняется только место, где собирается сообщение (после `BuildServerCache`).

## 5. Offline-проверки (выполнены)

`offline_tests/skinsync/build_type43.bat` (нужен готовый `build_local.bat`; запускается в папке игры только на чтение `items_game.txt`) — **ALL PASSED**:
- сообщение 162 байта (Skin Sync отдельно — 108), парсится обратно как `k_ESOMsg_CacheSubscribed`; объектов на один больше;
- 3 записи type 43: `{61,3,2}` (CT USP-S), `{60,3,15}` (CT M4A1-S), `{4,2,2}` (T Glock) читаются обратно точно; пример байтов: `08d58de1f403 10 3d 18 03 20 02` (= account, def 61, class 3, slot 2), 12 байт;
- ключи (account_id, class_id, slot_id) уникальны;
- валидатор по реальным `items_game.txt` принимает 3 записи и отвергает: USP-S для T, AWP в пистолетном слоте, USP-S в `secondary1`, `class_id=0`, нож со скином (не base item), неизвестный def.
Плюс для следующего этапа: уже существующие `skinsync_test`/`skin_apply_test` не затронуты.

## 6. Что можно подтвердить только live

Уровень 2/3: (а) что `server.dll` не отвергает сообщение с type 43 (нет ошибок, Skin Sync-нож по-прежнему виден); (б) что стартовый пистолет CT-игрока — USP-S, а не P2000, без консольных команд. Offline этого не ответить: `server.dll` — чёрный ящик, SRC в нужной функции вырезан.

## 7. Минимальный live-эксперимент (описание, НЕ применено — нужно ваше подтверждение)

Требует изменения production-кода в одном месте и **не** хардкодит USP-S: значения берутся из аргумента командной строки srcds, по умолчанию выключено.

**Предполагаемые изменения (всего 1 файл):** `csgo_gc/gc_server.cpp`, `ServerGC::ApplySkins`, сразу после `Inventory::BuildServerCache(...)` и до `GCMessageWrite write{…}`:
1. прочитать `LaunchArgs::Value(Platform::CommandLine(), "-gc_experiment_type43")` (один токен без пробелов; формат `<account_id>=<def>:<class>:<slot>[,<def>:<class>:<slot>…]`, например `1050166997=61:3:2`);
2. если аргумент пуст, или `account_id` не равен `plan.accountId` → **ничего не делать** (путь не меняется; другим игрокам type 43 не добавляется);
3. иначе для каждой записи проверить диапазон (`class ∈ {2,3}`, `slot ∈ 2..7 ∪ 14..19`, `def > 0`), добавить объект type 43 с `account_id = plan.accountId` (тот же код, что в тесте `AddType43`), залогировать `[EQUIP_SYNC] experiment: type 43 appended account=… def=… class=… slot=…`;
4. после отправки добавить в существующую строку результата число type 43.
Никаких изменений в Java, roster, protobuf, Skin Sync-классах, `PlanForPlayer`, `BuildServerCache`. (~25 строк + одна строка `#include "launch_args.h"`, если не подключён.)

**Откат:** убрать блок (или просто не передавать `-gc_experiment_type43` — без аргумента код инертен); вернуть прежнюю `csgo_gc.dll`.

**Инструкция для вашего live-теста (после подтверждения и сборки):**
1. Установите новую `csgo_gc.dll` (клиент и srcds берут один файл) и прежний jar backend (менять не нужно). Прежние аргументы srcds + один новый: `-gc_experiment_type43 1050166997=61:3:2`.
2. Контроль без эксперимента: запустить без аргумента — CT-игрок получает **P2000** (базовый пистолет `mp_ct_default_secondary weapon_hkp2000`), скин ножа (507/38/41) виден как раньше.
3. С аргументом: зайти за **CT**. Ожидаемые строки srcds: `[SKIN_SYNC] … application result=posted …` и `[EQUIP_SYNC] experiment: type 43 appended account=1050166997 def=61 class=3 slot=2`.
4. Наблюдение: в начале раунда/после покупки пистолета CT-игрок держит **USP-S** (модель `v_pist_223`, глушитель; в таблице убийств иконка USP-S), нож со скином на месте. Другой игрок (другой аккаунт) без аргумента должен получить P2000.
5. Интерпретация: USP-S есть → уровень 3 для `CacheSubscribed` подтверждён, можно строить Equipment Sync; P2000 остаётся → type 43 в подписке не применён, следующий эксперимент — тот же объект в `k_ESOMsg_UpdateMultiple` после подписки (отдельное подтверждение). Нож пропал/ошибки в логе → откат.
Если зайти за T: определяющих записей нет (Glock базовый), результат не информативен — тестировать CT.

## 8. Файлы для следующего этапа (полная реализация, не сейчас)

Клиент: `inventory.h/.cpp` (`CollectDefaultEquips`), новый `equipment_snapshot.h/.cpp`, `backend_client.h/.cpp`, `gc_client.cpp`. Backend: новые DTO/store, `SearchApiController`, `SearchService`, `SecurityConfig`. Сервер: `server_roster.h/.cpp` (callback), `gc_shared.h`, `gc_server.h/.cpp`, `inventory.cpp` (type 43 в `BuildServerCache` или соседней функции), `skin_snapshot.cpp`-аналог плана экипировки, таблица def↔slot↔class из `items_game.txt` (тест уже содержит рабочий разбор).

## 9. Риски

- `server.dll` может игнорировать type 43 в подписке (SRC вырезан) — тогда запасной путь `k_ESOMsg_UpdateMultiple`.
- Стартовый пистолет выдаётся через `GiveNamedItem(name)` → `FindMatchingWeaponsForTeamLoadout` (тело вырезано); совпадение по имени класса `weapon_hkp2000` у P2000 и USP-S (в `items_game.txt` оба `item_class weapon_hkp2000`) делает выбор по слоту правдоподобным, но не доказанным.
- Эксперимент меняет состояние только для одного аккаунта при явном аргументе; при ошибках — откат аргумента/DLL. Данные не сохраняются.
- Применять нужно до первого спавна/покупки (кэш покупок в соревновательном режиме); эксперимент использует тот же момент, что Skin Sync.
