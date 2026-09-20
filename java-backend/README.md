# csgo_gc matchmaking backend — этапы 1–3

Java-backend для `csgo_gc.dll`. Клиент (GC) только сообщает ему поиск, а **выбирает игровой сервер backend**:

```
CS:GO ──▶ csgo_gc.dll ──HTTP──▶ Java Matchmaking Backend
  ▲        (search / poll / cancel)   ├── Active Searches   (кто и что ищет, статус, назначенный сервер)
  │                                   ├── Matchmaker        (свободный сервер нужной категории и карты, сбор игроков)
  └── 9107 / Accept / QueueConnect ◀──├── Game Servers      (реестр srcds: категория, карта, enabled, состояние)
      (уже работающий код GC)         └── Admin Panel       (/admin, логин + пароль)
```

* **Этап 1:** приём и показ заявок, CRUD реестра серверов, админ-панель.
* **Этап 2 (этот):** matchmaker — заявка получает подходящий свободный сервер, сервер резервируется; для режимов
  с Accept матч собирается из `required_players` совместимых заявок; результат GC забирает опросом
  `GET /api/v1/matchmaking/search/{request_id}`; состояние серверов (AVAILABLE / RESERVED / BUSY) хранится здесь, а не
  в `config.txt` клиента. См. раздел «Подбор сервера».

**Чего здесь нет:** reservation на самом srcds (его по-прежнему делает GC: `G`/`Q`-резервация на стороне srcds),
heartbeat-модуль srcds (есть только API `POST /api/v1/servers/state` и ручное состояние в панели), party, рейтинг.

Стек: Java 21, Spring Boot 3.5, Maven, SQLite (`sqlite-jdbc`), Spring Security (сессия + CSRF), обычный
HTML/CSS/JS без сборки.

## Быстрый старт (Windows)

Нужен JDK 21. На этой машине портативный JDK лежит в `..\tools\jdk21` (папка в `.gitignore`); скрипты
берут `JAVA_HOME`, а если он не задан — `..\tools\jdk21`. Maven ставить не нужно (`mvnw.cmd`).

1. Создайте конфиг с учётными данными:
   ```
   copy config\application-example.properties config\application.properties
   ```
   и впишите `backend.admin.username`, `backend.admin.password`, `backend.api-key`.
   `config\application.properties` в `.gitignore` — реальные пароли в репозиторий не попадают.
   Учётных данных по умолчанию **нет**: без них backend не стартует.
2. Сборка (с тестами): `build.cmd`  (без тестов: `build.cmd -DskipTests`)
3. Запуск: `run.cmd`  → панель http://127.0.0.1:8080/admin

Вместо файла можно использовать переменные окружения (PowerShell):
```powershell
$env:BACKEND_ADMIN_USERNAME = 'admin'
$env:BACKEND_ADMIN_PASSWORD = '...'
$env:BACKEND_API_KEY        = '...'
.\run.cmd
```
Любой ключ можно передать и аргументом: `run.cmd --server.port=9090`.

## Конфигурация

Порядок: аргументы командной строки → переменные окружения → `config\application.properties` →
встроенные значения (`src/main/resources/application.properties`).

