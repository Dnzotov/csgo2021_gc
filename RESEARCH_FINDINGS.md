# RESEARCH_FINDINGS.md — CS:GO 2021 GC Matchmaking Protocol Research

> Технический журнал исследования. Создан перед `/compact`, чтобы продолжить работу без повторного RE/IDA-аудита.
> Дата создания: текущая сессия (после серии live-RE аудитов `client.dll`, `client_panorama.dll`, `project446/csgo_gc.dll` и трёх source-деревьев).
> **Правило чтения этого файла**: если факт не помечен `CONFIRMED`, относиться к нему как минимум с долей сомнения. Если RVA/адрес не был получен — написано `unknown`, не выдумывать.

---

## 1. Структура проекта

### Рабочий проект (редактируем, собираем)
- `D:\csgo2021_gc` — главный рабочий проект. Git-репозиторий (`origin` = `https://github.com/Dnzotov/csgo2021_gc.git`), 2 коммита в истории (`Initial commit` — только `.gitignore`; `Clean gc(csgo 2026)` — полный дамп чистого форка `mikkokko/csgo_gc`, БЕЗ matchmaking).
  - `csgo_gc/` — исходники GC DLL (C++). База — form mikkokko/csgo_gc, без matchmaking-логики.
  - `launcher/` — исходники лаунчера (`csgo.exe`/`srcds.exe` подмена).
  - `protobufs/` — наши `.proto` файлы (`cstrike15_gcmessages.proto`, `base_gcmessages.proto`, `gcsdk_gcmessages.proto`, и др.)
  - `steamworks/sdk/` — vendored Steamworks SDK (headers only).
  - `examples/` — `config.txt`, `inventory.txt`, `price_sheet.txt`, `unusual_loot_lists.txt` (деплоятся в игру вручную при тесте).
  - `tools/` — **portable build toolchain** (MSVC, CMake, Ninja, vcpkg), скопирован из `csgo_gc-master/launcher_builder/tools/`. **В `.gitignore`, не коммитится.**
  - `Build/` — build output (`Build/build_ninja/`, `Build/release/`). **В `.gitignore`.**
  - `build_local.bat` — единственный build-скрипт, использует portable toolchain, НЕ трогает глобальный PATH.
  - `.gitignore` — обновлён: `/tools/`, `*.log`, `build_local_log.txt` добавлены (плюс существовавший `/*build*/`).
  - **Изменённые файлы относительно чистого форка (текущее состояние, не закоммичено дальше исходных 2 коммитов)**:
    - `csgo_gc/gc_client.cpp`, функция `BuildMatchmakingHello`: `required_appid_version` изменён с `13857` на `1352` (соответствует `ClientVersion` из реального `steam.inf`); удалён вызов `set_required_appid_version2(13862)` (поле не существует в реальной схеме GlobalStatistics этой сборки — см. раздел 2).
    - `csgo_gc/steam_hook.cpp`: добавлена **временная READ-ONLY диагностика** `LogIceTransportConfigDiagnostic()` — читает (не меняет!) `P2P_Transport_ICE_Enable` через `ISteamNetworkingUtils::GetConfigValue` при создании `ClientGC`/`ServerGC`. Вызывается из обеих веток `SteamGameCoordinatorProxy`'s constructor. **Код остаётся в дереве**, тест не был завершён успешно (см. раздел 11/14).
    - `build_local.bat` — новый файл.

