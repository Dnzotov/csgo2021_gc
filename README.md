# csgo2021_gc — Game Coordinator и matchmaking для CS:GO (сборка 8 октября 2021)

> **Основано на [mikkokko/csgo_gc](https://github.com/mikkokko/csgo_gc)** (см. также [issue #82](https://github.com/mikkokko/csgo_gc/issues/82) в исходном проекте).
> Исходный проект распространяется под **BSD-2-Clause License**. Этот репозиторий — его развитие/модификация: он содержит
> собственные изменения и дополнительные компоненты (matchmaking-логика в GC, отдельный Java backend, тестовая
> инфраструктура, Panorama-правки, документация). Код исходного проекта и сторонние библиотеки написаны не нами — подробности в
> разделе [License](#license).

> [!CAUTION]
> Проект экспериментальный и не готов для общего использования. Всё, что описано ниже, проверялось на Windows (32-битные
> `csgo.exe` / `srcds.exe`) и на сборке клиента, указанной в разделе [Важные предупреждения](#важные-предупреждения).

## Содержание

1. [Что это за проект](#что-это-за-проект)
2. [Структура репозитория](#структура-репозитория)
3. [Требования](#требования)
4. [Клонирование](#клонирование)
5. [Сборка C++ Game Coordinator](#сборка-c-game-coordinator)
6. [Сборка Java backend](#сборка-java-backend)
7. [Конфигурация Java backend](#конфигурация-java-backend)
8. [Запуск Java backend](#запуск-java-backend)
9. [Установка C++ GC](#установка-c-gc)
10. [Panorama: `code.pbin` и `panorama.dll`](#panorama-codepbin-и-panoramadll)
11. [Запуск matchmaking](#запуск-matchmaking)
12. [Тесты](#тесты)
13. [Troubleshooting](#troubleshooting)
14. [Важные предупреждения](#важные-предупреждения)
15. [License](#license)
16. [Attribution / Credits](#attribution--credits)

---

## Что это за проект

Это экспериментальная реализация Game Coordinator (GC) и matchmaking-инфраструктуры для Counter-Strike: Global Offensive,
сборка от 8 октября 2021 года. В Valve-играх GC — это backend-сервис, который отвечает за matchmaking, инвентарь и т. п.
Оригинальный `csgo_gc` подменяет трафик к GC собственной реализацией внутри процесса игры; этот репозиторий добавляет к ней
matchmaking.

В репозитории две основные части. **Это отдельные компоненты, и их нужно настраивать совместно** (адрес и API-ключ backend'а
прописываются в конфиге GC, а серверы и режимы — в панели backend'а).

### C++ Game Coordinator (`csgo_gc.dll`)

Наш модифицированный GC. Загружается лаунчерами `csgo.exe` / `srcds.exe`, общается с клиентом CS:GO и с dedicated-сервером и
реализует matchmaking lifecycle:

- Game Coordinator (наследие upstream: инвентарь, кейсы, стикеры, магазин, лобби и т. д.);
- matchmaking search (клиент сообщает поиск backend'у и получает от него назначенный сервер);
- server reservation (резервация dedicated-сервера + keep-alive резервации);
- Accept / Match Found flow (Competitive, Wingman, Danger Zone);
- server roster (ростер матча, который srcds берёт у backend'а);
- fake-player testing infrastructure (виртуальные участники для проверки Accept без десяти человек);
- взаимодействие с backend'ом по HTTP;
- protobuf-сообщения (`protobufs/`);
- интеграция с Panorama/UI (модифицированный `code.pbin`, см. ниже).

### Java matchmaking backend (`java-backend/`)

Отдельное Spring Boot приложение (SQLite), которое отвечает за:

- matchmaking searches (заявки игроков);
- сбор игроков в матч;
- выбор dedicated-сервера;
- состояние серверов (`AVAILABLE` / `RESERVED` / `BUSY`);
- reservation lifecycle;
- Accept timeout;
- fake players (тестовый инструмент);
- assignments (какой сервер получил игрок);
- HTTP API для GC и srcds;
- web-панель администратора (`/admin`).

Подробная документация backend'а (HTTP API, панель, режимы) — в [java-backend/README.md](java-backend/README.md).
Журнал реверс-инжиниринга и принятых решений — [RESEARCH_FINDINGS.md](RESEARCH_FINDINGS.md) (это исследовательские заметки,
а не инструкция по установке).

### Наследие upstream (без изменений по смыслу)

Редактируемый инвентарь (`inventory.txt`), экипировка, открытие кейсов, граффити, StatTrak, хранилища, стикеры и патчи,
name tag'и, музыкальные наборы, магазин, лобби, dedicated server, браузер серверов, сеть через Steam P2P. Подробнее об этих
возможностях — в [исходном проекте](https://github.com/mikkokko/csgo_gc). Руководство по ручному редактированию инвентаря
сторонним автором: [gist](https://gist.github.com/dricotec/1ae3deb06c42012970c00df914348e76); GUI-редакторы обсуждаются в
[issue #82](https://github.com/mikkokko/csgo_gc/issues/82) исходного проекта.

---

## Структура репозитория

Список соответствует ветке `experimental-native-mm` (ветка по умолчанию).

```text
csgo2021_gc/
├── csgo_gc/              # C++ Game Coordinator (исходники csgo_gc.dll)
├── launcher/             # лаунчеры csgo.exe и srcds.exe (загружают csgo_gc.dll)
├── protobufs/            # .proto клиента CS:GO (C++ код генерируется при сборке)
├── steamworks/           # заголовки Steamworks SDK для сборки
├── java-backend/         # Java matchmaking backend (pom.xml, mvnw, build.cmd, run.cmd, src/)
│   ├── config/           #   application-example.properties (шаблон конфига)
│   └── README.md         #   документация backend'а (HTTP API, панель, режимы)
├── offline_tests/        # offline-тесты C++ и Panorama (roster, reservation, panorama)
├── examples/             # примеры конфигов GC: config.txt, inventory.txt, price_sheet.txt, unusual_loot_lists.txt
├── patched_panorama/     # code.pbin, panorama.dll и panorama.dll.i64 (см. раздел про Panorama)
├── .github/              # CI-workflow и шаблоны issue (унаследованы от upstream)
├── CMakeLists.txt        # корневой CMake
├── vcpkg.json            # зависимости C++ (mbedtls, protobuf)
├── build_local.bat       # локальная Windows-сборка (csgo_gc, csgo, srcds)
├── RESEARCH_FINDINGS.md  # журнал реверс-инжиниринга и решений
├── LICENSE               # BSD-2-Clause (лицензия исходного проекта)
└── README.md
```

Каталоги, которые **создаются при работе** и не хранятся в git:

```text
Build/                    # результат сборки C++ (Build/build_ninja — build dir, Build/release — итоговые файлы)
java-backend/target/      # результат сборки backend'а
java-backend/data/        # SQLite база backend'а
java-backend/config/application.properties   # ваш конфиг backend'а с паролями (git-ignored)
tools/                    # ваш локальный портативный toolchain для build_local.bat (git-ignored)
```

---

## Требования

### C++ GC

- **Windows.** Игра 32-битная, поэтому собирается **x86 (Win32)**.
- **MSVC** с поддержкой C++20 (в `CMakeLists.txt` задано `CMAKE_CXX_STANDARD 20`). Локальная сборка проекта использует MSVC
  14.44 (Visual Studio 2022) из портативной папки `tools\msvc`.
- **CMake 3.20+** (`cmake_minimum_required(VERSION 3.20)`).
- **Ninja** — для `build_local.bat`; при сборке через Visual Studio generator не нужен.
- **vcpkg** — зависимости из `vcpkg.json`: `mbedtls` и `protobuf` (версии зафиксированы `builtin-baseline`). Тройка для
  игры: `x86-windows-static`.
- **Git и доступ в интернет при первой конфигурации**: `funchook` (вместе с `distorm`) подтягивается через CMake
  `FetchContent`, а vcpkg скачивает и собирает `mbedtls` и `protobuf`.
- Никаких сгенерированных файлов в репозитории нет: `*.pb.cc` / `*.pb.h` генерируются при сборке из `protobufs/*.proto` в
  build-каталог (`csgo_gc/generated`).

Дополнительно ничего ставить вручную не нужно, если у вас есть toolchain из списка выше.

### Java backend

- **Java 21** — в `pom.xml` `<java.version>21</java.version>` (Spring Boot 3.5.16). Нужен именно JDK 21.
- **Maven** отдельно ставить не нужно: используется Maven Wrapper (`mvnw` / `mvnw.cmd`), который скачивает
  **Maven 3.9.11** (версия зафиксирована в `java-backend/.mvn/wrapper/maven-wrapper.properties`); нужен интернет при первой
  сборке (зависимости Maven Central).
- `build.cmd` / `run.cmd` берут JDK из переменной `JAVA_HOME`, а если она не задана — из `tools\jdk21` в корне репозитория.

### Игра

- CS:GO той сборки, которая описана в разделе [Важные предупреждения](#важные-предупреждения). Игру репозиторий не
  распространяет.

---

## Клонирование

```bash
git clone https://github.com/Dnzotov/csgo2021_gc.git
cd csgo2021_gc
git branch --show-current
```

Актуальный проект находится в ветке **`experimental-native-mm`** — это ветка по умолчанию, поэтому обычный `clone` сразу
даёт её. Если вы клонировали иначе или находитесь на другой ветке, переключитесь явно:

```bash
git clone -b experimental-native-mm https://github.com/Dnzotov/csgo2021_gc.git
# или, в уже клонированном репозитории:
git checkout experimental-native-mm
```

Ветка `main` — более ранний снимок, в ней нет ни `patched_panorama/`, ни `offline_tests/`, ни последних изменений
matchmaking. Не копируйте файлы вручную из старых копий проекта: всё нужное лежит в этой ветке.

---

## Сборка C++ Game Coordinator

### Вариант 1: `build_local.bat` (то, чем собирается проект)

`build_local.bat` — локальная Windows-сборка «в одну команду». Она использует **портативный toolchain из папки `tools\` в
корне репозитория**. Эта папка добавлена в `.gitignore` (большие бинарники) и **после `git clone` её нет** — вам нужно
создать её самим или подправить скрипт под свои установленные инструменты. Скрипт ожидает такие пути:

| Что | Путь, который использует скрипт |
|---|---|
| MSVC (x64 → x86 cross) | `tools\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat` |
| CMake | `tools\cmake\bin` |
| Ninja | `tools\ninja\ninja.exe` |
| vcpkg | `tools\vcpkg\scripts\buildsystems\vcpkg.cmake` |

Запуск (из `cmd`, в корне репозитория):

```bat
build_local.bat
```

Скрипт последовательно:

1. вызывает `vcvarsamd64_x86.bat` (окружение MSVC для 32-битной сборки);
2. конфигурирует CMake: `cmake -G Ninja -S . -B Build\build_ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVCPKG_TARGET_TRIPLET:STRING=x86-windows-static -DOUTDIR=<репозиторий>\Build\release ...`;
3. собирает цели `csgo_gc`, `csgo` и `srcds`.

### Вариант 2: без `tools\` — CMake напрямую

Если у вас установлены Visual Studio (C++), CMake и vcpkg, можно использовать команды из CI-workflow
(`.github/workflows/build.yml`, Windows). `<vcpkg>` — путь к вашему vcpkg:

```bat
mkdir release
cmake -A Win32 -B build -S . -DCMAKE_BUILD_TYPE=Release -DOUTDIR=%CD%\release -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET:STRING=x86-windows-static
cmake --build build --config Release --target csgo srcds csgo_gc
```

### Результат сборки

Параметр `-DOUTDIR` определяет каталог, куда post-build шаги копируют готовые файлы. Для `build_local.bat` это
`Build\release`:

```text
Build/release/csgo.exe                    # лаунчер клиента
Build/release/srcds.exe                   # лаунчер dedicated-сервера
Build/release/csgo_gc/csgo_gc.dll         # Game Coordinator
```

Build-каталог `Build\build_ninja` — промежуточный. Если вы использовали вариант 2, те же три файла будут в вашем `release\`.

---

## Сборка Java backend

```bat
cd java-backend
build.cmd
```

`build.cmd` = `mvnw.cmd -B package` (с запуском тестов; дополнительные аргументы передаются Maven'у). Без тестов:

```bat
build.cmd -DskipTests
```

Скрипт берёт JDK из `JAVA_HOME`, а если он не задан — из `..\tools\jdk21`; если нет ни того ни другого, он сообщит об ошибке.
Нужна **Java 21**.

Если у вас свой Maven, эквивалент — `mvn -B package` в каталоге `java-backend` (Maven Wrapper фиксирует версию 3.9.11).
На Linux/macOS: `./mvnw -B package`.

Итоговый файл:

```text
java-backend/target/matchmaking-backend.jar
```

---

## Конфигурация Java backend

Шаблон конфига: [`java-backend/config/application-example.properties`](java-backend/config/application-example.properties).
Скопируйте его и впишите свои значения:

```bat
cd java-backend
copy config\application-example.properties config\application.properties
```

`config/application.properties` находится в `.gitignore` — **не коммитьте настоящие пароли и ключи**. Учётных данных по
умолчанию нет: backend не запустится, пока не заданы `backend.admin.username` и `backend.admin.password`. Порядок приоритета:
аргументы командной строки → переменные окружения (например `BACKEND_ADMIN_PASSWORD`, `BACKEND_API_KEY`, `SERVER_PORT`) →
`config/application.properties` → значения по умолчанию из JAR.

### Что нужно изменить под вашу сеть

```properties
# 127.0.0.1 = только этот ПК. Чтобы клиенты и srcds на других ПК достучались до backend'а — 0.0.0.0 или LAN-адрес этого ПК.
server.address=0.0.0.0
server.port=8080

backend.admin.username=admin
backend.admin.password=CHANGE_ME

# общий секрет: тот же ключ прописывается в csgo_gc\config.txt (matchmaking.backend_api_key) клиента и srcds
backend.api-key=CHANGE_ME
```

### Основные параметры

| Параметр | По умолчанию | Смысл |
|---|---|---|
| `server.address` / `server.port` | `127.0.0.1` / `8080` | адрес и порт HTTP |
| `backend.admin.username` / `.password` | — (обязательно) | вход в админ-панель |
| `backend.api-key` | пусто | секрет для GC/srcds (заголовок `X-Api-Key`); пусто = эндпоинты для GC открыты всем |
| `backend.database-path` | `./data/matchmaking.db` | файл SQLite (относительно каталога запуска) |
| `backend.stale-search-timeout` | `PT15M` | поиск, который никто не опрашивал, завершается |
| `backend.matcher-interval` | `PT1S` | как часто matcher ищет сервер / игроков и проверяет дедлайны |
| `backend.server-reservation-ttl` | `PT10M` | сколько зарезервированный сервер остаётся `RESERVED` после выдачи, если матч не завершён иначе |
| `backend.assigned-search-timeout` | `PT2M` | назначенная заявка завершается через это время |
| `backend.accept-timeout` | `PT25S` | **Accept timeout**: время на Accept с момента выдачи сервера; потом матч отменяется |
| `backend.server-release-cooldown` | `PT15S` | сервер из отменённого Accept-матча не выдаётся столько, чтобы srcds успел снять резервацию |
| `backend.required-players.<mode>` | competitive 10, wingman 4, dangerzone 16 | сколько игроков собирает матч Accept-режима. **Не переопределяйте для игры**; значения меньше — только для тестов |
| `backend.roster-ack-timeout` / `backend.roster-poll-window` | `PT25S` / `PT10S` | ожидание подтверждения ростера от srcds (`-gc_mode`) |
| `backend.fake-players.enabled` | `true` | тестовый инструмент Fake Players; `false` — полностью выключить |
| `backend.login.*`, `server.servlet.session.*` | см. шаблон | защита формы входа и cookie сессии |

Длительности задаются в ISO-8601 (`PT25S` = 25 секунд).

### Реестр серверов

Какие dedicated-серверы участвуют в matchmaking, backend знает **только из своей админ-панели** (вкладка Game Servers), а не
из конфигов GC. Для каждого сервера указываются: host/IP, порт, категория (`competitive`, `wingman`, `dangerzone`, `casual`,
`deathmatch`, `armsrace`, `demolition`, `skirmish`), карта и enabled. Адрес и порт должны **совпадать** с `-ip` и `-port`, с
которыми запущен `srcds.exe`; карта — с картой сервера (`+map`) и входить в список карт, которые игрок выбирает в меню.

---

## Запуск Java backend

Из каталога `java-backend`:

```bat
run.cmd
```

`run.cmd` берёт JDK так же, как `build.cmd`, при отсутствии `target\matchmaking-backend.jar` сначала собирает его и запускает
`java -jar target\matchmaking-backend.jar`. Прямой запуск готового JAR (из каталога `java-backend`, чтобы нашлись
`config\application.properties` и `data\`):

```bat
java -jar target\matchmaking-backend.jar
```

Дополнительные параметры передаются аргументами, например: `run.cmd --server.port=9090`.

- **HTTP-порт:** `8080` (`server.port`); по умолчанию слушает только `127.0.0.1` (`server.address`).
- **База данных:** SQLite, файл `java-backend/data/matchmaking.db` (`backend.database-path`); каталог создаётся при старте,
  схема создаётся и мигрируется автоматически (в том числе для баз от предыдущих версий).
- **Проверка health** (публичный эндпоинт, авторизация не нужна):

  ```bat
  curl http://127.0.0.1:8080/api/v1/health
  ```

  Ответ содержит `"status":"ok"`.
- **Админ-панель:** <http://127.0.0.1:8080/admin>. **Авторизация нужна** — форма входа с `backend.admin.username` /
  `backend.admin.password`.
- **API для GC и srcds** (`/api/v1/matchmaking/*`, `/api/v1/servers/*`) требует заголовок `X-Api-Key` (если `backend.api-key`
  не пуст). Полный список эндпоинтов — в [java-backend/README.md](java-backend/README.md).

---

## Установка C++ GC

Оба лаунчера (`csgo.exe` и `srcds.exe`) ищут GC по одному пути относительно **своего** каталога:

```text
<каталог игры>\csgo_gc\csgo_gc.dll
```

(в коде лаунчера это `<каталог exe>\csgo_gc\<GC_LIB_DIR>\csgo_gc.dll`, где для 32-битной Windows-сборки `GC_LIB_DIR` = `.`).
Клиент и dedicated server используют **один и тот же каталог игры и одну и ту же DLL**, если сервер запускается из той же
установки; отдельный путь нужен только если у вас srcds стоит в другой копии игры.

Пример каталога игры (Steam): `C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\` (здесь лежат `csgo.exe`,
`srcds.exe`, `bin\`, `csgo\`).

1. **Сделайте резервные копии** оригинальных `csgo.exe` и `srcds.exe` — они будут заменены.
2. Скопируйте в каталог игры собранные лаунчеры:

   ```text
   Build\release\csgo.exe   ->  <каталог игры>\csgo.exe
   Build\release\srcds.exe  ->  <каталог игры>\srcds.exe
   ```
3. Скопируйте GC:

   ```text
   Build\release\csgo_gc\csgo_gc.dll  ->  <каталог игры>\csgo_gc\csgo_gc.dll
   ```
4. Скопируйте примеры конфигов из [`examples/`](examples/) в `<каталог игры>\csgo_gc\` (`config.txt`, `inventory.txt`,
   `price_sheet.txt`, `unusual_loot_lists.txt`) и отредактируйте `config.txt`:

   ```text
   "matchmaking"
   {
       "backend_url"      "http://192.168.1.150:8080"   // адрес backend'а, только http://
       "backend_api_key"  "CHANGE_ME"                     // то же значение, что backend.api-key
   }
   ```

   Этот `config.txt` читают **и клиент, и srcds** (srcds использует `backend_url`, чтобы получать ростер матча). Остальные
   опции описаны комментариями в [`examples/config.txt`](examples/config.txt) (среди них `appid_override`, `log_output`,
   `reservation_idle_seconds`).

**Клиент и srcds должны использовать одну и ту же (совместимую) сборку `csgo_gc.dll`** — обновляйте DLL на обеих сторонах
одновременно. Старая DLL клиента при новом backend'е не сообщает об успешном Accept и матчи будут отменяться по таймауту.

Если при запуске игры появляется сообщение VAC, запускайте её с аргументом `-steam` (как и в upstream).

---

## Panorama: `code.pbin` и `panorama.dll`

Меню игры (Panorama UI) грузится из упакованного ресурса `csgo\panorama\code.pbin`. Это **не обычный исходный файл**, а
бинарный архив (заголовок `PAN\x02`, блок подписи 512 байт, затем записи с файлами `panorama\layout\*.xml`,
`panorama\scripts\*.js`, стили и т. д.). Проект использует **модифицированный** `code.pbin` — например, из меню убран режим War
Games (правка в `panorama\scripts\mainmenu_play.js`, функция `_IsGameModeAvailable`, подробности в §62
[RESEARCH_FINDINGS.md](RESEARCH_FINDINGS.md)). Модифицированный архив не проходит проверку подписи оригинального
`panorama.dll`, поэтому вместе с ним нужен **пропатченный `panorama.dll`**.

### Файлы в репозитории (`patched_panorama/`)

| Файл | Куда положить | Что это |
|---|---|---|
| `patched_panorama/code.pbin` | `<каталог игры>\csgo\panorama\code.pbin` | модифицированный Panorama-архив |
| `patched_panorama/panorama.dll` | `<каталог игры>\bin\panorama.dll` | пропатченная `panorama.dll` под эту сборку клиента |
| `patched_panorama/panorama.dll.i64` | никуда (не нужен для игры) | база IDA Pro для реверс-инжиниринга, см. ниже |

Перед заменой **сохраните оригиналы** (например, как `code.pbin.bak` и `panorama.dll.bak` рядом) — если после установки
меню пустое или игра не стартует, верните резервные копии. `code.pbin` и `panorama.dll` должны быть от **одной и той же
сборки клиента**, под которую они сделаны.

### `pbin.exe`

`pbin.exe` — сторонняя утилита для распаковки и упаковки `code.pbin`; **в этот репозиторий она не входит**, её нужно получить
отдельно. Команды утилиты: `unpack`, `pack`, `patch_panorama`, `restore_panorama`. Для правки меню используются `unpack` и
`pack`:

```bat
:: 1. в пустой рабочей папке распаковать оригинальный архив (создаётся каталог panorama\ и файл code.pbin.table)
pbin.exe unpack "<каталог игры>\csgo\panorama\code.pbin"

:: 2. отредактировать нужные файлы внутри panorama\ (например, scripts\mainmenu_play.js)

:: 3. упаковать обратно; результат — code.pbin в текущей папке
pbin.exe pack
```

Ограничения `pack`, важные на практике: он требует `code.pbin.table`, созданный `unpack` из **того же** архива; размер файла
внутри архива увеличивать нельзя (файл меньшего размера добивается пробелами); блок подписи в результате обнулён — поэтому
и нужен пропатченный `panorama.dll`. Команды `patch_panorama` / `restore_panorama` рассчитаны на конкретные известные
сборки `panorama.dll`; для этого проекта используйте готовый `patched_panorama/panorama.dll`.

### `panorama.dll.i64`

`patched_panorama/panorama.dll.i64` — **база данных IDA Pro** (примерно 38 МБ), которая используется при реверс-инжиниринге
и анализе бинарника `panorama.dll` (навигация по функциям, комментарии, переименования). Это рабочий артефакт исследования,
**обычному пользователю для запуска игры и проекта он не нужен** — не копируйте его в каталог игры. Открывать его можно только
в IDA Pro. Файл лежит в репозитории для тех, кто продолжает анализ; он большой и содержит данные, полученные из бинарника
Valve. Если вы исследуете другую сборку клиента, создайте базу из **своей** копии `panorama.dll` в IDA, а не используйте эту.

### Workflow правки Panorama

```text
оригинальный panorama.dll (из вашей установки игры)
        │
        ▼
анализ в IDA (panorama.dll.i64) и патч → пропатченный panorama.dll   (одноразово, под сборку клиента)
        │
pbin.exe unpack  →  редактирование Panorama-ресурсов (layout / scripts / styles)
        │
        ▼
pbin.exe pack  →  code.pbin
        │
        ▼
<каталог игры>\bin\panorama.dll  +  <каталог игры>\csgo\panorama\code.pbin
```

Патч `panorama.dll` — это отдельная разовая работа под конкретную сборку клиента; готовый результат уже лежит в
`patched_panorama/panorama.dll`, поэтому пользователю, который просто ставит проект, достаточно скопировать два файла из
таблицы выше. Пошаговых инструкций по самому патчу здесь нет.

> Файлы `panorama.dll` и `code.pbin` — это изменённые файлы Valve, а не код проекта; лицензия BSD-2-Clause на них не
> распространяется.

---

## Запуск matchmaking

Общая схема:

```text
Java backend  ◄──HTTP──►  C++ GC (в csgo.exe)  ◄──►  клиент CS:GO
      ▲                                                     │
      └───────────HTTP──►  C++ GC (в srcds.exe)  ◄──────────┘  (клиент подключается к srcds)
```

### Порядок запуска

1. **Запустите Java backend** (`java-backend\run.cmd`), войдите в панель `/admin` и зарегистрируйте dedicated-серверы
   (Game Servers): адрес, порт, категория, карта.
2. **Запустите srcds** из каталога игры (по одному процессу на режим и порт). Команды запуска ниже. Не меняйте порт и `-ip`
   после регистрации в панели.
3. **Запустите клиент** `csgo.exe` (из каталога игры, с установленным `csgo_gc.dll` и настроенным `config.txt`). Если
   появляется сообщение VAC, запускайте с аргументом `-steam` (см. [Установка C++ GC](#установка-c-gc)).
4. **Нажмите поиск** нужного режима в меню.
5. Backend формирует матч (см. ниже), выдаёт клиенту сервер, клиент подключается.

Примеры команд `srcds` (запускать из каталога игры; IP и порты — пример, подставьте свои и зарегистрируйте те же в панели):

| Режим | Команда |
|---|---|
| Competitive | `srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27017 -gc_mode competitive +game_type 0 +game_mode 1 +map de_dust2` |
| Wingman | `srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27018 -gc_mode wingman +game_type 0 +game_mode 2 +map de_lake` |
| Danger Zone | `srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27019 -gc_mode dangerzone +game_type 6 +game_mode 0 +map dz_blacksite` |
| Casual | `srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27016 +game_type 0 +game_mode 0 +map de_dust2` |
| Deathmatch | `srcds.exe -game csgo -console -usercon -insecure -ip 192.168.1.150 -port 27014 +game_type 1 +game_mode 2 +map de_dust2` |

- `-gc_mode <competitive|wingman|dangerzone>` нужен **только** Accept-режимам: без него сервер держит обычную резервацию и
  Accept для нескольких игроков не синхронизируется. Для Casual / Deathmatch `-gc_mode` **не** указывается.
- Wingman в проекте не проверялся на живых процессах.
- `gc_log.txt` (лог GC при `log_output "2"`) общий для всех процессов из одного каталога игры и удаляется при старте каждого
  процесса: сначала запускайте клиент, потом srcds, и копируйте лог после каждого прогона.

### Режимы без Accept

Casual, Deathmatch, Arms Race, Demolition, Skirmish (War Games скрыт из меню, но серверная поддержка в backend осталась):
backend сразу назначает подходящий сервер, сервер **не резервируется** в backend'е (на нём могут играть несколько человек),
клиент подключается без Accept. Резервацию на стороне srcds поддерживает keep-alive.

### Режимы с Accept: Competitive / Wingman / Danger Zone

```text
SEARCHING
   → GATHERING   (в backend: FORMING — игроки собираются, сервер НЕ резервируется)
   → FULL        (набралось required_players: 10 / 4 / 16)
   → RESERVED    (только теперь атомарно резервируется свободный сервер)
   → ACCEPTING   (игроки получили сервер и жмут Accept; у backend'а есть дедлайн, по умолчанию 25 с)
   → ACCEPTED    (приняли все — backend получил отчёт от GC клиентов)
   → CONNECT     (клиенты подключаются к серверу)
```

Если дедлайн истёк (Match Found timeout) или игрок ушёл до Accept:

```text
ACCEPTING → timeout → CANCELLED → сервер освобождается (не выдаётся ещё ~15 с)
          → fake players снова SEARCHING → игроки снова ищут → следующий поиск создаёт НОВЫЙ матч
```

Состояние `ACCEPTED` — это не «подключён»: после подключения backend матч не отслеживает (он живёт до
`backend.server-reservation-ttl` или до нового поиска).

### Игра с реальными людьми и с fake-игроками

- **Fake-игроки** (панель → Fake Players) — тестовый инструмент: виртуальные участники добирают матч, пока людей меньше
  `required_players`. Они нужны, чтобы проверять Accept в одиночку.
- **Только реальные игроки:** остановите/удалите fake-заявки в панели (или поставьте `backend.fake-players.enabled=false`),
  не переопределяйте `backend.required-players.competitive`; srcds всё равно запускается с `-gc_mode competitive` (ростер из
  реальных игроков берётся у backend'а). Матч соберётся, когда 10 человек нажмут поиск на подходящей карте. Если людей
  меньше, добавьте fake-заявку на недостающее число участников.
- На каждом ПК игрока должна стоять актуальная `csgo_gc.dll`, а `matchmaking.backend_url` — указывать на backend, доступный
  по сети (см. `server.address`).

---

## Тесты

- **Java backend:** `java-backend\build.cmd` запускает все тесты (`mvnw -B package`). Только тесты: из `java-backend`
  `mvnw.cmd -B test`.
- **C++ offline-тесты** (не входят в сборку DLL; используют `tools\msvc` через `vcvarsamd64_x86.bat`, см. `build.bat`
  в каждой папке):
  - `offline_tests\reservation\build.bat` → `keepalive_test.exe`, `client_flow_test.exe` (в `build\`);
  - `offline_tests\roster\build.bat` → `controller_test.exe` и `roster_e2e.exe` (`roster_e2e.exe` работает против запущенного
    backend'а, порядок описан в `offline_tests\roster\README.md`).
- **Panorama:** `node offline_tests\panorama\war_games_test.js <модифицированный code.pbin> <оригинальный code.pbin>`.

---

## Troubleshooting

**`Invalid User Info` при запуске Training / игры с ботами.** Причина — остаточная (stale) reservation cookie на локальном
сервере процесса `csgo.exe`: движок сравнивает cookie сервера с сессией клиента и отклоняет локального игрока. Клиентская
резервация, которая её оставляла, убрана (§61 в [RESEARCH_FINDINGS.md](RESEARCH_FINDINGS.md), §64): Training должен запускаться.
Если ошибка осталась — проверьте, что в клиенте стоит **актуальная** `csgo_gc.dll` и что перед запуском не использовалась
старая сборка.

**`Failed to connect to the match`.** Проверьте по порядку:
- backend запущен, `matchmaking.backend_url` и `backend_api_key` в `csgo_gc\config.txt` верны, backend слушает нужный адрес;
- сервер зарегистрирован в панели (Game Servers): адрес, порт, категория и карта **совпадают** с командой запуска `srcds`;
- резервация: для Casual/Deathmatch сервер сам её продлевает (keep-alive); для Accept-режимов srcds должен быть запущен с
  `-gc_mode`; в его консоли ищите строки `[MM]` / `[MM-ACCEPT]`;
- клиент и srcds используют **одну и ту же** `csgo_gc.dll`;
- адрес и порт srcds доступны с ПК клиента (`-ip`, firewall, порт UDP).

**Меню Panorama пустое / сломано или игра не стартует после установки.** Проверьте, что `bin\panorama.dll` и
`csgo\panorama\code.pbin` соответствуют друг другу и версии клиента; верните резервные копии оригиналов
(`panorama.dll.bak`, `code.pbin.bak`) — поэтому копии нужно делать перед заменой.

**Match Found timeout (popup Accept закрылся сам).** Это штатный lifecycle: по истечении `backend.accept-timeout` матч
`ACCEPTING → CANCELLED`, сервер освобождается, fake-игроки возвращаются в `SEARCHING`, игроки продолжают поиск, следующий
поиск создаёт новый матч. Если после отмены новый матч не собирается, подождите `backend.server-release-cooldown` (15 с по
умолчанию): сервер в это время не выдаётся.

**Backend не стартует.** Проверьте Java 21 (`java -version`) и что заданы `backend.admin.username` /
`backend.admin.password` — без них backend завершается с ошибкой.

---

## Важные предупреждения

- Проект рассчитан на сборку CS:GO **от 8 октября 2021 года**. У проверенной установки `csgo\steam.inf` содержит
  `ClientVersion=1352`, `PatchVersion=1.38.0.5`, `VersionDate Oct 07 2021`. Файлы от других версий клиента могут быть
  несовместимы.
- `client.dll`, `engine.dll`, `panorama.dll` и `code.pbin` должны соответствовать ожидаемой версии клиента.
- `panorama.dll.i64` нужен только для реверс-инжиниринга, а не для запуска.
- Клиент и srcds должны использовать совместимую `csgo_gc.dll`; Java backend и GC должны быть настроены на правильные
  IP/порт (`server.address`, `matchmaking.backend_url`, `-ip` / `-port` серверов).
- Не коммитьте реальные API-ключи и пароли: `java-backend/config/application.properties` и `java-backend/data/` в
  `.gitignore` не случайно. Если секрет попал в репозиторий, считайте его скомпрометированным и замените.
- Часть возможностей (Wingman, игра десяти живых клиентов) на живых процессах не проверялась.

---

## License

Исходный проект [mikkokko/csgo_gc](https://github.com/mikkokko/csgo_gc) распространяется под **BSD-2-Clause License** (2-Clause
BSD). Текст лицензии лежит в файле [`LICENSE`](LICENSE) в корне этого репозитория (Copyright (c) 2024-2026, Mikko Kokko и
Theeto) и оставлен без изменений — он относится к коду, унаследованному от исходного проекта. Полный текст здесь не
дублируется.

Изменения и дополнительные компоненты этого репозитория (matchmaking-логика в GC, Java backend, тесты, Panorama-правки,
документация) относятся к текущему проекту и опубликованы в этом же репозитории. Если вам нужны иные условия для этих
частей, обратитесь к владельцу репозитория.

Сторонние компоненты сохраняют свои лицензии: `funchook` и `distorm` (подтягиваются CMake при сборке), библиотеки из vcpkg
(`mbedtls`, `protobuf`), Steamworks SDK (`steamworks/`), зависимости Java backend (Spring Boot и др.). CI-workflow upstream
собирает их лицензии в каталог `licenses/` релиза. Файлы `panorama.dll` и `code.pbin` — изменённые файлы Valve и BSD-2-Clause
не покрываются.

## Attribution / Credits

Проект основан на работе:

- **mikkokko/csgo_gc** — <https://github.com/mikkokko/csgo_gc>
- Related issue: <https://github.com/mikkokko/csgo_gc/issues/82>
- Лицензия исходного проекта: **BSD-2-Clause License**

Авторы исходного проекта: **Mikko Kokko** — автор; **Theeto** — код, переиспользованный из проекта-предшественника, списки
unusual loot.

Ошибки и вопросы по **этому** репозиторию оставляйте в его собственном issue tracker
(<https://github.com/Dnzotov/csgo2021_gc/issues>), а не в issues исходного проекта: matchmaking и Java backend в нём
отсутствуют.