| Ключ | Переменная окружения | По умолчанию | Смысл |
|---|---|---|---|
| `server.address` | `SERVER_ADDRESS` | `127.0.0.1` | на каком адресе слушать (`0.0.0.0` — для доступа с другого ПК) |
| `server.port` | `SERVER_PORT` | `8080` | HTTP-порт |
| `backend.admin.username` | `BACKEND_ADMIN_USERNAME` | — (обязательно) | логин админа |
| `backend.admin.password` | `BACKEND_ADMIN_PASSWORD` | — (обязательно) | пароль админа (в памяти хранится как BCrypt) |
| `backend.api-key` | `BACKEND_API_KEY` | пусто | общий секрет GC → backend, заголовок `X-Api-Key`. Пусто = `search`/`cancel` открыты всем, кто достучится до порта (в логе предупреждение) |
| `backend.database-path` | `BACKEND_DATABASE_PATH` | `./data/matchmaking.db` | файл SQLite, каталог создаётся |
| `backend.stale-search-timeout` | `BACKEND_STALE_SEARCH_TIMEOUT` | `PT15M` | ищущая заявка, которую GC не опрашивал дольше этого (игра упала) → «Timed out» |
| `backend.reaper-interval` | `BACKEND_REAPER_INTERVAL` | `PT10S` | как часто проверяются устаревшие заявки |
| `backend.finished-search-retention` | … | `PT1H` | сколько завершённые заявки и матчи видны по «show ended» |
| `backend.matcher-interval` | … | `PT1S` | как часто matcher ищет сервер / игроков, даже если ничего нового не пришло |
| `backend.server-reservation-ttl` | … | `PT10M` | сервер, выданный завершённому матчу, остаётся RESERVED столько, потом снова AVAILABLE (пока нет heartbeat srcds) |
| `backend.accept-timeout` | … | `PT25S` | Accept-режимы: сколько у игроков полного матча на Accept с момента, когда они получили сервер (retail popup 20 с + время до его появления). Потом матч `CANCELLED` |
| `backend.server-release-cooldown` | … | `PT15S` | сервер, освобождённый из отменённого Accept-матча, не выдаётся столько (srcds снимает резервацию, fake-драйверу нужно 12 с) |
| `backend.assigned-search-timeout` | … | `PT2M` | назначенная заявка (WAITING_ACCEPT / READY_TO_CONNECT) через это время → Completed |
| `backend.required-players.<mode>` | … | competitive 10, wingman 4, dangerzone 16 | сколько игроков собирает матч Accept-режима. **Только тестовая среда:** пока недостающих участников даёт тестовый ростер srcds (`-gc_mode`) и нет fake-поисков backend, ставят `1`. С профилями Fake Players (панель) значение не нужно: число виртуальных игроков задаёт профиль, capacity режима остаётся 10/4/16 |
| `backend.roster-ack-timeout` | … | `PT25S` | Accept-матч на сервере, который сам читает ростер у backend (srcds новой сборки), отдаётся игрокам только после подтверждения «ростер взведён»; если подтверждения нет столько времени — игроки получают сервер всё равно |
| `backend.roster-poll-window` | … | `PT10S` | сервер, спрашивавший ростер не позже этого времени назад, считается «читающим ростер с backend» (старый srcds с локальным fake-драйвером ничего не спрашивает — его игроков никто не держит) |
| `backend.fake-players.enabled` | `BACKEND_FAKE_PLAYERS_ENABLED` | `true` | тестовый инструмент «Fake Players»; `false` (production) — API отвечает `409`, matcher fake-поиски не смотрит |
| `backend.fake-players.gather-window` | `BACKEND_FAKE_PLAYERS_GATHER_WINDOW` | `PT10S` | сколько собирающийся матч ждёт **других реальных игроков**, прежде чем определится число fake (значение, сохранённое в панели, важнее); отсчёт — от входа последнего реального игрока (или партии). Без окна первый игрок сразу получал полный матч (1 + 9 fake), а все, кто нажал Play через несколько секунд, оставались без матча (§66). `PT0S` — решать сразу (тест с одним игроком). Реальным игрокам стоит нажимать Play в пределах окна друг от друга |
| `backend.login.max-failures` / `failure-window` / `lock-duration` | … | `5` / `PT5M` / `PT5M` | блокировка формы входа по IP после неудачных попыток |
| `server.servlet.session.timeout` | … | `30m` | срок админ-сессии |
| `server.servlet.session.cookie.secure` | … | `false` | `true`, если панель отдаётся по HTTPS |

Длительности — ISO-8601 (`PT15M`, `PT30S`) либо `15m`/`30s`.

## Подбор сервера (этап 2)

**Категория заявки** = `game_type & 0xF` (Casual 7, Competitive 8, Wingman 10, Danger Zone 13, Deathmatch 6, Arms Race 4,
Demolition 5, Skirmish 12). **Карты** приходят от GC списком `maps[]` (клиент уже раскодировал `mapMask` из `game_type`
своими таблицами, backend их не дублирует и не угадывает по названию группы в UI): сервер подходит, если

```
server.enabled  AND  server.state == AVAILABLE  AND  server.category == категория заявки
AND  ( maps[] пуст  ИЛИ  server.map ∈ maps[] )
```

Заявка без `maps[]` (режим, у которого выбор карт не раскодируется, напр. Skirmish) подходит любому серверу категории;
сервер без карты подходит только такой заявке. Из подходящих берётся **дольше всего не назначавшийся** (`last_assigned_at`),
адрес-имя из реестра резолвится в IPv4 (GC нужен `direct_udp_ip`).