### Игра (только тестовый деплой, не менять автоматически)
- `D:\SteamLibrary\steamapps\common\csgo legacy\` — установленный клиент CS:GO 2021 (build 1352, `VersionDate=Oct 07 2021`, `PatchVersion=1.38.0.5`), установлен через DepotDownloader (`.DepotDownloader` папка присутствует).
  - `csgo\bin\client.dll` (16,045,480 байт), `csgo\bin\client_panorama.dll` (15,582,496 байт), `csgo\bin\server.dll`, `bin\engine.dll` — реальные боевые бинарники, используемые для live RE.
  - `csgo_gc\csgo_gc.dll` — деплоится **вручную**, не автоматически.
  - **Правило**: ничего не копировать в эту директорию без явного разрешения пользователя на каждый конкретный деплой.

### Reference / read-only источники (НЕ рабочий проект)
- `C:\Users\Administrator\Downloads\csgo_gc-master\` — reference/toolbox папка:
  - `Dumper\` — **ProtobufDumper.exe** (.NET 10 console app, SteamDatabase/ProtobufDumper) + protobuf-net.dll + кэшированные копии `client.dll`, `client_panorama.dll`, `server.dll`, `matchmaking.dll` (**побайтово идентичны** текущей установке игры — проверено `cmp`) + уже готовые дампы: `Dumper\client\*.proto`, `Dumper\client_panorama\*.proto`, `Dumper\server\*.proto`, `Dumper\matchmaking_out\*.proto`, `Dumper\out_p446_client\`, `Dumper\out_p446_csgo_gc\`, `Dumper\out_p446_panorama\` (дампы из project446's бинарников).
  - `project446\` — **готовый собранный GC с реализованным matchmaking** (не наш код, только бинарники): `csgo_gc\csgo_gc.dll` (9,331,712 байт, без PDB), `csgo\bin\client.dll` (16,363,368 байт — **патченный**, отличается от чистого), `bin\panorama.dll` (патченный), `csgo\panorama\code.pbin` (модифицированный Panorama UI), `csgo\resource\csgo_gc_{english,russian}.txt` (кастомные строки), `csgo.exe`. **Read-only, не менять, не собирать.**
  - `launcher_builder\tools\` — источник portable toolchain (скопирован в `D:\csgo2021_gc\tools\`).
  - `pbin.exe`, `pbin_source.py` — в корне `csgo_gc-master\`. Для пересборки `code.pbin` **должен запускаться из рабочей директории самой игры** (`D:\SteamLibrary\...\csgo legacy\csgo\panorama\`), не из `csgo_gc-master`.
  - `references\` — сторонние reference-репозитории (nakama, open-match, agones) — НЕ относятся к текущей задаче (matchmaking-backend примеры, не нужны сейчас).
- `D:\ida_deobfuscated_grok\ida_deobfuscated_grok\src` — декомпилированное (IDA/Ghidra) дерево исходников CS:GO **2021-эры** (14,496 файлов, 4.4GB). Read-only reference.
- `D:\cstrike15_src-master\cstrike15_src-master` — исходники CS:GO **Hydra-эры** (~2015, Scaleform UI, 14,243 файла, 1.2GB). Read-only reference.

### Инструменты RE
- **IDA Professional 9.2** — установлена: `C:\Program Files\IDA Professional 9.2`. Headless-доступ через `idalib` (Python-модуль `idapro`), см. раздел 13.
- RE scratchpad (рабочие копии бинарников + IDA-базы + скрипты запросов): `C:\Users\Administrator\AppData\Local\Temp\claude\D--csgo2021-gc\353ac6d4-ff59-4e7c-aaa1-5d26308d91ad\scratchpad\re\` — содержит `client.dll`, `client_panorama.dll`, `p446_csgo_gc.dll` (копии) + их `.i64`/`.id0` базы (уже проанализированы, можно переоткрывать без повторного auto-analysis) + все `query_*.py` скрипты и `*_out.txt` результаты этой сессии.

### Build/test workflow
```bash
D:\csgo2021_gc\build_local.bat
```
Собирает `csgo_gc`, `csgo` (launcher). Портированный toolchain, ничего глобально не меняет. Деплой в игру — вручную, покомандно, с объявлением пользователю что именно копируется.

---

## 2. Protobuf — подтверждённые факты

### Message ID (подтверждено ВО ВСЕХ ТРЁХ источниках: Hydra source, 2021-decompiled source, И бинарным дампом реального `client.dll` через ProtobufDumper) — `CONFIRMED`

```
k_EMsgGCCStrike15_v2_Base                          = 9100
k_EMsgGCCStrike15_v2_MatchmakingStart               = 9101
k_EMsgGCCStrike15_v2_MatchmakingStop                = 9102
k_EMsgGCCStrike15_v2_MatchmakingClient2ServerPing   = 9103
k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate     = 9104
k_EMsgGCCStrike15_v2_MatchmakingGC2ServerReserve    = 9105
k_EMsgGCCStrike15_v2_MatchmakingServerReservationResponse = 9106
k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve    = 9107
k_EMsgGCCStrike15_v2_MatchmakingServerRoundStats    = 9108
k_EMsgGCCStrike15_v2_MatchmakingClient2GCHello      = 9109
k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello      = 9110
k_EMsgGCCStrike15_v2_MatchmakingServerMatchEnd      = 9111
k_EMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon    = 9112
k_EMsgGCCStrike15_v2_MatchmakingServer2GCKick       = 9113
k_EMsgGCCStrike15_v2_MatchmakingGC2ServerConfirm    = 9114
k_EMsgGCCStrike15_v2_MatchmakingGCOperationalStats  = 9115
k_EMsgGCCStrike15_v2_MatchmakingGC2ServerRankUpdate = 9116
k_EMsgGCCStrike15_v2_MatchmakingOperator2GCBlogUpdate = 9117
k_EMsgGCCStrike15_v2_GC2ServerReservationUpdate     = 9142
k_EMsgGCCStrike15_v2_Party_Register                 = 9189
k_EMsgGCCStrike15_v2_Party_Unregister               = 9190
k_EMsgGCCStrike15_v2_Party_Search                   = 9191
k_EMsgGCCStrike15_v2_Party_Invite                   = 9192
```
`MatchmakingServerMatchEndPartial = 9199` — **только** в 2021-decompiled дереве (B), нет в Hydra (A) и в нашем проекте (C). Новое сообщение, добавленное позже Hydra-эры.

Наш `.proto` (C, `D:\csgo2021_gc\protobufs\cstrike15_gcmessages.proto`) **не регистрирует в enum** значения 9105, 9108, 9111, 9113-9116 — хотя тела сообщений всё ещё определены в файле. Если где-то диспетчеризация идёт по enum-константе — эти сообщения физически недостижимы в нашем коде, надо проверить отдельно.

### Поля сообщений — сравнение A(Hydra)/B(2021 decompiled)/C(наш проект)/**BIN**(реальный дамп client.dll) — `CONFIRMED`

**`CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`** — **точное совпадение во ВСЕХ 4 источниках**, единственное сообщение без единого расхождения:
```proto
optional uint64 serverid = 1;
optional uint32 direct_udp_ip = 2;
optional uint32 direct_udp_port = 3;
optional uint64 reservationid = 4;
optional CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve reservation = 5;
optional string map = 6;
optional string server_address = 7;
```

**`CMsgGCCStrike15_v2_MatchmakingStart`**:
```proto
repeated uint32 account_ids = 1;
optional uint32 game_type = 2;
optional string ticket_data = 3;
optional uint32 client_version = 4;
optional TournamentMatchSetup tournament_match = 5;
optional bool prime_only = 6;
optional uint32 tv_control = 7;   // ПОДТВЕРЖДЕНО В BIN — есть и в реальном клиенте!
```
C имеет дополнительное поле `lobby_id = 8` — **не подтверждено** ни в A, ни в B, ни в BIN. Требует проверки, откуда взято (вероятно, из более позднего upstream mikkokko/csgo_gc, не проверялось на соответствие 2021 build).

**`CMsgGCCStrike15_v2_MatchmakingGC2ClientHello`**: поля 1-19 совпадают везде; поле 20 `rankings` (repeated PlayerRankingInfo) есть в B, C, **и в BIN**; отсутствует в A (Hydra).

**`CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate`**: поля 1-17 совпадают везде (matchmaking, waiting_account_id_sessions, error, ongoingmatch_account_id_sessions, global_stats, failping_account_id_sessions, penalty_account_id_sessions, failready_account_id_sessions, vacbanned_account_id_sessions, server_ipaddress_mask, notes[nested Note: type/region_id/region_r/distance], penalty_account_id_sessions_green, insufficientlevel_sessions, vsncheck_account_id_sessions, launcher_mismatch_sessions); поле 18 `insecure_account_id_sessions` есть в C **и подтверждено в BIN**; отсутствует в A.

**`CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve`**: поля 1-15 совпадают везде (account_ids, game_type, match_id, server_version, rankings, encryption_key, encryption_key_pub, party_ids, whitelist, tv_master_steamid, tournament_event, tournament_teams, tournament_casters_account_ids, tv_relay_steamid, pre_match_data); поля 16-18 (rtime32_event_start, tv_control, flags) есть в B и C, отсутствуют в A. Вложенный `CPreMatchInfoData` тоже отличается: B/C имеют дополнительное поле 6 `wins`, которого нет в A.

**`CMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon`** — **подтверждено в BIN**:
```proto
optional uint32 account_id = 1;
optional CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve abandoned_match = 2;
optional uint32 penalty_seconds = 3;
optional uint32 penalty_reason = 4;
```

**`GlobalStatistics`** (вложено в Hello/Update) — **ключевая находка**: поля 1-13 совпадают везде, включая BIN; поля 14-15 (rtime32_cur, rtime32_event_start) есть в B, C и BIN; **поле 16 `required_appid_version2` есть ТОЛЬКО в нашем проекте (C)** — отсутствует в A, в B, и **подтверждённо отсутствует в реальном бинарнике (BIN)**. Это поле уже удалено из отправляемых данных в `gc_client.cpp` (см. раздел 1) — но само определение поля в `.proto` файле осталось, это не критично (лишнее поле в схеме безвредно, раз мы его не заполняем).

**`CMsgClientHello`/`CMsgClientWelcome`** — **НЕ существуют как `.proto`-исходник ни в A, ни в B** (там только закомментированные строки "moved to gcsystemmsgs.proto", которого не существует как `.proto` ни в одном из деревьев — только бинарно). Существуют **только в нашем проекте C** (`protobufs/gcsdk_gcmessages.proto`) — сверить не с чем, кроме дампа BIN (не проверялось отдельно в этой сессии).

### Реальные значения `EMsgGCCStrike15_v2_MatchmakingGame_t` (game_type) — `CONFIRMED` (из Hydra source, лит. текст) + `HIGH CONFIDENCE` (значение 13, см. ниже)
```
4  = ArmsRace
5  = Demolition
6  = Deathmatch
7  = ClassicCasual
8  = ClassicCompetitive   (с октября 2012)
9  = Cooperative
10 = ScrimComp2v2 (Wingman)  (с апреля 2017)
11 = ScrimComp5v5            (с апреля 2017)
12 = Skirmish                 (с апреля 2017)
13 = ??? (Hydra source не называл явно; ЖИВОЙ БИНАРНИК client_panorama.dll использует case 13 → gametype="freeforall", gamemode="survival" — по общеизвестному внутреннему кодовому имени Valve "Survival" = Danger Zone. Это HIGH CONFIDENCE, не подтверждено буквальным именем enum-константы в исходниках, только по строкам в реальном бинарнике + общеизвестное соответствие.)
```

---

## 3. Полный аудит `ida_deobfuscated_grok` (2021-эра decompiled source) — `CONFIRMED`

**Главный вывод**: реальная client-side orchestration-логика matchmaking-очереди **целенаправленно вырезана** — маркер `/** Removed for partner depot **/`, **140 вхождений в 47 файлах**. `.proto`-схемы целые, C++ код почти весь отсутствует.

- `gcsdk/` — **только заголовки**, `.cpp` реализации нет вообще (`CJobMgr::BRouteMsgToJob`, `RegisterJobType` — declared, not defined). Линкуется как `.lib` (не source).
- `UTIL_GenerateMMGameType()` (`cstrike15_matchmaking_utils.cpp:30`) — тело стёрто маркером. Sibling `UTIL_GenerateMMGame` (строки 12-28) — **реальна**, работает через `#include "cstrike15_gcgamemodes.inc"` (макрос `GAMEMODEENUM`).
- Ни одна из 8 ключевых matchmaking-сообщений (`MatchmakingStart/Stop/Client2GCHello/GC2ClientHello/GC2ClientUpdate/GC2ClientReserve/GC2ClientAbandon/ServerReservationResponse` как *исходящих/обрабатываемых на клиенте*) не встречается нигде вне сгенерённых `.pb.h`.
- `g_GC2ClientHello` — `extern`-объявление используется (`cs_app_lifetime_gamestats.cpp:23,291`), но **нигде не присваивается** — обработчик, который должен его заполнять, отсутствует.
- `GC_REG_CLIENT_JOB` инвентаризация — 10 регистраций найдено, **ни одна** не для 8 ключевых matchmaking-сообщений, кроме `GC2ServerReservationUpdate` (`game/server/cstrike15/cs_gameinterface.cpp:73-92`) — но это **серверный** обработчик Twitch-viewer-count, не резервации.
- `ClientRequestJoinFriendData`/`ClientRequestJoinServerData` (`game/client/cstrike15/gameui/uigamedata.cpp:1468-1547`) — **реальные, рабочие** обработчики, парсят вложенный `res`-подполе типа `MatchmakingGC2ClientReserve`, пишут в `g_mapServerCookies` (`uigamedata.cpp:984-998`). Но `Helper_GetServerCookie` (читатель этого кэша) **не имеет вызывающих** — мёртвый код. Это join-friend (rich presence), НЕ competitive-очередь.
- **Легаси KeyValues/`IMatchFramework`/SysSession pipeline** — полностью рабочий, архитектурно ОТДЕЛЬНЫЙ от GC-протокола:
  ```
  IMatchFramework::CreateSession/MatchSession(KeyValues*)
    ↓ (ключи "game/mode", "game/mapgroupname" — CMatchTitleGameSettingsMgr)
  CDsSearcher::ReserveNextServer()                    [matchmaking/ds_searcher.cpp:765-836]
    ↓
  INetSupport::ReserveServer                          [engine/net_support.cpp:222-230]
    ↓
  CBaseClientState::ReserveServer()                    [engine/baseclientstate.cpp:3551-3592]
    → SendReserveServerChallenge → A2S_GETCHALLENGE
    → BuildReserveServerPayload/A2S_RESERVE (UDP OOB, ICE-шифрование)  [:3711-3836]
    ↓
  HandleReservationResponse → CDsSearcher::OnOperationFinished
    ↓
  CMatchSessionOnlineClient::OnRunCommand_QueueConnect/ConnectGameServer  [mm_session_online_client.cpp:589-645]
    → IVEngineClient::StartLoadingScreenForCommand("connect ip:port")
  ```
  Это **не** GC-очередь для ranked — это X360-эры "SysSession"/dedicated-server-search механизм (UDP OOB, без Steam GC протобуфов вообще).