**Классические режимы** (Casual, Deathmatch, Arms Race, Demolition, Skirmish): игрока просто **направляют** на подходящий
`AVAILABLE` сервер, поиск сразу `READY_TO_CONNECT` с `assignment`. Сервер **не резервируется** и остаётся `AVAILABLE`
(на нём играют несколько человек; нагрузка распределяется по `last_assigned_at`, «занят» — только `BUSY` вручную или отчётом
сервера). Вместимость (`max_players`) backend пока не учитывает (нужен heartbeat srcds).

**Accept-режимы** (Competitive / Wingman / Danger Zone) — жизненный цикл матча, RESEARCH_FINDINGS.md §63:

```
SEARCHING → FORMING (набор, СЕРВЕР НЕ РЕЗЕРВИРУЕТСЯ) → FULL → READY (сервер RESERVED) → ACCEPTING → ACCEPTED → ENDED
                                                                          └── CANCELLED (сервер свободен, игроки и fake снова ищут)
```

* Первый поиск открывает матч `FORMING` **без сервера**; совместимые поиски (та же категория, пересечение выбранных карт
  непусто и есть сервер категории, который такую карту запускает) и fake-игроки присоединяются (`MATCHED`,
  `match.players / required_players`).
* Набрались `required_players` → `FULL`. Только теперь берётся свободный сервер (карта ∈ пересечению карт участников):
  условный `UPDATE … WHERE state='AVAILABLE'` + запись сервера в матч в одной транзакции — один сервер двум полным матчам не
  достанется. Свободного нет → матч остаётся `FULL` и ждёт (повтор каждую секунду).
* Сервер зарезервирован → `READY`. Если srcds сам читает ростер у backend, игроки ждут его подтверждения
  (`POST /servers/roster/ready`, до `backend.roster-ack-timeout`); потом все получают `WAITING_ACCEPT` и **один и тот же**
  `assignment`, матч → `ACCEPTING` с дедлайном `backend.accept-timeout` (`match.accept_deadline_at`).
* GC клиента, увидев `0x25 stage 2, awaiting 0`, шлёт `POST /api/v1/matchmaking/accepted {account_id, request_id}`. Backend
  проверяет, что игрок состоит в `ACCEPTING`-матче; когда приняли все реальные игроки — `ACCEPTED`, поиски `READY_TO_CONNECT`.
* Дедлайн истёк, либо игрок ушёл до Accept (отмена, замена, новый поиск, истёк) → `CANCELLED`: **все** реальные игроки снова
  `SEARCHING` (тот же поиск, тем же `request_id`, прежний `assignment` больше не выдаётся), fake-игроки снова `SEARCHING` и сами
  входят в следующий матч, сервер снова `AVAILABLE`, но `backend.server-release-cooldown` не выдаётся (srcds должен снять старую
  резервацию: он видит `404` на `GET /servers/roster` и делает Unreserve).
* Игрок, уже принявший, и ушедший (клиент шлёт `MatchmakingStop` при подключении) матч не отменяет.
* `ACCEPTED` ≠ подключён: дальше матч живёт до `server-reservation-ttl` или до нового поиска единственного игрока.
* `RESERVED`-сервер, который не держит ни один живой матч (оборванная передача), возвращается через минуту
  (`GameServerRepository.findReservedBefore`).

### Skirmish («War Games»): Arms Race и Demolition — RESEARCH_FINDINGS.md §54

В актуальном клиенте Arms Race и Demolition существуют **только** внутри Skirmish: клиент шлёт `eGame` = 12 (никогда 4 / 5), а маска
`game_type >> 8` — это не карты, а **выбранные режимы** (бит `1 << (id-1)`, id — из `items_game.txt` `skirmish_modes`: armsrace = 10 → `0x200`,
demolition = 11 → `0x400`, retakes = 12 → `0x800`). GC раскодирует маску и присылает `variants[]` — по элементу на выбранный режим
(`name`, `game_mode` из `items_game.txt`, `maps[]` = карты группы `mg_skirmish_<name>` из `gamemodes.txt`). Каждый вариант обслуживает **своя
категория серверов**: `gungameprogressive` → `armsrace`, `gungametrbomb` → `demolition`, остальное (Retakes, Flying Scoutsman, …) → `skirmish`.
Подходит сервер любого варианта (`категория варианта` И `server.map ∈ maps варианта`), из подходящих берётся дольше всех не назначавшийся.
Без `variants` (старый GC) Skirmish-поиск подходит любому серверу категории `skirmish`. `variants` для не-Skirmish поиска → `400 variants_not_allowed`.