- `sv_mmqueue_reservation` — реальный, полный формат подтверждён (`engine/baseserver.cpp:4159-4287`, `engine/sv_steamauth.cpp:938-950`):
  ```
  "Q<cookie_hex>,<matchid_hex>,<0|1>:[accountid_hex][accountid_hex]...{casterid_hex}..."
  ```
  `'Q'`=queued/locked, `'G'`=joinable-in-progress. Игроки — `[accountid_hex]`, casters — `{accountid_hex}`.
- **Panorama bridge** — реальный V8-мост через `panorama::RegisterJSMethod`/`ExposeGlobalObjectToJavaScript` (макрос `PANORAMA_COMPONENT_API_INSTALL`, `game/client/cstrike15/uicomponents/uicomponent_common.h:574-613`). Зарегистрированы: `CUiComponent_GameInterface→GameInterfaceAPI`, `CUiComponent_GameState→GameStateAPI`, `CUiComponent_UiToolkit→UiToolkitAPI`, `CUiComponent_OptionsMenu→OptionsMenuAPI`. **`CUiComponent_Lobby`/`LobbyAPI` НЕ найден в этом source tree** (позже подтверждён в реальном бинарнике, см. раздел 5 — файл был исключён из этого конкретного decompiled дерева).
  - `GameInterfaceAPI.ConsoleCommand` → `engine->ClientCmd_Unrestricted()` — универсальный пропуск команд.
  - `GameInterfaceAPI.GetSettingString/SetSettingString` → напрямую `ConVar::Get/SetValue` через alias-таблицу.
  - `CUI_Popup_Generic_Command::HandlePopupButtonClicked` — явно комментировано "runs con commands instead of firing Panorama events" — параллельный ConCommand-путь существует НАРЯДУ с event-based.
  - `IMatchmakingStatus`/`CUI_MMStatus_Popup` (`gameui_matchmakingstatus.cpp`) — нативный C++-владеемый статус-попап, `OnCancel()` вызывает `g_pMatchFramework->GetMatchSession()->Command(KeyValues("Cancel","run","host"))` — синхронный вызов.
  - Accept-специфичного попапа НЕ найдено (только Cancel-обработчик).
- Bot: `game/server/cstrike15/bot/cs_bot_manager.cpp:1230-1267` — dead code (`if(0)`) для QMM bot-занимает-слот-человека.
- Data-center/region selection — НЕ найден в `steam_datacenterjobs.cpp`/`datacenter.cpp` (только generic GC KV stat sync).

---

## 4. Полный аудит Hydra-era source (`cstrike15_src-master`) — `CONFIRMED`

Тот же паттерн вырезания (`/** Removed for partner depot **/`, **44 вхождения**), но другая эпоха/детали:

- `gcsdk/` — тоже только заголовки (только `steamextra/tier1/*.cpp` helper-утилиты — murmurhash3, tsmempool, tsmultimempool, utlstringbuilder).
- `UTIL_GenerateMMGameType` — **не существует в этом дереве вообще**, ни рабочей, ни стёртой версии. Есть `GameTypes::GetGameModeAndTypeIntsFromStrings()` (`gametypes.cpp:2471-2500`) — реальная, но конвертирует в ЛОКАЛЬНЫЕ ConVar-индексы (`game_type`/`game_mode`), не в GC-enum'ы.
- Две GC-enum хелпер-функции в `cstrike15_gcconstants.h` **стёрты**: `MatchmakingGameTypeMapGroupExtendToLargeGroup` (частично, тело макро-инстанцирования удалено, строки 624-641) и `MatchmakingGameTypeMapToString` (полностью, всегда возвращает NULL, строки 660-665).
- `MatchmakingGameTypeCompose` (реальна, строки 545-548): `(eGame&0xF)<<0 | (eMapGroup&0xFFFFFF)<<8` — но **нигде не вызывается** в дереве.
- Единственный след `g_GC2ClientHello` — закомментированная строка `game/client/cstrike15/Scaleform/createmainmenuscreen_scaleform.cpp:288`.
- `CCreateMainMenuScreenScaleform::OnEvent()` (реализует `IMatchEventsSink`) — стёрт, строка 306.
- Нет никакого Accept→GC кода нигде.
- **Steam Lobby — РЕАЛЬНЫЙ, рабочий**: `matchmaking/sys_session.cpp` (`CSysSessionHost`/`CSysSession`) — `SteamMatchmaking()->CreateLobby` (`sys_session.cpp:1861-1874`), `SetLobbyData` (986, 1042), `JoinLobby` (2772, 3763). ВАЖНО: `matchmaking/steam_lobbyapi.cpp`, несмотря на название, содержит ТОЛЬКО `CSteamLeaderboardWriter` (заливка результатов на leaderboard) — не lobby API вообще, вводящее в заблуждение имя файла.
- `sv_mmqueue_reservation` — идентичный формат, `engine/baseserver.cpp:238` (объявление), `CBaseServer::ReserveServerForQueuedGame` (`baseserver.cpp:4053-4109`), `SetReservationCookie` (4111-4190, парсинг `[%x]`/`{%x}`), потребители в `sv_steamauth.cpp:910-916`, `hltvbroadcast.cpp:293-314`, `cs_player.cpp:8879-8880/9633-9653`, `cs_player_resource.cpp:542-543`, `cs_bot_manager.h:148-149`, `cs_gamerules.cpp:4028-4032/16881-16882`.
- Connect-команда: `CMatchSessionOnlineClient::OnRunCommand_QueueConnect` (`mm_session_online_client.cpp:589-612`) читает `"adronline"` KV-поле, `StartLoadingScreenForCommand("connect %s")`; `ConnectGameServer` (614-645) через `ClientCmd_Unrestricted`; идентичный паттерн в `mm_session_online_host.cpp:619`, `mm_session_offline_custom.cpp:176`; `servermanager.cpp:93` — `ClientCmd("connect %s\n")`.
- Полные тела protobuf-сообщений (Hydra) — см. раздел 2 выше, обозначены как "A".

---

## 5. Live RE `client_panorama.dll` (реальный бинарник, IDA 9.2 + Hex-Rays)

Auto-analysis: 297.0с, база сохранена в scratchpad (`client_panorama.dll.i64`).

### `CONFIRMED` (полная декомпиляция + xref)

**`LobbyAPI` / `CUiComponent_Lobby`** — **не существует ни в одном из двух source tree**, но реально скомпилирован в бинарник:
- Строка `"LobbyAPI"` @ `0x10bbdbf0`.
- `sub_1057F2D0` (RVA `0x1057F2D0`) — регистрационная функция, декомпилирована полностью: регистрирует глобальный JS-объект `"LobbyAPI"`, класс `"CUiComponent_Lobby"`, таблица методов `sub_1057F1E0` (RVA `0x1057F1E0`, 21 запись, `sub_10582580`...`sub_10583D50`, каждая ~0x130 байт).

**`LobbyAPI.SetLocalPlayerReady(reason: string)`** — цепочка полностью прослежена:
1. Строка `"SetLocalPlayerReady"` @ `0x10bbd8d4`, doc-строка "Tell matchmaking this player is ready to play a queued match (which leave peanalties)." @ `0x10bbd940` (опечатка "peanalties" реальна, не артефакт).
2. `sub_10583500` (RVA `0x10583500`) — регистрация V8 `FunctionTemplate`, параметр называется `"reason"`.
3. **Реальный нативный callback: `sub_10584F70`** (RVA `0x10584F70`) — JS-аргумент-марштализация (проверка кол-ва аргументов), вызывает `sub_10512130` (RVA `0x10512130`, проверка что объект не удалён), извлекает строку через `sub_1047AF10`, вызывает `sub_10585B00(&value)`.
4. `sub_10585B00` (RVA `0x10585B00`) — классифицирует reason через `sub_10580C20`, пишет в `this+12` один из двух вариантов состояния (не шлёт сеть синхронно!).
5. `sub_10580C20` (RVA `0x10580C20`) — reason-классификатор: `if (reason == "deferred") { check global flag dword_15278690; ...} else { sub_103DEA80(0,2); return true; }`. **`reason` — строка, не int.** `"deferred"` — специальное значение.

**`sub_103DF430`** (RVA `0x103DF430`) — **ЦЕНТРАЛЬНАЯ функция**, независимо переоткрыта в этой сессии (через строку `"Server reservation check %p ready-up!"` @ `0x10b6c5e4`) и **совпадает по адресу** с ранее задокументированной (в предыдущем, ныне удалённом проекте) функцией `FUN_103df430` — высокая перекрёстная достоверность. Полная декомпиляция (1966 байт). Это **KeyValues change-listener для поддерева `"game/mmqueue"`** (`sub_108F9A90("game/mmqueue",...)`).

Структура полей (по офсетам `this+N`, имена условные):
- `this+140` — STAGE: `==1` → ready-up/показ Accept-попапа; иначе → queue-connect ветка.
- `this+136`, `this+132` — доп. флаги/стадии.
- `this+92` — числовой game_type (0, 4-13).
- `this+68`/`this+88` — указатель на имя карты (SSO-стиль: если `this+88>=0x10`, `*this+68` — указатель, иначе inline буфер).
- `this+40`/`this+44` — 64-битный `reservationid`.

Логика:
```
if (this+140 == 1) {
    DevMsg("Server reservation check %p ready-up!")
    → sub_10418AC0("popup_accept_match_found", 0)   // ПОКАЗЫВАЕТ ACCEPT-ПОПАП
    → (условно) dispatch событий через sub_103D8AF0()
} else if (this+132 истинно) {
    DevMsg("Server reservation check %p queue connect")
    → sub_103D7640(this+92)   // ACCEPT-REQUIRED CHECK — см. ниже
        true  → sub_10418AC0("popup_accept_match_confirmed", 0)
        false → sub_10418AC0("popup_accept_match_found", 0) + строит "@"+map через sub_101AFA50(...,"@%s",...)
    // БЕЗУСЛОВНО далее:
    → KeyValuesSystem() → "QueueConnect" команда:
        "adronline"       = адрес подключения (sub_103D7590(this+96))
        "reservationid"   = this+40/this+44 (64-бит)
        "helper_pSession", "helper_time" (+2000мс если matched confirmed-путь)
        "auto_close_session"
        "map"             = this+68/88 (та же строка)
        "gametype"/"gamemode" — switch по this+92:
            4  → "gungame"/"gungameprogressive"       (ArmsRace)
            5  → "gungame"/"gungametrbomb"             (Demolition)
            6  → "gungame"/"deathmatch"                (Deathmatch)
            7  → "classic"/"casual"                    (Casual)
            8  → "classic"/"competitive"                (Competitive)
            9  → "cooperative"/"cooperative"             (Cooperative)
            10 → "classic"/"scrimcomp2v2"                (Wingman)
            11 → "classic"/"scrimcomp5v5"                 (ScrimComp5v5)
            12 → "skirmish"/"skirmish"                    (Skirmish)
            13 → "freeforall"/"survival"                  (Danger Zone, HIGH CONFIDENCE по названию)
            default → "unknown"/"unknown"
    → sub_103DEA80(queueConnectHandle, confirmedFlag)   // исполнение команды
}
```
Единственный xref к самой `sub_103DF430` — DATA-ссылка (не code-call) из `0x10b6a408`, т.е. функция вызывается через generic KV-listener callback table, не напрямую по имени в видимом коде.

**`sub_103D7640`** (RVA `0x103D7640`) — Accept-required-by-gametype проверка **на стороне Panorama**:
```c
bool sub_103D7640(char gameType) {
    if (gameType != 9 && gameType != 13 && gameType != 11) {
        v1 = gameType - 8;
        if (v1 != 0 && v1 != 2) return false;   // gameType не в {8,10}
    }
    return true;
}
```
Эффект: **true** (accept required) для gameType ∈ `{8,9,10,11,13}`; **false** для `{4,5,6,7,12}`.
⚠️ **РАСХОЖДЕНИЕ с project446 GC-стороной** (`{8,10,13}`) — см. раздел 9, НЕ объяснено.

**Event dispatchers** (структурно идентичные, "тонкие" шаблонные функции):
- `sub_10581640` (RVA `0x10581640`) — диспетчер `"PanoramaComponent_Lobby_ReadyUpForMatch"`, `v5=3` (3-аргументный дескриптор, совпадает с известным `$.DispatchEvent(name, bool, int, int)`). Хелперы `sub_1057F0A0`/`sub_1057F0C0`.
- `sub_103E1390` (RVA `0x103E1390`) — диспетчер `"ServerReserved"`, `v5=1` (1-аргументный). Хелперы `sub_103D7730`/`sub_103B3F60`.
- Оба вызываются ТОЛЬКО через data-таблицу (function pointer table): `sub_10581640` из data @ `0x10a98d30`, `sub_103E1390` из data @ `0x10a98234`. Тривиальные trampolines `sub_100502C0`→`sub_10581640` и `sub_100454A0`→`sub_103E1390` тоже не имеют code-xrefs, только data.
- **Кто именно решает вызвать эти события — НЕ найдено** (см. `TODO`, раздел 12).

### `HIGH CONFIDENCE` (найдено, не полностью декомпилировано)

- `CancelConnectToServer` = `sub_103E04E0` (RVA `0x103E04E0`) — найдена через xref строки `"CancelConnectToServer"` @ `0x10b6a5e8`. **Тело не декомпилировано.**
- `QueueConnectToServer` = `sub_103E05A0` (RVA `0x103E05A0`) — xref строки `"QueueConnectToServer"` @ `0x10b6a600`. **Тело не декомпилировано.**
- `MatchAssistedAccept` = `sub_103E0660` (RVA `0x103E0660`) — xref строки `"MatchAssistedAccept"` @ `0x10b6a6e4`. **Тело не декомпилировано.** (Название намекает на отдельный "ассистированный" accept-механизм, не исследовано.)
- Строка `"net_client_steamdatagram_enable_override"` с описанием `"0: Use connect method requested by GC. >0: Always use SDR if possible. <0: Always use direct UDP if possible"` @ `0x10b6a754/0x10b6a780` — подтверждает, что **метод подключения (SDR vs прямой UDP) по умолчанию диктуется GC**.
- 22 функции ссылаются на строку `"game/mmqueue"` в этом бинарнике; **декомпилирована только `sub_103DF430`**. Остальные 17 (за вычетом 3 self-xrefs внутри `sub_103DF430` самой) — кандидаты на роль "кто ЗАПИСЫВАЕТ в `game/mmqueue`" (т.е. обработчик входящего 9107): `sub_103D9190`, `sub_103D9900`, `sub_103DC4B0`, `sub_103DDCB0`, `sub_103DE7C0`, `sub_103DE8E0`, `sub_103DE960`, `sub_103DE9B0`, `sub_103DEF30`, `sub_10484820`, `sub_10484A00`, `sub_104B0360`, `sub_10513AE0`, `sub_1057F760`, `sub_1057F9F0`, `sub_10580890`. **Ни одна не декомпилирована** — приоритетная задача следующей сессии.

### `TODO / NOT YET REVERSED`
- Scan client.dll на immediate-значения message ID (9101/9104/9107/9109/9110/9112) — **упал без вывода** (вероятно, IDA-процесс аварийно завершился на прямом Python-переборе миллионов инструкций). НЕ повторялся другим методом в этой сессии.
- `sub_100774E0`/`sub_1009B750`/`sub_10088500` эквиваленты в `client.dll` (не project446) — не искались вообще, только в project446.
- Кто вызывает `sub_100502C0`/`sub_100454A0` (и через какую именно таблицу) — не найдено, только что они в data-таблице.

---

## 6. `project446/csgo_gc.dll` — детальный аудит