**Статусы заявки:** `SEARCHING`, `MATCHED`, `WAITING_ACCEPT`, `READY_TO_CONNECT`, `CANCELLED`, `EXPIRED`, `REMOVED`, `COMPLETED`.
**Состояния сервера:** `AVAILABLE`, `RESERVED` (ставит только matcher), `BUSY` (админ или сам сервер). Сервер возвращается в `AVAILABLE`:
кнопкой Release / Set available в панели, отчётом сервера `state=AVAILABLE` (если он не RESERVED), по TTL
`backend.server-reservation-ttl`, а также **когда игрок, оставшийся один в своём матче, начинает новый поиск** (провал
Accept / подключения и «искать снова» — обычный случай, клиент сам не сообщает, что матч закончился).

`assignment` (в ответе поиска, когда матч полный):

```json
{ "match_id": "m-5b0cf1f7", "server_id": 1, "server_address": "192.168.1.150", "server_port": 27016,
  "map": "de_dust2", "accept_required": false, "required_players": 1,
  "players": [ { "account_id": 1050166997, "fake": false } ] }
```

`players` — состав матча: реальные игроки и виртуальные (`fake: true`, id `0xFA4E0000 + n`, n = порядок вступления; та же схема, что у
тестового ростера srcds). Старый GC поле игнорирует.

### Fake Players (тестовый инструмент, RESEARCH_FINDINGS.md §54, §67)

Админ-панель → **Fake Players** управляет наполнением подбора виртуальными игроками. Это **профили**, а не очередь:
профиль режима говорит, **сколько** fake-игроков получит матч этого режима. Настройки хранятся в SQLite и меняются из панели,
действуют на **следующий** матч, у которого fake определяются (уже решённый матч свой состав не меняет).

Два разных вопроса:

* **Кто в матче** — `gather-window` (панель → «Gather window», иначе `backend.fake-players.gather-window`, по умолчанию 10 с). Реальные игроки
  (и party) одного режима, нажавшие Play в пределах окна, попадают в один собирающийся матч (`MATCHED`, без сервера, без Match Found);
  каждый вошедший перезапускает окно.
* **Сколько fake** — профиль. Когда окно закончилось, берётся включённый профиль режима с наибольшим приоритетом (при равенстве — старейший), у
  которого карты совместимы с матчем и (если задан) есть свободный/существующий привязанный сервер. Матч получает
  `effectiveFake = min(count профиля, capacity − real)`. `capacity` режима (Competitive 10, Wingman 4, Danger Zone 16) не меняется и до него
  **не добирается**: 3 real + count 2 = матч из 5 (ростер srcds из 5), 3 real + count 7 = 10, 8 real + count 7 = 8 + 2 = 10 (лишние не используются,
  лог: «the rest is not used»). Count `0` — матч стартует только с реальными игроками после окна.

Поля профиля: режим, включён (ON/OFF), число fake (0…capacity−1), карты (пусто = любая; профиль ограничивает и карты матча, и сервер: играть можно
только на сервере с картой из пересечения карт игроков и профиля), сервер (необязательно: тогда матчи профиля идут только на нём),
приоритет. Профили складываются **не** суммой — выбирается один. Общий выключатель **Fake Players ON/OFF** (панель) и «нет включённого
профиля режима / нет подходящего по картам» означают обычный matchmaking: матч ждёт реальных игроков, fake не добавляются.
Виртуальные игроки Accept'а не имеют: матч ждёт каждого **реального** игрока (`match.real_players` / `match.accepted_players`).
Профиль сам матч не открывает и сервер не занимает.

API (админ-сессия + CSRF): `GET /admin/api/fake-searches` (`enabled` — инструмент включён на backend, `master`, `gather_window_seconds`,
`fake_searches[]` с `matches[]` — живые матчи, использующие профиль), `POST` / `PUT /admin/api/fake-searches[/{id}]`
(`mode`, `players`, `maps`, `enabled`, `priority`, `server_id`), `POST /{id}/enabled`, `DELETE /{id}`, `PUT /admin/api/fake-settings`
(`master`, `gather_window_seconds` 0…600). Состав матча (реальные + fake) видно во вкладке Matches, для srcds —
`GET /api/v1/servers/roster?address=&port=` (`required_players` там — размер ростера = real + fake, `capacity` — в `match.required_players` поиска).