Путь: `C:\Users\Administrator\Downloads\csgo_gc-master\project446\csgo_gc\csgo_gc.dll`, 9,331,712 байт, PE32 i386, **без PDB**. Auto-analysis: 488.9с, база `p446_csgo_gc.dll.i64` в scratchpad.

### Архитектура backend — `CONFIRMED` (по строкам)
- **WebSocket**, не HTTP+JSON (использует `boost::beast`, подтверждено мангл-именами C++ классов). Конфиг: `csgo_gc.env`/`server.env` (`GC_FLEET_SERVER_ID`, `SERVER_TOKEN`, `DATABASE_URL`).
- REST-surface backend'а: `POST /api/admin/servers` (регистрация), `PUT /api/admin/servers/{id}/unban`, `/api/content/{file,manifest,signature}`, `/api/fleet/sdr-cert`, `/api/replays/upload/`, `/api/server/fleet/`, `/api/update/{download,manifest}`. `application/json` подтверждён.
- Реальные датацентры: `SFUI_OfficialDatacenterID_100-105` = Москва/СПб/Екатеринбург/Новосибирск/Алматы/Гравелин (Франция) — подтверждает продакшн-эксплуатацию на реальных регионах.
- Строка `"Notified C# backend"` намекает на .NET/C# backend implementation.
- Fork identity (из `csgo_gc_english.txt`): создатель форка — "morgan"; доп. авторы "shashlik" (StatTrak storage), "gt610" (MVP Music Kit).

### GC_REG-подобные функции и accept flow — `CONFIRMED` (полная декомпиляция)

| Функция | RVA | Назначение |
|---|---|---|
| `sub_10077C90` | `0x10077C90` | Обработчик `MatchmakingStart` (9101) — логирует mode/prime_pool/ranked_pool/ui/prime/flags |
| `sub_100BADE0` | `0x100BADE0` | **Устанавливает vtable-хук `ReportGCQueuedMatchStart`** на `IServerGameDLL` |
| `sub_100787F0` | `0x100787F0` | Accept-инжектор — синтезирует `MatchmakingStop(9102, abandon=0)` |
| `sub_100789A0` | `0x100789A0` | Главный обработчик 9102 — вся accept/state-machine логика |
| `sub_100694A0` | `0x100694A0` | `NeedsAccept(gameType)` — однострочная, точная |
| `sub_1009B750` | `0x1009B750` | Обрабатывает и 9105 (получение), и 9106 (отправку) — GC↔сервер relay |
| `sub_10088500` | `0x10088500` | Вызывает реальный `IVEngineServer::ReserveServerForQueuedGame` |
| `sub_100774E0` | `0x100774E0` | "Proceed"-функция (аналог нашего ConnectAfterAccept/QueueConnect); также содержит dedup-лог для 9107 (`"Skip duplicate 9107"` @ xref `0x100775ef`, внутри этой же функции) |

#### `sub_100BADE0` — установка хука `ReportGCQueuedMatchStart` (`CONFIRMED`, полный код виден)
```c
ModuleHandleA = GetModuleHandleA("server.dll");  // или LoadLibraryA если не загружен
CreateInterface(...)("ServerGameDLL005", &v12);  // fallback "ServerGameDLL001"
dword_1074B2E8 = *v2;  // vtable IServerGameDLL
// Хук #1: GameFrame, VTable[4] (offset+16) — безусловно
*v5 = sub_100BAD70;
// Хук #2: ReportGCQueuedMatchStart, VTable slot по умолчанию = 47 (offset = 4*47 = 188)
v6 = 47;
v7 = getenv("GC_VTBL_REPORT_MATCH_START");   // slot ПЕРЕОПРЕДЕЛЯЕМ через env!
if (v7 valid) v6 = atoi(v7);
if (v6 < 0) {
    log("hook disabled by env — ready-up accepts stay a guess from MatchmakingStop");
    return 1;  // FALLBACK: без хука, accept определяется угадыванием (как у нас!)
}
v8 = vtable + 4*v6;
dword_1074B3F4 = *v8;  // сохраняем оригинал
*v8 = sub_100BAD90;    // ставим хук
log("hooked at VTable[%d], original = %p (ready-up accepts now come from the engine)");
// Хук #3: "IsValveDS" на VTable+164 (slot 41) → IsGCSendAvailable
```
**Вывод**: `ReportGCQueuedMatchStart` — реальная, вызываемая virtual-функция на `IServerGameDLL` в скомпилированном `server.dll`, несмотря на то что её ТЕЛО (в обоих source tree, раздел 3/4) вырезано `/** Removed for partner depot **/`. Слот **47 по умолчанию**, но **явно НЕ гарантированно стабилен** между сборками движка — сделан configurable через `GC_VTBL_REPORT_MATCH_START` env var. Fallback-путь (без хука) явно называется "guess from MatchmakingStop" — то есть project446 **тоже** изначально имел ровно нашу проблему и позже её решил через vtable hook.

#### `sub_100787F0` — Accept-инжектор (`CONFIRMED`, полный код виден)
```c
if (state ∈ {2,3} или flag) {
    if (gameType == 8 || gameType == 10 || gameType == 13) {   // ACCEPT-REQUIRED MATRIX
        log("[GC] Accept hook: injecting MatchmakingStop(abandon=0) — legacy client omits 9102 on Accept");
        // строит CMsgGCCStrike15_v2_MatchmakingStop с vftable
        sub_10115780(9102, builtMessage);   // ФАКТИЧЕСКАЯ передача msgID=9102
    } else {
        state = 3;  // "ready/connect" немедленно
        log("[GC] Accept hook: live join ");
        sub_100774E0(0,1,1,1);   // proceed
    }
}
```

#### `sub_100694A0` — точная Accept-матрица (`CONFIRMED`, максимальная достоверность — однострочная функция, дважды независимо подтверждена)
```c
bool sub_100694A0(char gameType) {
    return gameType != 8 && gameType != 10 && gameType != 13;   // true = "не нужен accept"
}
```
**Итог**: `NeedsAccept(gameType) == (gameType == 8 || gameType == 10 || gameType == 13)`.
`8=Competitive, 10=Wingman, 13=DangerZone` — **точное совпадение с уже используемой у нас матрицей!**

#### `sub_100789A0` — главный обработчик 9102 (`CONFIRMED`, полный код виден, 1426 байт)
State machine по полю `this+1568`:
- **state==3**: "готов/подключение" — обработка дублей через `this+1752` флаг.
- **state==2** (или `this+1640` флаг) — ожидание accept:
  - abandon=true → шлёт abandon наружу через `sub_100BFC20(9102,...)`, "sent abandon=true to GC server", очистка.
  - abandon=false, не обработано ранее: снова проверяет `sub_100694A0(gameType)`:
    - true → `state=3`, `"Matchmaking: live join Accept "`, `sub_100774E0(0,1,1,1)`.
    - false → **`"Matchmaking: Player ACCEPTED, waiting for all players"`**, `sub_100BFC20(9102,&v23)` (форвардит сигнал наружу — на backend), **остаётся в состоянии ожидания**. **Подсчёт "все ли приняли" — НЕ в этом DLL, на backend.**
  - уже обработано (`this+1736`==1): `"MatchmakingStop: duplicate accept ignored"` — **идентичный dedup-паттерн нашему.**
- **state==1**: сравнение высокоточных timestamp'ов (`sub_1052E6B0`, микросекундный масштаб) против сохранённого — если новое событие приходит слишком быстро после auto-requeue, **игнорируется как "stale"** (`"ignored stale close after automatic requeue"`). Это временной guard **отдельно от** булева dedup-флага — решает именно тот класс проблем, что наш "~40мс мистический MatchmakingStop".
- default: доп. проверки (`this+2295`, `this+1976/1928`) → `"ignored (state=%d, abandon=%d)"` или `"cleared phantom backend search (state desync)"`.

### 9105/9106/9107 flow — `CONFIRMED` (по debug-строкам + частичные xrefs)
```
[Client] MatchmakingStart (9101)
    ↓ sub_10077C90 — читает mode/prime_pool/ranked_pool/ui/prime/flags
(backend WS решает подбор — вне DLL, НЕ реверсено)
    ↓
MatchmakingGC2ServerReserve (9105) → на dedicated server
    log: "Forwarded live join 9105 to engine: match_id=%llu game_type=0x%X accounts=%d"
    log: "Stored 9105: match_id=%llu accounts=%d game_type=%u flags=%u"
    log: "Dropping stale 9105: match_id=%llu < current=%llu"
    → sub_1009B750 (та же функция обрабатывает и 9105, и отправку 9106)
    ↓
sub_10088500 → IVEngineServer::ReserveServerForQueuedGame("%.64s")   [РЕАЛЬНЫЙ ENGINE API, не строковый ConVar-poke]
    ↓
MatchmakingServerReservationResponse (9106) → на backend
    log: "Sending 9106: reservationId=%llu map='%s' wsClient=%p"
    log: "9106 Payload size: %zu"
    log: "ERROR: m_wsClient is NULL, cannot send 9106!"
    ↓
MatchmakingGC2ClientReserve (9107)
    log: "Skip duplicate 9107 resId=%llu"   [dedup, в sub_100774E0]
```