**Ростер с backend на srcds (§55).** srcds с `-gc_mode` раз в секунду спрашивает этот эндпоинт (адрес и порт — то, под чем сервер записан в Game Servers:
`-backend_ip`/`-backend_port` в командной строке srcds, а без них его `-ip`/`-port`; игровой сокет всегда `-ip`/`-port`, `-backend_*` влияют только на
этот запрос и на `roster/ready`. Например `srcds.exe ... -ip 192.168.1.150 -port 27016 -backend_ip 146.158.123.140 -backend_port 27016 -gc_mode competitive`,
если сервер в панели записан как `146.158.123.140:27016`; при несовпадении эндпоинт отвечает 404 и srcds остаётся на legacy-ростере). Когда матч на его сервере `READY` (режим srcds, есть реальный игрок; размер любой: число fake задаёт профиль, §67),
srcds взводит резервацию именно с этим составом (настоящие AccountID и fake-id backend'а), доводит fake-участников до stage 1 и
шлёт `POST /api/v1/servers/roster/ready {address, port, match_id}`. **До этого подтверждения игрок остаётся `MATCHED`** (матч 10/10, `awaiting_server`)
и не получает `assignment` — поэтому первая же проверка резервации у клиента даёт `awaiting=0`. Для сервера, который ростер не спрашивает
(старый srcds), удержания нет. `required_players` — размер матча (`= real + fake`), а не число fake: 1 real + 9 fake = 10.

Cookie резервации на srcds пока константа GC (`GameServerCookieId`), поэтому в `assignment` её нет: reservation на самом
srcds — этап C/D (см. RESEARCH_FINDINGS.md §49).

## Админ-панель `/admin`

Форма входа → панель. Cookie `MMSESSION` (HttpOnly, SameSite=Lax), CSRF-токен на всех изменяющих запросах,
`Content-Security-Policy` без inline-скриптов. Выход — кнопка «Log out».

* **Active Searches** — AccountID, режим (с меткой Accept), `game_type` (сырое значение из 9101), `game_mode`,
  карты, время старта («12 s ago»), длительность, статус. Обновляется каждые 2 с. Кнопка **End** снимает
  заявку. «Show ended» показывает завершённые за последний час (Cancelled / Timed out / Removed).
  «Add a test search» добавляет тестовую заявку (метка TEST). Nickname из GC недоступен — показывается
  только AccountID (при наведении — SteamID64).
  В колонке «Server / match» — назначенный сервер или прогресс сбора (`1/2 on de_dust2`).
* **Game Servers** — таблица `host/IP, порт, категория, карта, состояние, enabled, последний отчёт, дата добавления`;
  добавить (в т.ч. с начальным состоянием Available/Busy), изменить, удалить, включить/выключить, **Set busy / Set available /
  Release**. Категории (MVP): Competitive, Wingman, Danger Zone, Casual, Deathmatch, Arms Race, Demolition, Skirmish.
  **ScrimComp5v5 и Cooperative не поддерживаются** (400). Пара `host:порт` уникальна. Правка не меняет состояние.
* **Matches** — последние матчи: сервер, карта, статус (`FORMING` / `READY` / `ENDED` / `CANCELLED`), игроки (реальные + fake), AccountID.
* **Fake Players** — тестовые виртуальные игроки (см. «Fake Players» выше): добавить (режим, число игроков, карты кнопками из карт ваших серверов или
  вручную), Stop search / Start, Edit, Delete, статус и матч/сервер, к которому они присоединились.

## HTTP API (для GC)

Все тела и ответы — JSON, поля в `snake_case`. Ошибки: `{"error":"<код>","message":"<текст>"}`.

| Метод и путь | Доступ | Назначение |
|---|---|---|
| `GET /api/v1/health` | публично | `{"status":"ok",...}` |
| `POST /api/v1/matchmaking/search` | `X-Api-Key` | игрок начал поиск |
| `GET /api/v1/matchmaking/search/{request_id}` | `X-Api-Key` или сессия админа | состояние поиска + `assignment`; **GC опрашивает раз в секунду**, это же продлевает поиск (heartbeat); 404 — backend такого поиска не знает |
| `POST /api/v1/matchmaking/cancel` | `X-Api-Key` | игрок отменил поиск |
| `POST /api/v1/matchmaking/accepted` | `X-Api-Key` | GC игрока: srcds сообщил «все на stage 2»; `ACCEPTED` наступает, когда отчитались **все** реальные игроки матча; GC подключает игрока только после этого |
| `GET /api/v1/matchmaking/account/{account_id}` | `X-Api-Key` | GC участника party находит поиск, который создал лидер (`party_leader_id`, `request_id`, `mode`, `game_type`); 404 — поиска нет |
| `GET /api/v1/servers/roster?address=&port=` | `X-Api-Key` | состав ближайшего `FORMING`/`READY` матча на зарегистрированном сервере (`players[]` с `fake`), 404 если матча нет; канал для srcds (§54) |
| `POST /api/v1/servers/state` | `X-Api-Key` | сервер сообщает о себе: `{"address","port","map"?,"state"?}` (state `AVAILABLE`/`BUSY`); только зарегистрированные серверы, `AVAILABLE` не снимает RESERVED |
| `GET /api/v1/matchmaking/searches[?include_finished=true]` | `X-Api-Key` или сессия админа | список заявок (его же читает панель) |

### `POST /api/v1/matchmaking/search`

```json
{
  "account_id": 1050166997,
  "game_type": 520,
  "mode": "competitive",
  "game_mode": "competitive",
  "maps": ["de_dust2"],
  "request_id": "76561199010432725-17-8123456"
}
```

| Поле | Обяз. | Откуда в GC (уже есть на момент 9101) |
|---|---|---|
| `account_id` | да | `ClientGC::AccountId()` (`ISteamUser::GetSteamID()` & 0xFFFFFFFF), 1…4294967295 |
| `game_type` | да | `request.game_type()` — сырое значение `eGame \| (mapMask << 8)`; категория = `game_type & 0xF` |
| `mode` | нет | `MM::GameMode::name`; если задан — обязан совпасть с `game_type & 0xF` (иначе `400 mode_mismatch`), если нет — выводится из `game_type` |
| `game_mode` | нет | `MM::GameMode::serverGameMode` (`competitive`, `scrimcomp2v2`, …); если нет — берётся из категории |
| `maps` | нет | `MM::DecodeMapSelection(...).maps`; пусто для Skirmish (там выбор — режимы, см. `variants`) |
| `variants` | нет | только Skirmish: `[{name, game_mode, maps[]}]` — выбранные режимы (`MM::DecodeSkirmishSelection`), см. «Skirmish» выше |
| `request_id` | нет | уникальный id этого поиска на стороне GC; нужен, чтобы отличить повтор того же запроса от нового поиска и чтобы запоздалый `cancel` не снял новый поиск |

Ответ `200`:
```json
{ "result": "created",
  "search": { "id": 5, "account_id": 1050166997, "steam_id64": "76561199010432725", "game_type": 520, "e_game": 8,
              "mode": "competitive", "mode_label": "Competitive", "accept_required": true,
              "game_mode": "competitive", "maps": ["de_dust2"], "request_id": "…", "status": "SEARCHING",
              "source": "gc", "started_at": "2026-09-19T10:37:01.341Z", "last_seen_at": "…", "duration_seconds": 0 } }
```
`result`: `created` — новой заявки не было; `refreshed` — тот же поиск (тот же `request_id`, либо без
`request_id` те же режим/карты) повторён, обновлён только таймаут; `replaced` — игрок сменил режим/карты
(или пришёл новый `request_id`), заявка переписана, время старта сброшено. **Дубликатов не бывает:** одна
активная заявка на AccountID (уникальный индекс в БД).

Ошибки `400`: `invalid_request`, `unsupported_game_type` (eGame вне MVP, например 9 = Cooperative,
11 = ScrimComp5v5), `unsupported_mode`, `mode_mismatch`. `401`: нет/неверный `X-Api-Key`.

### `POST /api/v1/matchmaking/cancel`

```json
{ "account_id": 1050166997, "request_id": "76561199010432725-17-8123456" }
```
Ответ `200`: `{"cancelled": true}` либо `{"cancelled": false, "reason": "no_active_search" | "request_id_mismatch"}`.
Отмена несуществующего поиска — не ошибка (идемпотентно). `request_id` необязателен; если он передан и
не совпадает с текущим поиском — отмена игнорируется.

### Timeout

Заявка, которую не обновляли и не отменяли дольше `backend.stale-search-timeout`, получает статус
`EXPIRED` («Timed out»): игрок не «висит» в подборе бесконечно, если GC или игра упали. Таймаут
отсчитывается от последнего `search` (повтор запроса = простейший heartbeat). Отдельного heartbeat-протокола нет.

## Проверка руками (PowerShell)

```powershell
$base = 'http://127.0.0.1:8080'
$h    = @{ 'X-Api-Key' = $env:BACKEND_API_KEY }

Invoke-RestMethod "$base/api/v1/health"

# начало поиска: Competitive, de_dust2 (game_type 520 — реальное значение из логов)
$body = @{ account_id = 1050166997; game_type = 520; mode = 'competitive'; game_mode = 'competitive';
           maps = @('de_dust2'); request_id = 'test-1' } | ConvertTo-Json
Invoke-RestMethod "$base/api/v1/matchmaking/search" -Method Post -Headers $h -ContentType 'application/json' -Body $body

# список (то же видно в панели)
(Invoke-RestMethod "$base/api/v1/matchmaking/searches" -Headers $h).searches

# отмена
Invoke-RestMethod "$base/api/v1/matchmaking/cancel" -Method Post -Headers $h -ContentType 'application/json' `
  -Body (@{ account_id = 1050166997; request_id = 'test-1' } | ConvertTo-Json)
```

`curl.exe` (в PowerShell кавычки в JSON неудобны — лучше файл):
```powershell
Set-Content search.json '{"account_id":1050166997,"game_type":520,"maps":["de_dust2"]}' -Encoding ascii
curl.exe -H "X-Api-Key: $env:BACKEND_API_KEY" -H "Content-Type: application/json" --data-binary "@search.json" http://127.0.0.1:8080/api/v1/matchmaking/search
curl.exe -H "X-Api-Key: $env:BACKEND_API_KEY" http://127.0.0.1:8080/api/v1/matchmaking/searches
```

Реальные `game_type` из живых логов проекта: Casual `519`, Competitive `520`, Deathmatch `518`, Danger Zone `4877`
(карты `dz_blacksite,dz_sirocco,dz_county`). `151977990` — тоже Deathmatch (eGame 6), но с выбором `mg_hostage`.
Backend берёт категорию только из `game_type & 0xF`; карты присылает GC списком `maps`.

## Что делает C++ GC (реализовано, RESEARCH_FINDINGS.md §48–§49)

`csgo_gc/backend_client.*` — свой HTTP-клиент на сокетах, отдельный поток, короткие таймауты (connect 1,5 с, запрос 3 с).
В `config.txt` клиента в блоке `matchmaking` **только** `backend_url` и `backend_api_key`.

1. `OnMatchmakingStart` (9101) → `POST /search` (`account_id`, `game_type`, `mode`, `game_mode`, `maps[]`, `request_id`),
   дальше опрос `GET /search/{request_id}` раз в секунду, пока нет результата. Backend недоступен — повторы каждые 2 с
   (лог с троттлингом), поиск, потерянный backend'ом (404), регистрируется заново.
2. Пришёл `assignment` (`READY_TO_CONNECT` / `WAITING_ACCEPT`) → GC запускает уже работавший поток 9107 / Accept /
   QueueConnect с адресом, портом и картой из `assignment` (раньше — из `config.txt`).
3. `OnMatchmakingStop` (9102) → `POST /cancel`, опрос прекращается.

## Устройство проекта

```
java-backend/
  pom.xml, mvnw.cmd, build.cmd, run.cmd, config/application-example.properties
  src/main/java/dev/csgogc/mm/
    MatchmakingBackendApplication   старт (+ обход AF_UNIX-проблемы JDK на Windows, см. ниже)
    config/   BackendProperties, DatabaseConfig + SchemaInitializer (SQLite, миграция старой БД), SecurityConfig, ApiKeyFilter, LoginThrottle(+Filter), StartupValidator
    mode/     ModeCategory            8 MVP-категорий: eGame ↔ ключ ↔ srcds game_type/mode ↔ accept/игроков
    search/   SearchService (matcher: выбор сервера, резервирование, сбор игроков), Search/MatchRepository, SearchReaper (таймеры), SearchApiController (/api/v1)
    server/   GameServerService/Repository (реестр + состояние AVAILABLE/RESERVED/BUSY, отчёт сервера)
    admin/    AdminPageController (страницы), AdminApiController (/admin/api/**)
    web/      ApiException, ApiExceptionHandler
  src/main/resources/  application.properties, schema.sql, admin/{login,index}.html, static/admin/assets/{admin.js,admin.css}
  src/test/java/…      BackendIntegrationTest (15: авторизация, CSRF, API, дедупликация, cancel, timeout, CRUD),
                       MatchmakingIntegrationTest (21: выбор сервера, режим/карта/disabled/BUSY, эксклюзивность, сбор игроков,
                       TTL, polling, отчёт сервера), SchemaMigrationTest (миграция БД этапа 1) — всего 37
```
Таблицы SQLite: `matchmaking_match` (матч = сервер + заявки), `matchmaking_search` (уникальный частичный индекс «одна живая заявка на AccountID»),
`game_server` (уникальный `host+port`). Схема создаётся `schema.sql` при каждом старте (идемпотентно).

## Безопасность (что учтено и чего нет)

* Учётных данных в исходниках нет; без `backend.admin.*` backend не запускается.
* Админ: сессия + CSRF, блокировка входа по IP после 5 неудач, CSP, `X-Frame-Options: DENY`, `no-store` на страницах.
* GC: `X-Api-Key` (сравнение за постоянное время); ключ не открывает админку.
* По умолчанию слушает только `127.0.0.1`. **Нет TLS**: при `0.0.0.0` пароль и ключ идут открытым текстом —
  используйте только в доверенной сети/за reverse proxy с HTTPS (тогда `cookie.secure=true`).
* Ключ API в `config.txt` GC будет лежать в открытом виде — это ключ доверенной сети, не секрет уровня интернета.

## Устранение проблем

* **`Unable to establish loopback connection` / `Invalid argument: connect`** при старте Tomcat: JDK 21
  на Windows строит NIO-селектор на AF_UNIX-сокете во временной папке; на этой машине это не работает для
  `%TEMP%` пользователя. `main()` сам направляет сокеты в `data\tmp`; если задаёте вручную —
  `-Djdk.net.unixdomain.tmpdir=<любая обычная папка>`.
* **`'mvnw.cmd' is not recognized`** при запуске своих скриптов: в окружении включён
  `NoDefaultCurrentDirectoryInExePath` — вызывайте скрипты по полному пути (`build.cmd`/`run.cmd` это уже делают).
* **`Admin credentials are not configured`** — задайте логин и пароль (см. «Быстрый старт»).
* **Порт занят** — `run.cmd --server.port=9090`.

## Party: несколько реальных игроков в одном матче (RESEARCH_FINDINGS.md §65)

`MatchmakingStart.account_ids` лидера лобби содержит **всех** участников лобби; клиенты остальных участников поиск не шлют.
GC лидера передаёт остальных в `POST /matchmaking/search` как `"party_account_ids": [..]`. Backend:

* создаёт участнику поиск (тот же режим/карты, `party_leader_id` = поиск лидера, `request_id` = `pt<id строки>`) и размещает **всю
  партию в один матч** (нужно место на всех, fake добирают остаток);
* все участники получают assignment, попадают в ростер srcds и должны принять; `ACCEPTED` — только когда приняли все;
* GC участника раз в секунду (пока нет своего поиска) спрашивает `GET /matchmaking/account/{id}`, находит поиск и ведёт его как
  свой: собственный 9107 и Match Found;
* отмена лидера до его Accept снимает всю партию; Stop лидера после Accept (подключение) остальных не трогает.

Fake-заявка — пул: партия из двух реальных игроков в Competitive добирается 8 из пула 9 (после `gather-window`), отдельной заявки «на 8» не нужно (§66).

## Несколько реальных игроков (не в party), RESEARCH_FINDINGS.md §66

Игроки, которые ищут по отдельности, попадают в один матч, пока он собирается (`MATCHED`, без сервера): каждому — свой поиск и своё
assignment, все в ростре srcds, `ACCEPTED` только когда приняли все реальные. Прогресс виден в ответе поиска:
`match.real_players` / `match.accepted_players` (пока `accepted_players < real_players`, никто не подключается). В логе backend на каждый матч:
кто вошёл, кто получил Match Found (`fetched the server data`), кто принял; при таймауте Accept — «accepted 1/3: account 7 accepted,
account 8 got Match Found but did not accept, account 9 never fetched the server data». `backend.required-players.<mode>=1` для игры
несколькими людьми **не использовать**: каждый игрок получил бы свой матч и свой сервер (стартовая проверка пишет WARN).