### Reservation cookie / Q-G / SDR — `CONFIRMED` (по строкам)
- `"[GC] Reservation generation %u for new match_id=%llu"`
- `"[GC] Reservation cookie changed %llx -> %llx (backend re-registered IP)"`
- `"[GC] ResolveReservationCookie enter: reserve=%p has_enc=%d enc_key=0x%llx latched=0x%llx"` — резолвинг куки завязан на `encryption_key` (совпадает с полем `encryption_key`/`encryption_key_pub` в `MatchmakingGC2ServerReserve`).
- `"[GC] Derived SDR reservation cookie from identity: 0x%llx (steamid=%llu)"` — SDR-кука выводится из SteamID напрямую.
- Q/G — та же система, что в source tree: `"Q-reservation kept"`, `"Swapped Q-reservation for G on live join connection (generation %u)"`, `"Re-armed G-reservation on live join connection"`.
- Whitelist — три отдельных типа: `AdminWhitelist`, `LiveJoinWhitelist`, `SpectatorWhitelist`, каждый с логированием размера/truncation.

### Penalty/abandon — `CONFIRMED` (по строкам)
- `"[GC] Connecting session explicitly abandoned -> penalty requested"`
- `"[GC] Disconnect penalty skipped: account=%u ..."` — множественные условия пропуска (whitelist, non-ranked mode, report_sent/intermission/match_active/warmup флаги).
- `"[GC] AFK observed... no penalty (set GC_AFK_PENALTY=1 to enforce)"` — AFK-penalty opt-in через env.
- `"[GC] Whitelist grace expired for account %u (no reservation, kicking)"`.

### `[ReadyUp] native connect` — важная параллель с нашим RE (`CONFIRMED`, по строкам)
```
"[ReadyUp] native connect did not arm (kind=%u stage=%u queueState=%u), falling back to the game_type=0 reserve"
"[ReadyUp] native connect refused (queueState=%u beforeKind=%d), falling back to the game_type=0 reserve"
"[ReadyUp] native connect signature not found in client.dll, falling back to the game_type=0 reserve"
```
**Подтверждает**: project446 тоже делает **байт-сигнатурный поиск** "native connect" функции в `client.dll` (тот же класс RE, что делаем мы) и имеет graceful fallback, когда сигнатура не совпадает (например, после обновления игры) — подтверждает, что этот путь объективно хрупкий даже для зрелого проекта.

### `TODO / NOT YET REVERSED` (project446)
- `sub_1009B750` — тело не декомпилировано полностью.
- `sub_10088500` — тело не декомпилировано полностью.
- `sub_100774E0` — только первые ~115 строк декомпиляции получены (функция большая), не удалось дочитать в бюджет сессии.
- Мангл-имя намекает на `std::function`-based callback для 9104 внутри лямбды в конструкторе `ClientGC` — **конкретная функция НЕ локализована**.
- Как именно backend агрегирует "все приняли" — не реверсено (это на backend, вне DLL).
- Player selection / matchmaking pool algorithm — не найден в DLL (видимо, целиком на backend).

---

## 7. Разделение CONFIRMED / HIGH / MEDIUM / TODO — сводная таблица

### CONFIRMED (подтверждено ≥2 независимыми источниками ИЛИ прямой декомпиляцией с полным телом функции)
- Все message ID 9100-9117, 9142, 9189-9192 (3 источника: Hydra, 2021-decompiled, бинарный дамп).
- `MatchmakingGC2ClientReserve` — точная схема, 4 источника, 0 расхождений.
- `GlobalStatistics` останавливается на поле 15 в реальном бинарнике (поле 16 `required_appid_version2` — только в нашем проекте, нигде больше).
- `sv_mmqueue_reservation` формат `"Q<cookie>,<matchid>,<flag>:[acct]...{caster}..."` — идентичен в Hydra source И в project446's использовании.
- `ReportGCQueuedMatchStart` — реальная vtable-функция на `IServerGameDLL`, вызываемая в скомпилированном `server.dll` (подтверждено фактом успешного хука в project446), несмотря на вырезанное тело в ОБОИХ source tree.
- Accept-матрица GC-стороны project446: `{8,10,13}` = Competitive/Wingman/DangerZone (дважды декомпилировано, идентичный результат).
- `LobbyAPI`/`CUiComponent_Lobby`/`SetLocalPlayerReady(reason: string)` — реальны в `client_panorama.dll`, полная цепочка декомпилирована до `this+12` state-write.
- `sub_103DF430` — реальный `"game/mmqueue"` KV-listener, вызывающий `popup_accept_match_found`/`_confirmed` и строящий `"QueueConnect"` KV-команду с полями `adronline`/`reservationid`/`map`/`gametype`/`gamemode`.
- "Все приняли" считается НЕ на клиенте (ни в `client_panorama.dll`, ни в project446 GC DLL) — клиентская сторона только форвардит accept-сигнал наружу.
- Дедуп по reservation/accept — независимо реализован и у нас, и в project446 (`this+1736`-подобный флаг).

### HIGH CONFIDENCE (найдено, частично декомпилировано ИЛИ подтверждено 1 источником с ясным контекстом)
- `game_type=13` = Danger Zone (по строке "survival"+общеизвестное кодовое имя, не по буквальному имени enum-константы).
- `CancelConnectToServer`/`QueueConnectToServer`/`MatchAssistedAccept` в `client_panorama.dll` — локализованы по RVA, не декомпилированы.
- `sub_100774E0`/`sub_1009B750`/`sub_10088500` в project446 — роль понятна по контексту, тело не полностью изучено.
- Метод подключения (SDR vs direct UDP) по умолчанию диктуется GC (по строке-описанию convar).

### MEDIUM CONFIDENCE (одна строка/одна зацепка, требует подтверждения)
- Расхождение Accept-матрицы Panorama `{8,9,10,11,13}` vs project446 GC `{8,10,13}` — обе стороны подтверждены декомпиляцией, но СМЫСЛ расхождения не объяснён (см. раздел 9).
- `this+1568` state machine значения (1/2/3) — рабочая интерпретация выведена из контекста логов, не подтверждена явными именами enum.

### TODO / NOT YET REVERSED
- Кто вызывает event-dispatchers `sub_10581640`/`sub_103E1390` (только data-xrefs найдены, реальный "решающий" caller — нет).
- Кто ПИШЕТ в `"game/mmqueue"` (т.е. настоящий обработчик входящего 9107 в `client_panorama.dll`/`client.dll`) — 17 функций-кандидатов найдены, ни одна не декомпилирована.
- `client.dll` — обработчики 9101/9104/9107/9112 НЕ найдены (immediate-scan упал).
- Полное тело `sub_100774E0`, `sub_1009B750`, `sub_10088500` (project446).
- Backend-side player-matching algorithm — недоступен для RE (не в DLL).

---

## 8. Реконструкция протокола — с разделением доказанное/реконструкция

```
Panorama (Play menu)
   ↓ [РЕКОНСТРУКЦИЯ — точный native entrypoint НЕ найден ни в одном дереве/бинарнике]
MatchmakingStart (9101)
   ↓ [CONFIRMED формат сообщения; CONFIRMED что project446 читает mode/prime_pool/ranked_pool/ui/prime/flags]
   ↓ [РЕКОНСТРУКЦИЯ — backend queue/matching algorithm недоступен для RE]
GC → dedicated server: MatchmakingGC2ServerReserve (9105)
   ↓ [CONFIRMED в project446: sub_1009B750, "Forwarded live join 9105 to engine"]
IVEngineServer::ReserveServerForQueuedGame
   ↓ [CONFIRMED: sub_10088500 в project446; CONFIRMED интерфейс в Hydra source]
sv_mmqueue_reservation установлен на сервере (формат "Q...")
   ↓ [CONFIRMED формат, 2 независимых источника]
GC → backend: MatchmakingServerReservationResponse (9106)
   ↓ [CONFIRMED в project446: sub_1009B750, "Sending 9106"]
Match found (клиент получает через GC2ClientUpdate=9104 предположительно, ТОЧНЫЙ путь НЕ подтверждён на клиенте)
   ↓ [РЕКОНСТРУКЦИЯ на клиентской стороне — 9104-обработчик не декомпилирован ни в client.dll, ни точно в project446]
Panorama: popup_accept_match_found (если accept нужен) ИЛИ "@"+map (live join)
   ↓ [CONFIRMED: sub_103DF430 в client_panorama.dll, "game/mmqueue" слушатель]
Player нажимает Accept → LobbyAPI.SetLocalPlayerReady(reason)
   ↓ [CONFIRMED: полная цепочка в client_panorama.dll до this+12 state-write]
   ↓ [РЕКОНСТРУКЦИЯ — как именно от этого state доходит до реального сетевого 9102/accept-сигнала, НЕ прослежено до конца]
MatchmakingStop(9102, abandon=0) — как legacy accept-сигнал
   ↓ [CONFIRMED в project446: и как self-injected (sub_100787F0), и как real-received (sub_100789A0) путь; CONFIRMED в Hydra source как архитектурный паттерн]
"Player ACCEPTED, waiting for all players" (client форвардит наружу)
   ↓ [CONFIRMED: агрегация НЕ на клиенте]
backend аггрегирует accepts (НЕ РЕВЕРСЕНО — на backend)
   ↓
ReportGCQueuedMatchStart (vtable hook на server.dll) — НАДЁЖНЫЙ сигнал готовности
   ↓ [CONFIRMED: реальная функция существует и хукается в project446, слот 47 по умолчанию]
MatchmakingGC2ClientReserve (9107) → клиенту
   ↓ [CONFIRMED формат сообщения, 4 источника; CONFIRMED дедуп в project446]
game/mmqueue KV дерево на клиенте обновляется reservationid/map/server_address
   ↓ [РЕКОНСТРУКЦИЯ — кто именно пишет в это дерево на клиенте, НЕ найдено]
QueueConnect KV-команда строится
   ↓ [CONFIRMED: sub_103DF430, полный набор полей известен]
connect ip:port (через реальный движковый API, НЕ через сырой ConVar)
```

---

## 9. Расхождение Accept-матрицы — НЕ ОБЪЯСНЕНО

| Источник | Функция | Матрица |
|---|---|---|
| `client_panorama.dll` (Panorama-сторона) | `sub_103D7640` @ `0x103D7640` | `{8, 9, 10, 11, 13}` — Competitive, Cooperative, Wingman, ScrimComp5v5, DangerZone |
| `project446/csgo_gc.dll` (GC-сторона) | `sub_100694A0` @ `0x100694A0`, дважды подтверждено | `{8, 10, 13}` — Competitive, Wingman, DangerZone |

**Обе стороны подтверждены прямой декомпиляцией.** Возможные объяснения (НЕ проверены, только гипотезы):
1. `sub_103D7640` может определять "показать какой-то accept-style попап" (более широкий охват UI-поведения), а не "требуется ли GC-side round-trip подтверждение" — то есть это разные концепции с похожим названием.
2. project446 мог сознательно не реализовать полный accept-cycle для Cooperative(9)/ScrimComp5v5(11) — практическое ограничение объёма форка, не протокольная истина.
3. Возможна versioning-разница между тем, что видит Panorama (UI слой) и что реализовано в конкретном community GC (серверная логика).

**Это требует дальнейшего RE** (например, найти, действительно ли реальный клиент шлёт что-то отличное для game_type 9/11 при отсутствии GC-side подтверждения).

---

## 10. Текущие цели проекта

- Настоящий (не через CVar/консольный туннель) GC matchmaking protocol — если технически достижимо через реальный нативный протокол.
- Mode/map selection — должно доходить от Panorama до backend корректно.
- Будущий Java backend (пока не пишем).
- Dedicated server — интеграция резервации.
- Оригинальный Panorama Match Found UI — не менять визуально/XML, только логику подключения.
- Accept для нужных режимов (Competitive/Wingman/DangerZone — как минимум; возможно шире, см. раздел 9).
- Сервер выбирается по mode/map.
- Reservation → connect — полный цикл.
- Bot/server testing для проверки Accept flow (для дальнейшего этапа, не сейчас).
- Без CVar/console-туннеля, если можно реализовать настоящий протокол — но признать, что часть цепочки (native entrypoints от Panorama) НЕ восстановлена, и до полного восстановления возможен гибридный подход.

---

## 11. Что НЕ делать

- Не возвращаться к старому BETA HTTP+JSON backend как основной архитектуре (project446 показывает WebSocket как более production-proven подход, но мы пока ничего не решили окончательно).
- Не копировать DLL автоматически в игру — только по явному запросу, с объявлением что именно копируется.
- Не трогать inventory/skins сейчас (это отдельная, отложенная задача — не связана с текущим matchmaking research; см. `+ip` расследование ниже, тоже отложено).
- Не считать старый (source-tree) `MatchmakingStart`/orchestration код полноценной реализацией — он вырезан в обоих деревьях, это не рабочий референс.
- Не выдавать предположение за подтверждённый протокол — всегда помечать confidence level.
- Не переписывать `csgo_gc` (наш проект) прямо сейчас — это фаза исследования, не имплементации.
- Не переносить код напрямую из project446 — только концепции/паттерны, с пониманием, зачем.
- Не менять файлы `project446` (read-only reference).
- Не делать destructive actions над reference-материалами.
- ICE_Enable диагностика (`LogIceTransportConfigDiagnostic` в `steam_hook.cpp`) — тест не завершён, не удалять код, но и не считать вопрос `+ip`/inventory решённым (см. раздел 14).

---

## 12. NEXT RESEARCH STEPS (в порядке приоритета)

1. **Декомпилировать 17 кандидатов "кто пишет в game/mmqueue"** в `client_panorama.dll` (список — раздел 5, HIGH CONFIDENCE) — это найдёт реальный обработчик 9107 на клиенте.
2. **Повторить immediate-value scan в `client.dll`** для 9101/9102/9104/9107/9109/9110/9112 — БЕЗОПАСНЫМ методом (не прямой Python-перебор по каждой инструкции — использовать `ida_bytes.bin_search` с байтовым паттерном, или Hex-Rays batch-decompile + grep по псевдокоду, или ограничить область поиска конкретными функциями/сегментами).
3. Найти обработчики 9101/9102/9104/9107/9112 **непосредственно в `client.dll`** (не только в `client_panorama.dll`/project446) — это должно быть архитектурно логичным местом (game logic DLL), раз client_panorama.dll оказался неожиданно information-rich, но 9104-handler (mangled lambda hint в project446) намекает, что часть логики может жить и в самом game client тоже.
4. Проследить `ServerReserved` (`sub_103E1390`) — найти РЕАЛЬНОГО caller'а через таблицу диспетчеризации (не просто data-xref, а раскрыть саму таблицу и её consumer).
5. Проверить `ReportGCQueuedMatchStart` слот **в реальном `server.dll` данной версии игры** (project446 использует default=47, но это МОЖЕТ отличаться для нашего конкретного build — нужно RE подтверждение через саму игру, не только project446's config-default).
6. Полностью декомпилировать `sub_1009B750`, `sub_10088500`, `sub_100774E0` в project446 (сейчас только частично).
7. Декомпилировать `CancelConnectToServer`/`QueueConnectToServer`/`MatchAssistedAccept` (`sub_103E04E0`/`sub_103E05A0`/`sub_103E0660`) в `client_panorama.dll`.
8. Определить точный формат reservation, который ожидает клиент (все поля `MatchmakingGC2ClientReserve`, подтверждено — но нужно подтвердить, что КОНКРЕТНО читается на клиенте из каждого поля, не только что оно есть в схеме).
9. Определить, что именно backend отправляет клиенту (общий JSON/WS message envelope, если удастся найти образец где-то — например, если найдутся сетевые логи/дампы трафика, или сериализация в project446 более подробно).
10. Разрешить расхождение Accept-матрицы (раздел 9).
11. Определить **минимальный набор сообщений для первого рабочего solo/casual теста** — вероятно: `MatchmakingStart(9101)` → (сразу, без accept) → `MatchmakingGC2ClientReserve(9107)` → `sv_mmqueue_reservation` → connect. Casual/Deathmatch-подобные режимы (no-accept path) — самый простой первый тест.

---

## 13. Инструменты и команды

### IDA Pro 9.2 + idalib
- Установка: `C:\Program Files\IDA Professional 9.2`
- Python-модуль (headless доступ):
  ```bash
  cd "/c/Program Files/IDA Professional 9.2/idalib/python" && python -m pip install .
  python "/c/Program Files/IDA Professional 9.2/idalib/python/py-activate-idalib.py"
  ```
- Использование в скрипте (Python, **`import idapro` должен быть первым импортом**):
  ```python
  import idapro
  import ida_auto
  idapro.open_database(r"path\to\binary.dll", True)   # True = запустить auto-analysis
  ida_auto.auto_wait()
  # ... IDA API вызовы (idautils, idc, ida_funcs, ida_hexrays, ...)
  idapro.close_database(False)   # False = не пересохранять изменения в БД (мы read-only)
  ```
- Hex-Rays декомпилятор: `import ida_hexrays; ida_hexrays.init_hexrays_plugin(); ida_hexrays.decompile(func_ea)`.
- Открытие УЖЕ проанализированной базы (быстро, без повторного auto-analysis): `idapro.open_database(path, False)` — если рядом лежит `.i64` (или `.id0/.id1/.id2/.nam/.til` для незапакованной формы), IDA переиспользует её.
- **Известная проблема**: `idc.find_binary` НЕ существует в этой версии IDA API — использовать `idautils.Strings()` + фильтрацию вместо этого.
- **Известная проблема**: прямой Python-перебор instruction-by-instruction по всему `.text` (для immediate-value scan) **упал без вывода** на `client.dll` (16МБ) — не повторять этот метод, искать байтовый паттерн через `ida_bytes.bin_search` вместо этого.

### ProtobufDumper
- `C:\Users\Administrator\Downloads\csgo_gc-master\Dumper\ProtobufDumper.exe` (.NET 10, требует `dotnet` runtime или self-contained — уже работал в этой сессии).
- Уже готовые дампы (не нужно перезапускать): `Dumper\client\*.proto`, `Dumper\client_panorama\*.proto`, `Dumper\out_p446_client\`, `Dumper\out_p446_csgo_gc\`, `Dumper\out_p446_panorama\`.
- Кэшированные бинарники в этой папке подтверждены байт-в-байт идентичными текущей установке игры.

### pbin.exe
- `C:\Users\Administrator\Downloads\csgo_gc-master\pbin.exe`, `pbin_source.py` рядом.
- Команда упаковки **обязательно из рабочей директории игры**:
  ```powershell
  Set-Location "D:\SteamLibrary\steamapps\common\csgo legacy\csgo\panorama"
  & "C:\Users\Administrator\Downloads\csgo_gc-master\pbin.exe" pack
  ```

### RE scratchpad (сохранённые IDA-базы, переиспользовать без повторного анализа)
```
C:\Users\Administrator\AppData\Local\Temp\claude\D--csgo2021-gc\353ac6d4-ff59-4e7c-aaa1-5d26308d91ad\scratchpad\re\
    client.dll (+ .id0/.id1/.id2/.nam/.til)
    client_panorama.dll (+ .i64)
    p446_csgo_gc.dll (+ .i64)
    query_*.py — все скрипты запросов этой сессии
    *_out.txt — все результаты
```
⚠️ Это временная папка сессии — может не пережить перезапуск среды. Если недоступна после `/compact`, бинарники нужно скопировать заново из `Downloads\csgo_gc-master\Dumper\` (уже подтверждены идентичными) и повторить `idapro.open_database(path, True)` (займёт ~5-8 минут на файл).

---

## 14. KNOWN DEAD ENDS (не возвращаться без новых оснований)

- **CVar/`joy_name`-туннель для mode/map/accept** — рабочий, но подтверждённо НЕ соответствует ничему в реальном протоколе (ни один source tree, ни один бинарник не использует ConVar для этой цели). Оставлен как fallback, не как целевая архитектура.
- **NSNET (RevEmu)** — расследовано, оказалось НЕ релевантно текущей задаче (RevEmu-специфичный legacy-протокол для очень старых игр, не для реального Steam-based CS:GO 2021 GC).
- **`P2P_Transport_ICE_Enable` гипотеза для `+ip`/inventory проблемы** — диагностика добавлена (`LogIceTransportConfigDiagnostic` в `steam_hook.cpp`), но тест дал **INCONCLUSIVE** результат: `gc_log.txt` показал, что наш `csgo_gc.dll` вообще не был загружен в том конкретном тестовом запуске (файл не пересоздавался — см. `Platform::Initialize()`'s `DeleteFileA("gc_log.txt")`, которое ДОЛЖНО происходить на каждом запуске). Эта задача **отложена**, не решена и не опровергнута. Код диагностики остаётся в дереве, но не активна пока не будет повторно протестирована правильно (нужно подтвердить, что DLL реально загрузился — по обновлению timestamp `gc_log.txt`).
- **Прямой Python instruction-by-instruction immediate scan** по всему `.text` сегменту 16МБ бинарника — падает без диагностируемой ошибки. Использовать `ida_bytes.bin_search` или decompile+grep вместо этого.
- **Предположение, что `client.dll` — главное место для Accept/matchmaking логики** — оказалось ЧАСТИЧНО неверным: `client_panorama.dll` неожиданно оказался гораздо информативнее (содержит `CUiComponent_Lobby`, которого нет в обоих source tree). Не игнорировать `client_panorama.dll` в будущих RE-раундах.

---

## 15. STATE AT COMPACTION

1. **Доказано**: полный набор GC matchmaking message ID (9100-9199 диапазон) идентичен между Hydra-source (2015), 2021-decompiled-source и реальным бинарным дампом `client.dll` — 0 конфликтов номеров полей.
2. **Доказано**: реальная client-side orchestration-логика (кто шлёт `MatchmakingStart`, кто обрабатывает `9104`) **вырезана** из ОБОИХ публичных source-дропов (`/** Removed for partner depot **/`, 140+44 вхождений) — не пробел декомпиляции, осознанное решение Valve.
3. **Доказано**: `CUiComponent_Lobby`/`LobbyAPI.SetLocalPlayerReady(reason: string)` — реальный, работающий native-мост в `client_panorama.dll` (RVA `0x1057F2D0` регистрация, `0x10584F70` callback), отсутствующий в обоих source tree.
4. **Доказано**: `sub_103DF430` (`0x103DF430`, `client_panorama.dll`) — центральная функция, слушающая `"game/mmqueue"` KV-дерево, показывающая Accept-попап и строящая `"QueueConnect"` команду с полями `adronline`/`reservationid`/`map`/`gametype`/`gamemode`.
5. **Доказано**: `ReportGCQueuedMatchStart` (вырезанная из source функция) — реально существует и успешно хукается через vtable в компилированном `server.dll` (project446, `sub_100BADE0` @ `0x100BADE0`, default slot 47, configurable через env).
6. **Доказано**: project446's GC-side Accept-матрица `{8,10,13}` (Competitive/Wingman/DangerZone) — точное совпадение с нашей существующей реализацией, дважды независимо подтверждено декомпиляцией (`sub_100694A0` и `sub_100787F0`).
7. **Доказано**: "все ли приняли" считается на **backend**, не в клиентском GC DLL (ни у нас, ни в project446, ни в реальном `client_panorama.dll`) — клиент только форвардит accept-сигнал наружу.
8. **Найдено, но НЕ объяснено**: расхождение Accept-матрицы между Panorama-стороной (`{8,9,10,11,13}`) и GC-стороной project446 (`{8,10,13}`).
9. **Сейчас находимся**: закончили live-RE трёх источников (2 source tree + 2 боевых бинарника + 1 сторонний компилированный GC). Все находки записаны в этот файл.
10. **Следующим исследуем**: кто пишет в `"game/mmqueue"` на клиенте (17 функций-кандидатов в `client_panorama.dll`, ни одна не декомпилирована) — это найдёт настоящий клиентский обработчик `9107`.
11. **Главный технический вопрос, который остался открытым**: как ИМЕННО Panorama UI (native entrypoint от кнопки "Find a Game"/Play) инициирует отправку `MatchmakingStart` — этот путь НЕ найден ни в одном из пяти исследованных источников (2 source tree, 2 боевых DLL, 1 сторонний GC). Это единственное по-настоящему недостающее звено для полной картины протокола "от клика до 9101".
12. IDA-базы для `client.dll`, `client_panorama.dll`, `p446_csgo_gc.dll` уже проанализированы и сохранены в scratchpad — переоткрывать без `True` (auto-analysis) флага, экономит ~5-8 минут на файл.
13. Портированный build toolchain и `build_local.bat` рабочие, база проекта (`csgo_gc/gc_client.cpp`) содержит 2 применённых фикса (`required_appid_version=1352`, удалён `required_appid_version2`) плюс временную ICE-диагностику (не завершённую).
14. `+ip`/inventory проблема (отдельная, более ранняя ветка исследования) — **отложена**, не решена, не входит в текущий фокус matchmaking-research.
15. Ничего не закоммичено в git сверх исходных 2 коммитов форка — текущие правки в `gc_client.cpp`/`steam_hook.cpp`/`build_local.bat`/`.gitignore` остаются в рабочем дереве, не закоммичены.
