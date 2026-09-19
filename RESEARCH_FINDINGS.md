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

---

## 16. ПРОРЫВ (продолжение после compaction): реальный GC-job dispatch в `client_panorama.dll` НАЙДЕН — `CONFIRMED`

Эта сессия проверяла гипотезу: «можно ли вручную инициировать штатный Valve pipeline, просто прислав клиенту корректно сформированные GC-сообщения через уже существующий механизм нашего `SendMessageToGame`, вместо переписывания клиентской логики». Ответ на **9104/9107 (GC→Client) сторону — ДА, подтверждено RTTI-данными**, см. ниже.

### Реальная GC-SDK job-таблица в `client_panorama.dll` — `CONFIRMED` (RTTI type descriptor + данные, не эвристика)

Продолжили декомпиляцию 16 кандидатов "кто пишет в game/mmqueue" (раздел 5, TODO). Два оказались буквально протобуф-обработчиками (объявляют `CProtoBufMsg<T>::\`vftable'` как первое поле своего объекта):

- **`sub_103DC4B0`** (RVA `0x103DC4B0`) — конструирует `GCSDK::CProtoBufMsg<CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve>`, вызывает `sub_1078B580` (= подтверждено по VProfile-строке `"CProtoBufMsg::InitFromPacket( IMsgNetPacket )"` — это буквально `GCSDK::CProtoBufMsg<T>::InitFromPacket`), затем читает `game/mmqueue`, при флаге `(msg+48)&0x20` обновляет `"Update/game/map"`, вызывает `sub_103DFBE0(v31)` (копирует поля развёрнутого протобуфа — reservationid/map/etc — в объект состояния, тот же класс, что слушает `sub_103DF430`), логирует `"Matchmaking reservation confirmed: %llx/%s\n"`.
- **`sub_103D9900`** (RVA `0x103D9900`) — конструирует `GCSDK::CProtoBufMsg<CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate>`, логирует `"Matchmaking update: %d\n"`, `"Matchmaking waiting for %d accounts (%X, ...)\n"`.

**Прямое подтверждение через RTTI и статическую job-таблицу** (дамп данных вокруг адресов вызова, `query_panorama_dispatch_table.py`):

| Адрес таблицы | RTTI type descriptor (реальное имя класса Valve) | Handler | Msg ID (найден как соседнее поле данных) |
|---|---|---|---|
| `0x10b6a4c4` | `??_R4ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve@@6B@` | `sub_103DC4B0` | `0x10b6a4d8` = **0x2393 = 9107** ✅ точное совпадение |
| `0x10b6a5c8` | `??_R4ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate@@6B@` | `sub_103D9900` | (соседняя запись другого job'а: `0x2396`=9110, структура подтверждает паттерн массива job-объектов) |
| `0x10b6a408` | `??_R4CServerConfirmedReservationCheckCallback@@6B@` | `sub_103DF430` | — (это не job, а KV change-callback объект, имя класса теперь известно точно) |
| рядом (`0x10b6a3f8`) | `??_R4?$CProtoBufMsg@VCMsgGCCStrike15_v2_MatchmakingStart@@@GCSDK@@6B@` | — | подтверждает, что объект-обёртка для ИСХОДЯЩЕГО `MatchmakingStart` (9101) тоже существует в этом бинарнике (сам send-call site не найден, `sub_103E0AB0`/`sub_103E0B10` рядом — это generic-деструкторы объекта сообщения, не сам send) |

Строки класса рядом в этой же таблице (`aClientjobEmsgg_*`): `ClientJob_EMsgGCCStrike15_v2_GC2ClientTournamentInfo`, `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`, `ClientJob_EMsgGCCStrike15_v2_ClientPartyWarning` — подтверждает, что это классический паттерн Source Engine GCSDK: массив статически сконструированных `CGCClientJob`-наследников, каждый со своим RTTI vtable и захардкоженным message-ID полем. **Это ТОТ ЖЕ механизм, что использует наш собственный `csgo_gc.dll` на стороне сервера/клиента (GC_REG_CLIENT_JOB и `HANDLE_MSG`-таблица в `gc_shared.cpp`) — архитектурно идентичен.**

**Побочная находка (не проверено дальше, не путать с confirmed-списком message ID)**: `0x10b6a424` содержит dword `0x23CF = 9167` рядом с `CServerConfirmedReservationCheckCallback`'s таблицей — это **вне** диапазона задокументированных в разделе 2 ID (9100-9117, 9142, 9189-9192). Может быть msg ID необнаруженного ранее сообщения (например, более позднего client2GC подтверждения) либо просто соседняя невязанная константа. **Не делать выводов, требует отдельной проверки.**

### Ключевой вывод для feasibility (Этап B пользовательского запроса)

1. **`client_panorama.dll` действительно содержит рабочий, зарегистрированный обработчик для 9107** (`ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` → `sub_103DC4B0`), который читает протобуф, обновляет объект состояния (тот же, что слушает `game/mmqueue`), и логирует подтверждение резервации. Это ПОДТВЕРЖДАЕТ (не просто вероятно), что путь `9107 → game/mmqueue → QueueConnect` — рабочий штатный Valve-код, НЕ вырезанный из бинарника (хотя вырезан из ОБОИХ source tree).
2. **Наш собственный проект уже имеет полностью рабочий, используемый в продакшене (для 9110 Hello) механизм отправки произвольного GC→Client protobuf-сообщения**: `ClientGC::SendMessageToGame(false, msgType, protobufMessage)` (`gc_client.cpp:182-193`, вызывается для `k_EMsgGCClientWelcome` и `k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello` в `OnClientHello`, строки 306/311). Это ставит сообщение в очередь `PostToHost(HostEvent::Message, ...)`, откуда клиент забирает его через захуканный `SteamGameCoordinatorProxy::RetrieveMessage` (`steam_hook.cpp:255-266`).
3. **Вывод**: чтобы вручную инициировать штатный Valve pipeline `9107 → game/mmqueue → QueueConnect`, НЕ требуется вызывать функции `client_panorama.dll` по RVA (раздел 13/14 исходного запроса пользователя) — достаточно **сконструировать корректный `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` и вызвать уже существующий `SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve, reserveMsg)`**. Схема сообщения (раздел 2) подтверждена идентичной во всех 4 источниках — 0 расхождений, самое надёжное поле во всём проекте.
4. **Встречная сторона (Client→GC, 9101) — ПРОБЕЛ в нашем коде, не в бинарнике клиента**: `ClientGC::HandleMessage` (`gc_client.cpp:46-...`, switch по `messageRead.TypeUnmasked()`) **не имеет `case k_EMsgGCCStrike15_v2_MatchmakingStart`** — то есть наш GC сейчас **полностью игнорирует** реальный 9101, который клиент точно шлёт (подтверждено: `project446/csgo_gc.dll`'s `sub_10077C90` активно его парсит; RTTI подтверждает существование `CProtoBufMsg<MatchmakingStart>` объекта в `client_panorama.dll`, то есть клиент умеет его строить и отправлять через тот же захуканный `SendMessage`). Это единственный недостающий кусок нашего кода для проверки гипотезы — не найти, а ДОБАВИТЬ обработчик.

### Обновление раздела 12 (NEXT RESEARCH STEPS)
- Пункт 1 («кто пишет в game/mmqueue») — **РЕШЕНО**: `sub_103DC4B0` (9107 handler) → `sub_103DFBE0` (полевой копир) → тот же state-объект, что слушает `sub_103DF430`.
- Пункт 3 («обработчики 9101/9104/9107 в client.dll») — **уточнено**: они не в `client.dll`, а в `client_panorama.dll` (как и предполагалось в разделе 14 dead-ends: "client_panorama.dll оказался информативнее"). 9104/9107 найдены. 9101 (send) и 9102/9112 (send/recv) — RTTI-объект для 9101 найден рядом, но сам send-call-site — нет (не критично: нам нужен ПРИЁМ 9101 в нашем GC, не в клиенте).
- **Новый приоритетный шаг**: минимальный патч в `gc_client.cpp` — добавить `case k_EMsgGCCStrike15_v2_MatchmakingStart` в `HandleMessage`, распарсить реальный `game_type`, собрать тестовый `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (game_type=7/Casual, чтобы не задевать нерешённое расхождение Accept-матрицы, раздел 9) и отправить через существующий `SendMessageToGame`. См. `[MM-TEST]`-диагностику, добавляемую в этом же патче.

### Инструменты этого RE-раунда (сохранено в scratchpad, переиспользуемо)
`query_panorama_mmqueue_candidates.py`, `query_panorama_handlers.py`, `query_panorama_dispatch_table.py`, `query_panorama_mmstart_sender.py` — все в `.../scratchpad/re/`, выходы `mmqueue_candidates_out.txt`, `handlers_out.txt`, `dispatch_table_out.txt`, `mmstart_sender_out.txt`.

---

## 17. LIVE TEST #1 (Casual, стаб-9107) — разбор по логам — `CONFIRMED`/`UNKNOWN` разделены

### Где именно проходил тест — важное расхождение с разделом 1
Реальный тестовый запуск использовал **`C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\`**, а НЕ `D:\SteamLibrary\...`, который был задокументирован в разделе 1 как основной. Обе установки существуют параллельно. `csgo_gc\csgo_gc.dll` в C:-установке имеет timestamp, совпадающий с билдом этой сессии — подтверждено, что тестировалась именно свежая сборка. **Для всех будущих тестов проверять именно `C:\Program Files (x86)\Steam\...\csgo legacy\csgo\console.log`.**

### Наш `Platform::Print` — куда реально пишет — `CONFIRMED` (по коду, `platform_windows.cpp:24-53`)
`Print()` **всегда** (если `GetConfig().GetLogOutput() > LogOutputNone`) зовёт `tier0!ConColorMsg("[GC] %s", ...)` — то есть пишет **в игровую консоль**, откуда `-condebug`/`con_logfile` пишет в `csgo/console.log`. Запись в `gc_log.txt` — **отдельная, опциональная** ветка (`if (logOutput >= LogOutputFile)`), не связана с console.log. В тестовой установке `gc_log.txt` **не создался вообще** (проверено — файла нет нигде в дереве C:-установки), что означает `LogOutput` там настроен НЕ на `LogOutputFile` (скорее всего `LogOutputConsole`) — это **НЕ повторение старой `+ip` "DLL не загрузился"** проблемы: DLL точно загрузился и работал (см. ниже), просто конфиг логов другой. **Важно на будущее**: не путать отсутствие `gc_log.txt` с "DLL не загружен" — теперь нужно всегда сначала проверять `console.log`.

### Факт: наш GC получал сообщения на всём протяжении сессии — `CONFIRMED`
`console.log` содержит наш собственный default-case лог `"[GC] ClientGC::HandleMessage: unhandled protobuf message k_EMsgGCCStrike15_v2_..."` многократно на протяжении всей игровой сессии (для `GetEventFavorites_Request`, `MatchmakingStop` — 4 раза в разных местах лога, на строках 623, 1667, 2780, 3835) — подтверждает, что message pump и хук `RetrieveMessage`/`SendMessage` работали стабильно на всём протяжении теста, не только в момент нажатия Play.

### Точная последовательность (`console.log`, строки 3781-3847, единственный релевантный фрагмент — файл обрывается на Host_Shutdown вскоре после)
```
3781  [unhandled] GetEventFavorites_Request
3783  [MM-TEST] MatchmakingStart received: game_type=519 accounts=1 prime_only=1 client_version=13805
3784  [MM-TEST] Sending stub MatchmakingGC2ClientReserve: map=de_dust2 server=127.0.0.1:27015 reservationid=293a206f6c6c6548
3785  [MM-TEST] MatchmakingGC2ClientReserve dispatched
3786-3833  (обычный SDR/relay ping шум, "Ping measurement completed" на 3814 — подтверждает, что прошло заметное реальное время, не мгновенно)
3834  [unhandled] GetEventFavorites_Request
3835  [unhandled] MatchmakingStop           <-- клиент сам прислал нам 9102
3836  **** Unable to localize '#GenericConfirmText_Label' on panel descendant of 'PopupManager'   <-- вероятно сам попап "Failed to connect"
3837  [unhandled] GetEventFavorites_Request
3838  Host_Shutdown (пользователь закрыл игру)
```

### `game_type=519` — РАСШИФРОВАНО, `HIGH CONFIDENCE` (совпадение с формулой из Hydra source, раздел 4)
519 = `0x207`. Раздел 4 документировал `MatchmakingGameTypeCompose(eGame, eMapGroup) = (eGame&0xF)<<0 | (eMapGroup&0xFFFFFF)<<8` как реальную, но "нигде не вызываемую в Hydra-дереве" функцию. Разбор: `eGame = 519 & 0xF = 7` (Casual — то, что пользователь реально выбрал!), `eMapGroup = (519>>8) & 0xFFFFFF = 2`. `7 | (2<<8) = 519` — **точное совпадение**. Это первое ЖИВОЕ подтверждение, что реальный клиент 2021 действительно составляет `game_type`-поле в `MatchmakingStart` через эту компоновку (game_type И mapgroup ID запакованы в одно uint32), а не шлёт голый game_type (7). **Наш текущий тестовый код читает `request.game_type()` напрямую и нигде не декомпозирует его** — это не баг для данного теста (мы не использовали значение для ветвления), но обязательно для следующего патча. mapgroup ID `2` пока не сопоставлен ни с одним конкретным mapgroup-именем — `unknown`, требует таблицы соответствия (не найдена в этой сессии).

### Разбор pipeline по шагам — именно то, что просил пользователь

| Шаг | Статус | Обоснование |
|---|---|---|
| 9101 получен нашим GC | **CONFIRMED** | `console.log:3783`, реальные поля видны |
| 9101 распарсен | **CONFIRMED** | `ReadProtobuf` не мог бы дать `game_type=519 accounts=1 prime_only=1 client_version=13805`, если бы парсинг упал (в этом случае сработал бы `Parsing ... failed` лог, которого нет) |
| game_type | **CONFIRMED** = 519 (составное) → decoded eGame=7 (Casual) | см. выше |
| game_mode | **UNKNOWN** | `CMsgGCCStrike15_v2_MatchmakingStart` не содержит отдельного поля game_mode (раздел 2, поля: account_ids/game_type/ticket_data/client_version/tournament_match/prime_only/tv_control/lobby_id) — если game_mode кодируется, то только внутри `ticket_data` (opaque string, не распарсен в этой сессии) |
| 9107 сконструирован | **CONFIRMED** | `console.log:3784`, видны все выставленные нами поля |
| 9107 отправлен через `SendMessageToGame`/`PostToHost` | **CONFIRMED** | `console.log:3785` "dispatched" + сам код `SendMessageToGame` не имеет условий отказа (см. `gc_client.cpp:182-193`) — сообщение гарантированно положено в `m_hostEvents` |
| 9107 дошёл до `RetrieveMessage`, который дёргает клиент | **NOT CONFIRMED, NOT REFUTED — UNKNOWN** | нет прямого лога с нашей или клиентской стороны, подтверждающего сам факт `RetrieveMessage` вызова именно с этим сообщением; архитектурно должно произойти (это тот же путь, что уже используется для 9110 Hello, который штатно работает), но не пронаблюдано напрямую в этом тесте |
| `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`/`sub_103DC4B0` выполнился | **UNKNOWN** (не NO!) | КЛЮЧЕВОЙ МЕТОДОЛОГИЧЕСКИЙ ФАКТ: все Valve-нативные `DevMsg(...)` вызовы (включая `"Matchmaking reservation confirmed: %llx/%s"`, `"Server reservation check %p ..."`) **гейтятся cvar'ом `developer`**. В `console.log` **нет вообще ни одного DevMsg-уровня сообщения от движка за всю сессию**, и нет упоминаний `developer` в логе — значит `developer` почти наверняка был `0` (по умолчанию) в этом тесте. **Отсутствие этих строк в логе НИЧЕГО не доказывает** — оно означает лишь, что DevMsg не печатался, а не что код не выполнился. Это не ошибка кода, а пробел методики теста. |
| `sub_103DFBE0` (запись reservation-полей в state) | **UNKNOWN** | зависит от предыдущего пункта |
| `game/mmqueue` изменился | **UNKNOWN** | не пронаблюдано напрямую; KeyValues-дерево не логируется никуда, что мы видели |
| `sub_103DF430` выполнился | **UNKNOWN** | тот же DevMsg-гейтинг применяется к обеим веткам (`ready-up!`/`queue connect`) |
| Ветка (`ready-up` vs `queue-connect`) | **UNKNOWN** | не определено |
| `QueueConnect` выполнен | **UNKNOWN, но косвенно скорее НЕТ** | если бы `QueueConnect` реально построился и отработал, ожидался бы попытка `connect 127.0.0.1:27015` и в норме — engine-уровневое сообщение о попытке подключения (`"Connecting to 127.0.0.1:27015..."`-подобное, обычно НЕ DevMsg-гейтится, это обычный `Msg()`). В логе **такого сообщения нет вообще** — ни успеха, ни явного отказа на движковом уровне. Это ослабляет (но не исключает) гипотезу, что `QueueConnect`/`connect` реально был вызван |
| Фактический connect target | **UNKNOWN** | не залогирован нигде, куда у нас есть доступ |
| Итоговая ошибка "Failed to connect to the match" | **CONFIRMED со стороны пользователя (визуально), NOT в console.log** | это, вероятно, локализованный UI-текст (Panorama popup), не консольная строка — не найден в console.log дословно; строка `"Unable to localize '#GenericConfirmText_Label'"` на 3836 по времени точно совпадает и, весьма вероятно, ЭТО и есть генерация того самого попапа (обобщённый confirm-диалог, использованный для сообщения об ошибке), но это **HIGH CONFIDENCE, не CONFIRMED** — прямой связи (какой конкретно попап вызвал `#GenericConfirmText_Label`) не установлено |

### `MatchmakingStop` (9102) от клиента — ОСТОРОЖНО, вероятно НЕ связано с нашим тестом — важная методологическая находка
`k_EMsgGCCStrike15_v2_MatchmakingStop` встречается в логе **4 раза**: строки 623, 1667, 2780 (все — **задолго до** MM-TEST на 3783) и 3835 (после). Три из четырёх случаев произошли без всякой связи с нашим тестом — значит клиент шлёт `MatchmakingStop` **периодически/рутинно** в рамках обычной навигации по меню (вероятно cleanup стейл-резерваций при входе/выходе из разных экранов), а не исключительно как реакцию на провал конкретного поиска. **Нельзя делать вывод, что MatchmakingStop на 3835 — прямое следствие нашего 9107/таймаута этого конкретного поиска** — требует отдельной проверки (например, повторный тест с более чёткими временными метками, `con_timestamp 1`). Наш GC ни разу за сессию не обработал ни один из 4 случаев (`unhandled`) — это существующий, отдельный от Matchmaking-Start-эксперимента, пробел (обработчика 9102 у нас нет вообще).

### JobId — непроверенная, но конкретная гипотеза для необъяснённого "молчания" клиента
`GCMessageRead::JobId()` (`gc_message.h:17`) — существует и используется в проекте для других сообщений с ответом-в-job (например, `StorePurchaseInit`/`StorePurchaseFinalize` отвечают через `SendMessageToGame(..., messageRead.JobId())`). Наш тестовый `OnMatchmakingStart` **не читал и не логировал `messageRead.JobId()` полученного 9101**, и отправил 9107 с `jobId = JobIdInvalid` (значение по умолчанию). **Не проверено**: ожидает ли `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` со стороны GCSDK, что ответ придёт как continuation конкретного job'а (тогда `JobIdInvalid` сделал бы наше сообщение невидимым для клиентской job-системы, независимо от корректности самого протобуфа), или это полностью самостоятельное, несвязанное GC-initiated сообщение (как 9110 Hello, который штатно работает с `JobIdInvalid`). **Это гипотеза, не факт** — следующий приоритетный RE-шаг.

### Сравнение с project446 — что project446 реально делает с полями 9107 (для справки, НЕ копировать бездумно)
Из раздела 6: project446's `sub_1009B750` строит 9107 **после реального 9105/9106 relay через настоящий dedicated server** (`"Sending 9106: reservationId=%llu map='%s' wsClient=%p"`), то есть в реальном пайплайне project446 9107 **всегда следует** за подтверждённой резервацией от backend, а не отправляется как немедленный ответ на 9101 напрямую (в отличие от нашего теста, что было осознанным упрощением для первого эксперимента). project446 также использует `sub_100774E0`'s dedup-логику ("Skip duplicate 9107 resId=%llu") — предполагает, что клиент способен получить 9107 многократно/предварительно, что не противоречит нашему тесту. **Не найдено доказательств**, что project446 передаёт JobId по-другому — это тоже не реверсено (см. раздел 6 TODO).

### ВЫВОД для следующего шага (диагностика, БЕЗ изменения кода в рамках этой сессии)
1. **Методология теста должна измениться первой, раньше кода**: следующий тест обязателен с `developer 2` (или хотя бы `1`) и желательно `con_timestamp 1` в автостарт-конфиге/консоли ДО захода в Play — иначе все "UNKNOWN" из таблицы выше останутся UNKNOWN бесконечно, независимо от того, что мы поменяем в коде.
2. Приоритетная гипотеза №1 (проверяется без кода, только RE): выяснить, требует ли `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` совпадения JobId с исходным 9101 (или иного pending-state токена) — искать в `client_panorama.dll` конструктор/фабрику этого job'а (кто его СОЗДАЁТ и на основании чего решает, что ответ релевантен) — сейчас у нас есть только сам handler-метод (`sub_103DC4B0`), но не факт его создания/условий приёма.
3. Приоритетная гипотеза №2: "Confirming match..." экран, возможно, управляется через 9104 (GC2ClientUpdate), а не напрямую через 9107 — мы НИ РАЗУ не отправили 9104 в этом тесте. Проверить в RE: есть ли в `client_panorama.dll`/Panorama-слое состояние, зависящее конкретно от получения 9104 ПЕРЕД тем, как 9107 будет принят к рассмотрению.

---

## 18. РЕШЕНО (RE, без нового live-теста): полный код `sub_103DC4B0` найден — причина провала теста #1 установлена на уровне кода, `CONFIRMED`/`HIGH CONFIDENCE`

Получена ПОЛНАЯ декомпиляция `sub_103DC4B0` (788 байт, ранее видели только первые ~115 строк). Это меняет почти все `UNKNOWN` из таблицы раздела 17 на обоснованные выводы.

### Реальная структура функции — `CONFIRMED` (полное тело)
```c
char __stdcall sub_103DC4B0(int a1)   // a1 = сырой IMsgNetPacket нашего 9107
{
    // ... строит CProtoBufMsg<MatchmakingGC2ClientReserve>, v31 = деcериализованное сообщение
    sub_1078B580(a1);  // InitFromPacket — парсинг нашего 9107 в v31

    v1 = (...dword_15280E10 + 52...)(dword_15280E10);   // получить какой-то менеджер/сессию
    if ( v1 )
    {
        (...)(v1);   // AddRef/Lock?
        if ( *(_BYTE *)sub_108F9A90("game/mmqueue", Locale) )   // <-- ГЛАВНЫЙ GATE
        {
            // ветка A: "у меня ЕСТЬ активная запись game/mmqueue"
            // ... обновляет map (если флаг 0x20 у reservation), sub_103DFBE0(v31) (копирует поля),
            // DevMsg("Matchmaking reservation confirmed..."),
            // v10 = (*(v9+32)) & 0xF, где v9 = v31+32 (nested `reservation` msg, ЕСЛИ она есть)
            //       иначе fallback v9 = dword_10D9A180+32 (ГЛОБАЛЬНОЕ клиентское состояние поиска)
            // if (v10 in {8,9,10,11,13}) → sub_103DF1C0(v34, 1, 0)   // accept-required режим
            // else                       → sub_103DF1C0(v34, 2, 2)   // direct queue-connect режим
            // (это и есть создание/обновление CServerConfirmedReservationCheckCallback,
            //  этим объясняются `this+140`/`this+132` из sub_103DF430, раздел 5)
        }
    }
    else /* ветка B: game/mmqueue ПУСТ/false */
    {
        // строит CMsgGCCStrike15_v2_MatchmakingStop и ОТПРАВЛЯЕТ его ОБРАТНО В GC
        // через GCSDK::CProtoBufGCClientSendHandler (sub_1078B640)
    }
}
```

### ГЛАВНЫЙ ВЫВОД — `HIGH CONFIDENCE` (код подтверждён декомпиляцией; причинно-следственная связь с конкретным live-тестом — по совпадению типа сообщения и таймингу, не по DevMsg)

**`sub_108F9A90("game/mmqueue", Locale)` в начале функции — это gate-проверка: "есть ли у клиента уже АКТИВНАЯ, локально известная запись `game/mmqueue`".** Если НЕТ (пусто/false) — обработчик **НЕ** обновляет резервацию и **НЕ** идёт к `game/mmqueue`/`QueueConnect` вообще. Вместо этого он **сам конструирует и отправляет обратно в GC `CMsgGCCStrike15_v2_MatchmakingStop`** — то есть явно говорит GC "у меня нет активного поиска, на который можно было бы это принять".

**Это ТОЧНО совпадает с тем, что мы видели в live-тесте**: наш 9107 ушёл (`console.log:3785`), и вскоре после (`console.log:3835`) клиент прислал нам именно `MatchmakingStop` — тот самый механизм, теперь найденный в коде. Это резко повышает уверенность (с "вероятно, рутинный шум" до **HIGH CONFIDENCE прямой причины**), что три БОЛЕ�е ранних вхождения `MatchmakingStop` в логе (строки 623/1667/2780, до MM-теста) — независимый шум/рутинная очистка при навигации по меню, а вхождение на 3835 — **прямая, кодово-обоснованная реакция именно на наш 9107**, потому что путь для этого теперь буквально виден в декомпиляции.

### Почему `game/mmqueue` был пуст — рабочая гипотеза, `NOT YET CONFIRMED`, следующий шаг
`game/mmqueue`-корень, судя по всему, выставляется в true/присутствует ТОЛЬКО когда клиент официально знает, что его поиск зарегистрирован GC — вероятно, это происходит при получении `MatchmakingGC2ClientUpdate` (9104) **до** 9107, а не мгновенно при отправке 9101. **Мы НИ РАЗУ не отправили 9104** в этом тесте — сразу ответили 9107 на голый 9101. Это ПОЛНОСТЬЮ объясняет наблюдаемое поведение без необходимости привлекать гипотезу про JobId или про DevMsg-гейтинг для объяснения ГЛАВНОГО провала (хотя DevMsg-гейтинг остаётся отдельной, отдельно верной причиной, почему мы не увидели `"Matchmaking reservation confirmed"` — та строка была бы напечатана только в ветке A, в которую мы не попали, так что её отсутствие теперь дважды объяснено: (1) ветка A не выполнилась, (2) даже если бы выполнилась, DevMsg мог быть не виден без `developer 1`).

### Второстепенная находка — наш стаб не заполнял nested `reservation` — `CONFIRMED` (по коду)
Даже если бы gate прошёл, `v9 = *(v31+32)` (похоже, указатель на nested `CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve reservation`, поле 5 в схеме, раздел 2) был бы `0`, потому что наш тестовый код НЕ вызывал `reserve.mutable_reservation()`. Код корректно **fallback**-ится на `dword_10D9A180+32` (глобальное клиентское состояние) в этом случае — то есть это НЕ обязательно фатально само по себе, но означает, что наш 9107 сейчас неполный относительно того, что реальный GC обычно присылает (project446 тоже всегда relay'ит через 9105/9106 прежде чем строить 9107 — раздел 6 — согласуется с тем, что nested `reservation` в реальности заполняется из данных, полученных через полный 9105/9106 цикл).

### Обновление таблицы раздела 17 — статусы, которые теперь можно уточнить
- `sub_103DC4B0` выполнился — было `UNKNOWN`, теперь **HIGH CONFIDENCE: ДА, выполнился, но пошёл по ветке B (game/mmqueue пуст) и НЕМЕДЛЕННО вышел, отправив `MatchmakingStop`** — не по ветке A (обновление резервации).
- `sub_103DFBE0` — было `UNKNOWN`, теперь **HIGH CONFIDENCE: НЕ выполнился** (это внутри ветки A, которая не была достигнута).
- `game/mmqueue` изменился — было `UNKNOWN`, теперь **HIGH CONFIDENCE: НЕТ, не изменился** (ветка B не трогает game/mmqueue вообще).
- `sub_103DF430` выполнился — было `UNKNOWN`, теперь **HIGH CONFIDENCE: НЕТ** (ничего не писало в game/mmqueue, значит нечему было триггерить KV-listener).
- `QueueConnect` выполнен — было `UNKNOWN, но косвенно скорее НЕТ`, теперь **HIGH CONFIDENCE: НЕТ**, полностью объяснено кодом, а не только отсутствием лога.
- `MatchmakingStop` от клиента на 3835 — было "возможно рутина", теперь **HIGH CONFIDENCE: прямое следствие нашего 9107**, конкретный код-путь найден.

### Следующий диагностический (НЕ кодовый, по прямому требованию пользователя) шаг
Нужно RE (не live-тест) выяснить: что именно устанавливает `"game/mmqueue"` root-флаг в true на клиенте — конкретно, декомпилировать обработчик 9104 (`sub_103D9900`, раздел 16, TODO — тело пока не декомпилировано, только сигнатура/начало) и посмотреть, пишет ли ОН в `"game/mmqueue"` root. Это наиболее вероятный кандидат на "недостающее звено", раз 9107-обработчик явно ожидает, что этот флаг уже true к моменту его прихода.

---

## 19. Live MM: game/mmqueue writer and Play→9101 lifecycle

### [CONFIRMED]

**`client.dll` содержит ПОЛНУЮ ДУБЛИРУЮЩУЮ КОПИЮ всей matchmaking-job-инфраструктуры, найденной ранее в `client_panorama.dll`** — не отдельный, а буквально тот же исходный код, скомпилированный дважды в два разных модуля:
- `client.dll` строки `0x10bcaf68`-`0x10c1c874`: RTTI `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello/ClientUpdate/Client2ServerPing/ClientReserve/ClientAbandon/GCOperationalStats` — то же множество классов, что и в `client_panorama.dll` (раздел 16).
- `sub_103F49F0` (`client.dll`) побайтово структурно идентична `sub_103DC4B0` (`client_panorama.dll`, 9107-handler, раздел 18) — тот же gate по `"game/mmqueue"`, тот же fallback на `MatchmakingStop`, тот же `& 0xF`-декод game_type, та же ветка `sub_103F7980(v34, 1/2, 0/2)` (аналог `sub_103DF1C0`).
- `sub_103F1C00` (`client.dll`) побайтово структурно идентична `sub_103D9900` (`client_panorama.dll`, 9104-handler) — тот же gate, тот же fallback.
- Вывод: `client.dll` и `client_panorama.dll` — не архитектурно разные модули с разной ответственностью за matchmaking, а два **экземпляра одной и той же скомпилированной библиотеки matchmaking-job'ов** (вероятно, общий статически слинкованный исходник, включённый в оба таргета сборки). Live-тест RESULT из раздела 17/18 применим к ОБОИМ модулям одинаково.

**Найден точный словарь строковых значений `game/mmqueue`** — только в `client.dll` (эти литералы ОТСУТСТВУЮТ в `client_panorama.dll`, где есть только сам путь `"game/mmqueue"` и `"Delete/game/mmqueuestop"`):
```
"Delete { system { lock #empty# } game { mmqueue #empty# } } "     @ 0x10bcae78
"Update { system { lock #empty# } game { mmqueue searching } } "   @ 0x10bcaee8
"Update { system { lock mmqueue } game { mmqueue connect } } "     @ 0x10bcaf28
"Update { system { lock mmqueue } game { mmqueue reserved } } "    @ 0x10bcc020
"Update { game { mmqueue heartbeating } } "                         @ 0x10bcc574
"Update { game { mmqueue registering } } "                          @ 0x10bcc5c0
"Delete/game/mmqueuestop"                                            @ 0x10bcc678
```
Это КОНЕЧНЫЙ АВТОМАТ значений `game/mmqueue`: `#empty#` (нет активного поиска) → `searching` → (`registering` / `heartbeating` — промежуточные) → `reserved` (после успешной резервации) → `connect` (готов подключаться). Формат `"Update { system { lock X } game { mmqueue Y } } "` — целый KeyValues-текстовый блок, парсится и мёржится в сессию одним вызовом (конвенция `Update {...}`/`Delete {...}` — broadcast-команда всей legacy-сессии, раздел 3).

**Прочитанные в этой сессии функции НЕ являются writer'ом, а являются либо readers, либо cancel-path, либо message-builder'ами:**
- `sub_103F2150` (client.dll, 7911 байт) — единственный caller: конец `sub_103F1C00` (9104-handler), ветка "нет активного matchmaking-статуса" (`v37[42]==0`, т.е. сам GC прислал 9104 без поля `matchmaking`). Это гигантский строковый классификатор ПРИЧИНЫ остановки/локализованное сообщение (аналог `"#SFUI_QMM_State_%s_%s"` из `sub_103DE7C0`), не писатель состояния.
- `sub_103F6240` (client.dll, 2956 байт, содержит "reserved"/"heartbeating"/"registering") — вызывается ТОЛЬКО через data-таблицы (`0x10bccbf8`, `0x10c25798`), нет code-callers — паттерн идентичен `sub_103DF430`/`sub_1057F760` (periodic-tick/KV-listener callback), то есть это, вероятно, ПЕРИОДИЧЕСКИЙ heartbeat/watchdog поверх УЖЕ существующего состояния, а не изначальный setter.
- Все 19+11=30 функций (обе DLL), прямо ссылающиеся на литерал `"game/mmqueue"`, — это READERS (проверка текущего значения) либо CANCEL-builders (`sub_103DE630`/аналог в client.dll — строит и шлёт `MatchmakingStop`), НИ ОДНА не содержит прямого `SetString("game/mmqueue", "searching")`-подобного вызова с ЛИТЕРАЛЬНЫМ путём "game/mmqueue" как первым аргументом.

### [HIGH CONFIDENCE]

- **Реальный writer использует НЕ путь `"game/mmqueue"` напрямую, а один из шести `"Update {...}"/"Delete {...}"` KV-блоков целиком**, парсящихся одним вызовом (вероятно `KeyValues::LoadFromBuffer` + merge-в-сессию), поэтому наш посимвольный поиск литерала `"game/mmqueue"` НЕ находит сам момент установки значения "searching" — тот код оперирует ЦЕЛЫМ блоком-константой, а не отдельным path+value. Отсюда и то, что все 30 найденных функций-читателей выглядят "изолированно" от записи: запись, скорее всего, происходит в функции, ссылающейся на строку `0x10bcaee8` целиком (мы установили только ОДНОГО известного пользователя — конец `sub_103F1C00`, но именно КАКОЙ ветвью/вызовом эта строка реально передаётся в парсер, не декомпилировано до конца — `sub_103F2150` получает `v36`/`v35` как параметры, не сам текстовый литерал напрямую, значит применение блока происходит МЕЖДУ строкой 0x103f2004 (xref) и вызовом sub_103F2150, в непоказанном фрагменте, ИЛИ внутри непрочитанной части sub_103F1C00).
- Полный live-lifecycle, подтверждаемый строковым словарём (но НЕ подтверждённый прямым таймингом/трассировкой): `#empty#` → **при старте поиска** → `searching` → `registering`/`heartbeating` (пока GC не резервировал) → **при получении 9107 с успехом** → `reserved` → **когда клиент готов физически подключаться** → `connect` (это финальное значение, которое проверяют `sub_103DE960`/`sub_104B0360`/аналоги в client.dll как "connect" == true).
- **`sv_mmqueue_reservation_extended_timeout`** (`client.dll`, новая находка, не задокументированная ранее) — реальный ConVar, продлевающий таймаут резервации. Означает, что у retail-клиента ЕСТЬ настраиваемый таймаут ожидания резервации — наш live-тест мог просто исчерпать этот таймаут независимо от gate-проблемы, если бы gate прошёл (не проверено, какой у него default).
- **`LockMmQueue`/`#SFUI_SessionError_LockMmQueue`** (`sub_10482890`, client.dll) — существует отдельная ошибка про "заблокированную" mmqueue (`"system { lock mmqueue }"` в шаблонах "connect"/"reserved" выше). "lock"-подключ, вероятно, action-мьютекс (не даёт двум параллельным операциям гонки за один и тот же game/mmqueue), НЕ связан напрямую с нашей проблемой отсутствия "searching", но объясняет, зачем в шаблонах есть подключ `system/lock`.

### [UNKNOWN]

- Точный CALL SITE, который парсит и МЁРЖИТ блок `"Update {... searching ...}"` в сессию — не найден (только косвенно: единственная известная функция-держатель литерала — хвост `sub_103F1C00`, полный путь применения не прослежен до конца).
- Является ли этот setter триггером САМОГО клика "Play" (синхронно ДО отправки 9101), либо это происходит уже ПОСЛЕ получения первого ответа от GC (например, только после того, как GC подтвердит приём 9101 каким-то другим сигналом) — **направление причинности НЕ установлено**. Это меняет всё: если "searching" ставится ТОЛЬКО после какого-то ответа GC (не 9104 и не 9107, раз оба сообщения имеют одинаковый "бланк" gate), тогда единственный способ пройти gate — сначала найти/отправить ЭТОТ третий недостающий сигнал.
- JobId-гипотеза (раздел 17, п.4) — по-прежнему НЕ проверена в этой сессии; RE в этом раунде не нашла ничего, что явно требовало бы JobId-корреляции для 9104/9107 (оба обработчика читают сообщение по типу, не по JobId), что немного СНИЖАЕТ приоритет этой гипотезы, но не исключает её (GC-SDK job dispatch мог бы делать jobid-матчинг на уровне ВЫШЕ этих конкретных функций, не видно в декомпилированном коде).
- Реальный native entrypoint "Play"→9101 (пункт 3 запроса пользователя) — **НЕ найден и в этом раунде**. Поиск по `GameInterfaceAPI.ConsoleCommand`/Lobby/Party API xref-ам не проводился в этой сессии из-за приоритизации writer-поиска; остаётся открытым пунктом (см. NEXT).
- Зависит ли конкретное значение "reserved"/"connect" от `game_type`/mapgroup — не проверено; по коду `sub_103F49F0`/`sub_103DC4B0` разница по `v10 = *(v9+32)&0xF` (Accept-required matrix) влияет только на выбор режима callback-объекта (`sub_103F7980(v34,1,0)` vs `(v34,2,2)`), не напрямую на строку "reserved" (она пишется независимо от game_type, судя по коду — но САМ вызов записи не найден, так что это тоже не 100%).

### [NEXT MINIMAL CHANGE]

Приоритет по цене/полезности, БЕЗ изменения кода в этой сессии (только рекомендации):
1. **Дешевле всего и наиболее решающе: повторить live-тест с `developer 2` и `con_timestamp 1`**, ГЛАВНОЕ — включить ДО клика Play, чтобы поймать `DevMsg`-строки на всём пути `Play → 9101 → ...`, а не только вокруг нашего ответа. Это может напрямую показать момент перехода в `"searching"` (если он логируется где-то через `DevMsg`/`Msg`) и подтвердить/опровергнуть [UNKNOWN]-пункт про направление причинности.
2. Если #1 недоступен/нежелателен: продолжить RE — найти полный путь применения блока `"Update {... searching ...}"` (дособрать хвост `sub_103F1C00`/окрестности xref `0x103f2004`, точно узнать вызывающую цепочку) и параллельно поискать в `client.dll` xrefs на `GameInterfaceAPI`/`LobbyAPI`/Lobby-Party JS-биндинги в поисках настоящего Play-button entrypoint (пункт 3 запроса пользователя, не выполнен в этом раунде).
3. **Только после #1 или #2 дадут прямой ответ** — если подтвердится, что `"searching"` ставится клиентом ЛОКАЛЬНО и синхронно с отправкой 9101 (т.е. до всякого ответа GC) — тогда единственная гипотеза, объясняющая провал теста #1, это ТАЙМИНГ/раса или что-то СБРАСЫВАЕТ `game/mmqueue` между нашим 9101-parse и 9107-send (маловероятно за миллисекунды, но не исключено — не проверялось). Если подтвердится обратное (searching ставится только ПОСЛЕ ответа GC на что-то, что мы не шлём) — тогда нужен третий, ещё не идентифицированный сигнал ПЕРЕД 9107.
4. Никаких изменений `game/mmqueue` из нашего кода не делать — как и указано пользователем, это чисто клиентское локальное состояние, не канал передачи backend/dedicated-server адреса.

---

## 20. Live Test #2: runtime game/mmqueue lifecycle

Тест проведён с `developer 2`/`con_timestamp 1` (через `cfg/autoexec.cfg`, добавлен только на тестовой машине, не в репозитории). Новый `console.log` (`C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\csgo\console.log`), граница нового запуска — `ChangeGameUIState: CSGO_GAME_UI_STATE_INVALID -> CSGO_GAME_UI_STATE_MAINMENU` на строке 3919 (до этого — старый контент, включая Live Test #1, НЕ анализировался повторно). Код НЕ менялся с Live Test #1. Git не трогался.

### ⚠️ ПЕРЕСМОТР вывода Live Test #1 (раздел 18)
Раздел 18 ранее заключил "HIGH CONFIDENCE: гейт `game/mmqueue` НЕ пройден" для Теста #1 — этот вывод **отменяется**. Тест #2 (идентичный код, идентичная процедура) прошёл гейт успешно и получил `DevMsg`-подтверждение. Поскольку в Тесте #1 `developer` был `0` (весь DevMsg-вывод отсутствовал), у нас никогда не было прямого доказательства провала гейта — только совпадение по времени с `MatchmakingStop`, которое, как показал Тест #2, **происходит в любом случае** (успешный путь ТОЖЕ заканчивается `MatchmakingStop`, просто на 21-й секунде, а не мгновенно). Вывод раздела 18 был основан на недостаточных данных; правильная интерпретация теперь: **оба теста, вероятно, прошли гейт одинаково — разница только в видимости DevMsg.**

### [CONFIRMED] (прямо видно в timestamped runtime-логе)

**Полная хронология (все времена 2026-09-17):**
```
23:48:41  [GC] unhandled GetEventFavorites_Request  (сессия ещё не создана)
23:48:46  CreateSession: {game{type classic, mode casual, mapgroupname mg_dust247}, system{network offline}}
23:48:46  Created CMatchSessionOfflineCustom   -- game{} НЕ содержит ключ mmqueue вообще (эквивалент #empty#)
23:48:46  CMatchSessionOfflineCustom::InitializeGameSettings -- снова НЕТ mmqueue
23:48:46  CMatchSessionOfflineCustom::UpdateSessionSettings (x2) -- снова НЕТ mmqueue
23:48:48  [GC] [MM-TEST] MatchmakingStart received: game_type=519 accounts=1 prime_only=1 client_version=13805
23:48:48  [GC] [MM-TEST] Sending stub MatchmakingGC2ClientReserve: map=de_dust2 server=127.0.0.1:27015 reservationid=293a206f6c6c6548
23:48:48  [GC] [MM-TEST] MatchmakingGC2ClientReserve dispatched
23:48:48  CMatchSessionOfflineCustom::UpdateSessionSettings -- ВПЕРВЫЕ mmqueue reserved, map de_dust2, system{lock mmqueue}
23:48:48  Matchmaking reservation confirmed: 293a206f6c6c6548/127.0.0.1:27015     <-- DevMsg из 9107-handler (sub_103F49F0/sub_103DC4B0), branch A (gate ПРОЙДЕН)
23:48:48  Server reservation check 091B6D90 heartbeating                          <-- DevMsg из конструктора CServerConfirmedReservationCheckCallback (sub_103F7980)
   ... 21 секунда тишины на matchmaking-related фронте (обычный игровой/сетевой шум) ...
23:49:09  Server reservation check 091B6D90 will not queue connect                <-- тот же объект 091B6D90, DevMsg из sub_103F7C00 (fallback-ветка)
23:49:09  Destroying CMatchSessionOfflineCustom: {game{mmqueue reserved, map de_dust2}, system{network offline, lock mmqueue}}  -- mmqueue ВСЁ ЕЩЁ "reserved", не менялся всё время
23:49:09  [GC] unhandled MatchmakingStop     <-- клиент шлёт нам 9102
23:49:09  **** Unable to localize '#GenericConfirmText_Label' on panel descendant of 'PopupManager'   -- это и есть визуальный "Failed to connect to the match"
23:49:15  (тот же popup-warning повторно)
23:49:16  Host_Shutdown (пользователь закрыл игру)
```

**9101**: timestamp `23:48:48`. `game_type=519` → decoded `eGame=7` (Casual), `eMapGroup=2` (сопоставлено ЖИВЫМИ данными в этом тесте с `mapgroupname mg_dust247` — **новое подтверждение**: `eMapGroup=2 == "mg_dust247"`). `accounts=1`, `prime_only=1`, `client_version=13805`. Совпадает с реальным Casual/Prime запросом пользователя.

**9107**: timestamp `23:48:48` (та же секунда, что 9101 — наш ответ мгновенный). Отправленные поля (наш стаб, не изменялись): `map=de_dust2`, `server_address=127.0.0.1:27015`, `reservationid=0x293a206f6c6c6548 (GameServerCookieId)`. `serverid`/`direct_udp_ip`/`direct_udp_port`/`reservation` (nested) — не логировались нашим кодом отдельно, но по коду `OnMatchmakingStart` (раздел патча Live Test #1): `serverid=1`, `direct_udp_ip/port` = 127.0.0.1:27015, `reservation` (nested `MatchmakingGC2ServerReserve`) — **НЕ заполнялось** (подтверждено кодом, не изменялось).

**Gate `game/mmqueue`**: на момент прихода 9107 (23:48:48) значение было **НЕ пустым** (гейт `sub_103F49F0`/`sub_103DC4B0` пройден — прямое доказательство: `DevMsg("Matchmaking reservation confirmed...")` реально напечатан). Непосредственно ДО 9101 (23:48:46-47) ключ `mmqueue` **отсутствовал вообще** в трёх последовательных дампах `UpdateSessionSettings`/`InitializeGameSettings`. Значит: **где-то в промежутке между 23:48:46 (создание сессии) и 23:48:48 (приход 9101 к нашему GC) `game/mmqueue` перешло из отсутствующего в непустое состояние** — это временное окно совпадает ТОЧНО с моментом, когда реальный клиент готовил и отправлял 9101. Прямого дампа со значением `"searching"` в этом окне НЕТ (следующий дамп сессии происходит только ПОСЛЕ обработки нашего 9107, где уже видно `"reserved"`) — то есть промежуточное значение `"searching"` не зафиксировано напрямую, но логически это единственное окно, где оно могло появиться.

**`sub_103DF430`/`sub_103F7C00` (KV listener)**: **выполнился МИНИМУМ дважды** — один раз в момент создания объекта (`091B6D90`, "heartbeating" @ 23:48:48, это лог из конструктора `sub_103F7980`, а не из самого листенера) и один раз позже (`091B6D90`, "will not queue connect" @ 23:49:09, это лог ИЗ листенера `sub_103F7C00`, ветка fallback). Значение `game/mmqueue` было "reserved" в ОБА конца этого окна (23:48:48 и 23:49:09, подтверждено дампами) — то есть **значение НЕ изменилось**, но листенер тем не менее вызвался повторно и не прошёл внутреннюю проверку `(vtbl4(a2))==4` (см. HIGH CONFIDENCE ниже).

**`QueueConnect` НЕ вызван** — DevMsg `"Server reservation check %p queue connect\n"` (отдельная от "will not queue connect" строка) НИГДЕ не встречается в логе.

**`popup_accept_match_found`/`popup_accept_match_confirmed` НЕ вызваны** — game_type=7 (Casual) не входит ни в одну известную Accept-матрицу, ожидаемо.

**`MatchmakingStop` от клиента** — ровно ОДИН раз в этом диапазоне, `23:49:09`, **той же секундой**, что и "will not queue connect" и "Destroying CMatchSessionOfflineCustom" — прямая, однозначная временная (и, с учётом структуры кода раздела 18/19, причинная) связь с провалом "queue connect".

**Итоговая визуальная ошибка** — не литеральная строка "Failed to connect to the match" в логе, а движковый warning `"Unable to localize '#GenericConfirmText_Label'"` в ТОЧНО ТУ ЖЕ секунду (23:49:09) — это и есть генерация того самого попапа об ошибке (сам текст ошибки — локализованная строка, показанная через универсальный `GenericConfirmText`-попап).

### [HIGH CONFIDENCE] (сильно подтверждено совокупностью, но не 100% прямым доказательством)

- **Причина "Failed to connect" — НЕ проблема с содержимым 9107 и НЕ проблема с `game/mmqueue`-гейтом.** Весь путь `9101→9107→gate→reservation confirmed→game/mmqueue=reserved→listener` отработал ПОЛНОСТЬЮ УСПЕШНО. Провал происходит на 21-й секунде ПОСЛЕ этого, когда клиент решает "will not queue connect" НЕСМОТРЯ на то, что `mmqueue` всё ещё "reserved". Наиболее вероятное объяснение: клиент инициировал сетевой уровень проверки резервации (через `INETSUPPORT_003`-интерфейс, зарегистрированный в конструкторе `sub_103F7980`, вероятно эквивалент legacy `CBaseClientState::ReserveServer`/`A2S_GETCHALLENGE`, раздел 3) к адресу `127.0.0.1:27015`, там ничего не слушает (это наш ЗАВЕДОМО фиктивный стаб-адрес), и после ~20 секунд сетевой попытки/ретраев клиент сдаётся. **Прямого лог-сообщения о самой сетевой попытке (challenge/connecting) НЕ найдено** — это единственное, что мешает поднять статус до CONFIRMED.
- Внутри `sub_103F7C00` (полностью декомпилирован ранее) "will not queue connect" печатается, когда пройден внешний корреляционный чек (`a2 == this+128`, т.е. это релевантное для НАШЕГО объекта KV-изменение) и `game/mmqueue` НЕ пуст, но `(vtbl4(a2))==4` **не выполнено** — то есть КОНКРЕТНОЕ условие на характер изменения (какая именно вложенная структура/тип у `a2`) не совпало. Это единственная точка кода, которая объясняет наблюдаемое (mmqueue не менялся, но вызов всё равно ушёл в fallback).
- `sub_103F7170` (heartbeat-тикер) в данном случае, вероятно, НЕ является источником 21-секундного таймаута: он содержит ветку с константой 45000мс (0xAFC8), что не совпадает с наблюдаемыми ~21с; кроме того, лог показывает `system{lock mmqueue}` присутствующим на всём протяжении, что (по декомпилированной логике `sub_103F7170`) заставляет функцию вернуться РАНЬШЕ любой из веток таймаута. Более вероятный кандидат — необследованная в этой сессии `sub_103F5E40()` (вызывается из другой ветки того же тикера, специфично для состояния `heartbeating`), которая по названию соседних функций и общему контексту похожа на "проверить статус сетевого resolve/challenge" — **не декомпилирована**, точный механизм таймаута остаётся неподтверждённым.
- `CMatchSessionOfflineCustom` (не `CMatchSessionOnlineClient`) — используется для ЭТОГО Casual-запроса от начала (`23:48:46`, ДО всякого контакта с GC) до конца. `system/network=offline` держится неизменным всю сессию. Это, вероятно, ШТАТНОЕ поведение (эта сессия — локальный "staging"-объект для одиночного игрока при поиске матча, судя по составу `members{numMachines/numPlayers/numSlots=1, machine0{player0{...}}}"`), а не признак поломки, но это не проверялось напрямую (не с чем сравнить — ни одного успешного end-to-end retail-теста с реальным сервером в этой сессии не было).

### [UNKNOWN]

- Точный механизм/функция, вызывающая 21-секундный timeout ("will not queue connect") — конкретно `sub_103F5E40()` не декомпилирован.
- Была ли реально предпринята сетевая попытка (`A2S_GETCHALLENGE`/`ReserveServer`) к `127.0.0.1:27015` — не подтверждено ни одной строкой лога.
- Точное промежуточное значение `game/mmqueue` между "не существует" (23:48:46) и "reserved" (23:48:48) — было ли оно `"searching"` буквально, или переход был прямым в "reserved" без промежуточного видимого шага — не зафиксировано напрямую (интервал слишком короткий и не покрыт отдельным `UpdateSessionSettings`-дампом).
- JobId нашего 9107 — код по-прежнему отправляет `JobIdInvalid` (не менялось). Признаков того, что retail-flow ожидает корреляции по JobId, **не найдено** ни в runtime (это никак не диагностируется по `console.log`, GCMessageRead::JobId() никогда не логировался), ни в декомпиляции (обработчики `sub_103F49F0`/`sub_103DC4B0`/`sub_103F1C00`/`sub_103D9900` не читают JobId входящего сообщения — только тип и protobuf-payload). Гипотеза JobId-корреляции теперь имеет НИЗКИЙ приоритет: успешное прохождение гейта и `"Matchmaking reservation confirmed"` в Тесте #2 произошло БЕЗ JobId, что является веским (хотя не окончательным) аргументом, что JobId не требуется для этого пути.
- Значение `abandon`/причина в клиентском `MatchmakingStop` (23:49:09) — не видно из `console.log` (нужен byte-level дамп протобуфа, не логируется текстом).
- Оффлайн-природа сессии (`CMatchSessionOfflineCustom`, `network offline`) — является ли это нормой ДЛЯ ЛЮБОГО клика Play (в том числе на настоящих серверах Valve) или артефактом нашего окружения/фейкового GC — не установлено.

### [NEXT MINIMAL CHANGE]

Приоритет, без изменения кода в этой сессии:
1. **Самое дешёвое и решающее**: запустить ЛЮБОЙ UDP-респондер на `127.0.0.1:27015` (даже примитивный, отвечающий на `A2S_GETCHALLENGE`/echo) и повторить тест — если клиент после этого проходит "will not queue connect" и печатает "queue connect"/показывает реальный connect screen, это ПОДТВЕРЖДАЕТ HIGH CONFIDENCE гипотезу выше и превращает её в CONFIRMED. Это НЕ требует изменений в нашем `csgo_gc.dll`, только внешний stub-процесс.
2. Если #1 недоступен: декомпилировать `sub_103F5E40()` и метод, стоящий за `vtbl4(a2)` (нужно найти класс `a2`, вероятно `CKeyValuesChangeNotifier`-подобный, и его метод `+4`) — уточнит точный триггер таймаута без необходимости в живом тесте.
3. Параллельно (не блокирует #1/#2): исследовать `CMatchSessionOfflineCustom` природу — сравнить, будет ли настоящий (не тестовый) Casual-поиск с реальным GC тоже создавать `CMatchSessionOfflineCustom`, или это специфично для локальной/некорректно настроенной Steam-сессии.

---

## 21. Полная декомпиляция цепочки `heartbeating` → `will not queue connect` — `client.dll`

Задача: пройти ВЕСЬ call chain от создания `CServerConfirmedReservationCheckCallback` до `"will not queue connect"`, не предполагая заранее причину. Код/DLL/git не менялись, новый live-тест не запускался — только RE поверх артефактов Live Test #2.

### ⚠️ ГЛАВНАЯ ПОПРАВКА к разделу 20
Раздел 20 предполагал (HIGH CONFIDENCE), что `sub_103F7C00` — это **KV-change-listener**, и что таймаут связан с `sub_103F7170`'s 45-секундной веткой. **Оба этих предположения неверны**, что и требовалось доказать декомпиляцией:
- `sub_103F7170` (проверялась строкой `system/lock`) в течение ВСЕГО теста **не могла ничего сделать**, потому что `system/lock` держал значение `"mmqueue"` (непусто) на всём протяжении (подтверждено дампами в 23:48:48 И 23:49:09) — а функция требует `system/lock` быть ПУСТЫМ (`if (!*result)`), чтобы вообще войти в свою heartbeat/timeout-логику. Она была **no-op** весь тест.
- `sub_103F5E40` (проверено полной декомпиляцией) сравнивает elapsed-время с константой `0x1D4C0 = 120000мс = 120 секунд` — это в 5.7 раза больше наблюдаемых ~21с. Даже если бы функция вызывалась, порог не совпадает.
- `sub_103F7A80` (первый вызов в конструкторе `sub_103F7980`) оказался ПРОСТЫМ КОПИРОВАНИЕМ ПОЛЕЙ резервации (map/server_address/reservationid и т.д.) из объекта `a2`=результат работы 9107-хендлера в новосозданный `this` (аналог `sub_103DFBE0`/`sub_10902BC0`-семейства из раздела 18) — НЕ регистрацией KV-listener'а.

### [CONFIRMED] — полная цепочка вызовов, RVA и RTTI

```
sub_103F49F0 (9107-handler, ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve)
  @ 0x103F49F0
    ↓ sub_103F83B0(v34, v23)   -- копирует поля резервации в локальный буфер v34
    ↓ sub_103F7980(v34, mode, mode)   @ 0x103F7980  -- КОНСТРУКТОР CServerConfirmedReservationCheckCallback
        RTTI: '.?AVCServerConfirmedReservationCheckCallback@@' @ 0x10de3114
        │
        ├─ sub_103F7A80(a2=v34)  @ 0x103F7A80  -- копирует map/server_address/reservationid из v34 В this
        ├─ sub_10958860(0) / sub_10958870(0)   -- НЕ декомпилировано (см. UNKNOWN)
        ├─ *(this+32) = 0   (byte offset 128 -- обнулён ПЕРЕД сетевой регистрацией)
        ├─ sub_103F86A0(...)                    -- НЕ декомпилировано, читает доп. поле
        ├─ DevMsg("Server reservation check %p heartbeating\n", this)   <-- ЭТА строка печатается ЗДЕСЬ, один раз, при конструировании
        ├─ sub_103F8500(this+2)   @ 0x103F8500  -- пишет "cfg/qmmconnect.dt" (reconnect-файл на диск, USRLOCAL), НЕ связано с сетевым таймаутом
        └─ v7 = CreateInterface("INETSUPPORT_003")   -- строка @ 0x10b77968
           (*(vtbl+60))(v7, this+24, *(this+10), *(this+11), a3, this, this+32)
              -- РЕГИСТРАЦИЯ сетевого callback'а:
              --   this+24(byte96)=3            -- некий тип/режим запроса
              --   *(this+10)/*(this+11) (byte40/44) = reservationid (64-бит)
              --   a3                            -- accept-mode flag (0 или 2)
              --   this                          -- callback-объект (для будущих вызовов listener'а)
              --   this+32(byte128)              -- OUT-параметр: сюда INETSUPPORT_003 запишет "network request handle"
```

**`this+128` — это НЕ идентификатор KV-поддерева, это NETWORK REQUEST HANDLE, присваиваемый интерфейсом `INETSUPPORT_003`** (обнулён непосредственно перед регистрацией, передан по адресу как out-параметр в тот же вызов, что регистрирует сетевой callback). Это меняет интерпретацию всей функции `sub_103F7C00`.

### `sub_103F7C00` — переосмысленная роль: NETWORK EVENT CALLBACK, не KV-listener

```c
void __thiscall sub_103F7C00(int this, int a2)
{
  if (!a2 || a2 != *(this+128)) return;          // <-- a2 должен совпасть с НАШИМ network-handle
  v3 = (vtbl+8 на a2)();                          // читает 64-бит прогресс/счётчик (v4=младший байт, v30=байт1)
  if (!(vtbl+4 на a2)(a2, HIDWORD(v3)>>8)) {      // ПЕРВЫЙ статус-чек сетевого события
      if (this+140==2) { ...обновление UI прогресс-бара через sub_103F0C70... }
      return;                                      // <-- тихий return, БЕЗ DevMsg
  }
  *(this+136) = 1;
  if (менеджер сессии существует) {
      if (game/mmqueue непусто) {
          if ((vtbl+4 на a2)(a2) == 4) {           // <-- ВТОРОЙ статус-чек: КОНКРЕТНЫЙ код/тип события == 4
              // ready-up ЛИБО queue-connect (полный QueueConnect KV, раздел 19)
              return;
          }
      }
  }
  DevMsg("Server reservation check %p will not queue connect\n", this);   // <-- сюда попадаем, если a2's статус != 4
  ...
}
```

**Точное условие `will not queue connect`** (доказано декомпиляцией, не предположением):
```
will not queue connect  ⟺  ПРОЙДЕНЫ:
    (a2 != NULL) AND (a2 == this+128 [наш network-handle]) AND (vtbl+4(a2, progress) вернул true)
НО НЕ ПРОЙДЕНО:
    (game/mmqueue непусто [ВСЕГДА было true в Тесте #2]) AND (vtbl+4(a2) == 4)
```
Так как `game/mmqueue` подтверждённо оставался `"reserved"` в 23:48:48 И в 23:49:09 (раздел 20), единственная переменная, которая могла измениться — это **возвращаемое значение `(vtbl+4 на a2)(a2)`** между разными вызовами `sub_103F7C00` (функция вызывается КАЖДЫЙ РАЗ, когда сетевой уровень генерирует событие для нашего `this+128`-handle, а не по таймеру). Значение `4`, по всей видимости, — код "успешно получен ответ от резервированного сервера"; ЛЮБОЕ другое значение (включая вероятный код "нет ответа"/"таймаут решения") ведёт к fallback.

### [HIGH CONFIDENCE]

- Наблюдаемые ~21 секунды между "heartbeating" (при конструировании, 23:48:48) и "will not queue connect" (23:49:09) — это **не polling-таймер клиентского кода** (оба найденных таймера — 45с в `sub_103F7170` и 120с в `sub_103F5E40` — не совпадают по порогу И оба физически не могли исполниться из-за `system/lock`). Это, вероятно, **сетевой таймаут ВНУТРИ реализации `INETSUPPORT_003`** (интерфейс живёт в `engine.dll`, не декомпилирован в этой сессии — интерфейс лишь ИМПОРТИРУЕТСЯ в `client.dll` через `CreateInterface`), который сам генерирует финальное "событие-неудачу" через ~21с после безуспешных попыток связаться с адресом резервации (`127.0.0.1:27015`), что и есть `a2`, приходящий в `sub_103F7C00` с "неуспешным" статусом (`vtbl+4(a2) != 4`).
- Это делает гипотезу "нет сервера на 127.0.0.1:27015" **разумной и наиболее вероятной**, но теперь на основании реальной архитектуры (сетевой callback, а не голое совпадение по времени) — однако она остаётся **не подтверждённой напрямую**, так как сам механизм таймаута/ретраев находится в `engine.dll`, вне зоны декомпиляции этой сессии.
- Числовое значение "4" в `vtbl+4(a2)==4` — по контексту (единственный код, ведущий к успеху, среди диапазона статус-кодов сетевого запроса) весьма вероятно соответствует "успешный ответ"/"reservation confirmed by server", но буквальное имя константы не найдено (нет строк-имён enum рядом).

### [UNKNOWN]

- Точная реализация `INETSUPPORT_003` (интерфейс, не класс в client.dll) — находится в `engine.dll`, не проверялась в этой сессии.
- Что делают `sub_10958860(0)`/`sub_10958870(0)` (вызваны в конструкторе перед регистрацией) — не декомпилированы; в имени/окрестности нет явных зацепок.
- Что делает `sub_103F86A0` (читает данные в `v9`/копирует в `this+96..104` как OWORD) — не декомпилирован полностью.
- Буквальное числовое значение/enum-имя кода "4" и то, какие ДРУГИЕ значения возможны (timeout/refused/pending/etc.) — не найдено.
- Действительно ли `INETSUPPORT_003` посылает реальный UDP-пакет на `127.0.0.1:27015`, и через сколько попыток/с каким интервалом — не подтверждено (требует либо декомпиляции `engine.dll`, либо сетевого захвата трафика, либо контролируемого теста с UDP-респондером).
- Означает ли "will not queue connect" ЕДИНСТВЕННЫЙ вызов `sub_103F7C00` за весь тест, или он вызывался несколько раз (например, для промежуточных retry-обновлений прогресс-бара через ПЕРВЫЙ, "тихий" gate) — лог не позволяет отличить (тихие `return` не оставляют следов).

### EXACT FUNCTION CHAIN
```
sub_103F49F0 (9107 handler)
  → sub_103F7980 (CServerConfirmedReservationCheckCallback constructor)
      → sub_103F7A80 (copy reservation fields into `this`)
      → DevMsg("...heartbeating...")
      → sub_103F8500 (write cfg/qmmconnect.dt, unrelated to network timeout)
      → CreateInterface("INETSUPPORT_003") → vtbl+60 (register network callback, this+128 = out-handle)
          ↓ [~21s later, INETSUPPORT_003-internal, engine.dll, NOT decompiled]
sub_103F7C00 (network event callback, re-entered with a2 = event object)
  → gate1: a2 == this+128                      [PASSED, иначе не было бы DevMsg]
  → gate2: vtbl+4(a2, progress) truthy          [PASSED]
  → gate3: game/mmqueue непусто                 [PASSED, подтверждено дампом]
  → gate4: vtbl+4(a2) == 4                      [НЕ ПРОЙДЕНО -- корень проблемы]
  → DevMsg("...will not queue connect...")
  → popup MMFailedTitle/NoOngoingMatch → MatchmakingStop → Destroying session
```

---

## 22. INETSUPPORT_003 reservation check / engine.dll

RE проведено ТОЛЬКО read-only: `engine.dll` скопирован в scratchpad, проанализирован (`idapro.open_database(path, True)`, автоанализ сохранён ТОЛЬКО в scratchpad-копию `.i64`, не в оригинал файла игры). Исходники/DLL проекта и `git` НЕ трогались, новый live-тест НЕ запускался.

### [CONFIRMED] — точный RVA-chain и классы (RTTI)

**Поправка по единицам измерения**: в клиентском вызове `(*(vtbl)(*v7 + 60))(...)` число `60` в псевдокоде Hex-Rays — ДЕСЯТИЧНОЕ (60 = `0x3C`), а НЕ `0x60`. Это меняет номер слота с 24 на **15**. Буквальный `vtbl+0x60` (байт-смещение 0x60, слот 24) — ДРУГОЙ метод (найден отдельно, см. UNKNOWN).

```
CreateInterface("INETSUPPORT_003")                                    [строка @ 0x104abc50]
  ↓ InterfaceReg-таблица: имя@0x1059a9bc, next-указатель@0x1059a9b8=sub_1024B800
  ↓ sub_1024B800 (InstantiateInterfaceFn)                              @ 0x1024B800
      return off_1059A9C4;   -- статический синглтон-объект
  ↓ off_1059A9C4 → CNetSupportImpl::vftable                            @ 0x104abbc8
      RTTI: класс CNetSupportImpl (25 слотов, 0x00-0x60, далее данные)
  ↓ vtable СЛОТ 15 (+0x3C байт = ДЕСЯТИЧНОЕ 60 -- именно это вызывает client.dll)
      = sub_1024BC90                                                   @ 0x1024BC90
        -- тривиальный форвардер: return sub_10098300(a1,a2,a3,a4,a5,a6);
  ↓ sub_10098300 (реальная логика регистрации)                        @ 0x10098300
      -- проверяет -noip, инициализирует сеть (sub_10251DF0 один раз)
      -- v10 = sub_1008A350(this, a6=client_callback_ptr, a2=client_this+24,
                              *(NetSupportThis+36), a3=reservationid_lo,
                              a4=reservationid_hi, a5=accept_mode)
      -- *a7 = v10;         <-- ЭТО и есть запись "this+128 = handle" со стороны client.dll!
      -- sub_10023EF0(*(this+231), &a7)  -- регистрирует новый объект в списке pending-запросов
  ↓ sub_1008A350 (аллоцирует 112 байт через g_pMemAlloc)                @ 0x1008A350
      *this = &CServerMsg_CheckReservation::`vftable';                 <-- RTTI класс найден
      *(this+24) = a6  (указатель ОБРАТНО на client-side callback-объект!)
      *(this+25) = a7
      *(this+26) = a8
      ↓ sub_1008A2A0 (базовый конструктор CServerMsg)                  @ 0x1008A2A0
          *this = &CServerMsg::`vftable';   -- базовый класс всех OOB-серверных сообщений
          ... a7=0x3FF0000000000000 (double 1.0) записывается в объект -- ТАЙМАУТ 1.0 СЕКУНДА НА ПОПЫТКУ
```

**Класс, реализующий `INETSUPPORT_003` — `CNetSupportImpl`.** RTTI `??_7CNetSupportImpl@@6B@` подтверждена напрямую в бинарнике.

**"Network handle" (`this+128` со стороны client.dll) — указатель на СВЕЖЕСОЗДАННЫЙ C++-объект класса `CServerMsg_CheckReservation`** (RTTI `.?AVCServerMsg_CheckReservation@@` подтверждена), НЕ число/enum и НЕ идентификатор KV. Базовый класс — `CServerMsg` (RTTI подтверждена), часть семейства однотипных OOB-сообщений (найден RTTI-сосед `CServerMsg_Ping` — классическая Source-engine connectionless-схема, `A2S_*`-подобная, но `CheckReservation` — кастомное расширение под GC-матчмейкинг).

**Также найден `CAsyncOperation_ReserveServer` (вложенный класс `CBaseClientState`)** — RTTI `.?AVCAsyncOperation_ReserveServer@CBaseClientState@@` подтверждена, vtable @ `0x1046371C`. Прямая связь с legacy-пайплайном `CBaseClientState::ReserveServer` (раздел 3, UDP OOB `A2S_GETCHALLENGE`-подобный) — `CServerMsg_CheckReservation`, судя по всему, часть той же семьи механизмов.

**Протокол пакета** (декомпилирован `sub_1008A4F0`, метод построения+отправки запроса, слот 6 у `CServerMsg_CheckReservation`):
```
sub_102EA700(-1, 32)              -- запись 0xFFFFFFFF (стандартный connectionless OOB-маркер Source engine)
sub_100C1120(33, 8, ...)          -- запись opcode = 33 (0x21) -- КАСТОМНЫЙ тип OOB-сообщения (не A2S_GETCHALLENGE=0x57/A2S_INFO)
sub_102EA700(dword_138EF1C4, 32)  -- запись 32-битного протокольного/версионного поля
sub_102EA700(a4, 32)              -- запись доп. 32-битного поля запроса
sub_102EA700(*(this+26), 32)      -- запись поля из объекта (вероятно часть reservationid/challenge)
sub_100C1120(*(this+24), 32,...)  -- запись поля (this+24=client-callback-ptr -- требует уточнения, что сериализуется)
sub_100C1120(*(this+25), 32,...)
sub_100C1120(<64-бит через CreateInterface>, 32,...) x2  -- вероятно локальный SteamID/timestamp
if (*(a2+28) != 2) → sub_10250370(buffer, size, 0)   -- ОТПРАВКА (a2=netadr_t-подобный адрес назначения; +28=тип/классификация адреса)
else { доп. проверка sub_102515F0(1); в итоге тоже sub_10250370(...) }
```

**Найдены ТОЧНЫЕ, буквальные строки причин неудачи** (декомпилирован `sub_1008A3E0`, "OnFailure/OnComplete", слот 5):
```c
if (reason > 0) {
    if (reason <= 2) "User canceled matchmaking"
    else if (reason == 3) {
        if (this+80 [64-бит] != 0) "Matchmaking failed; Waiting on %d/%d clients"
        else "Matchmaking failed.  We never heard from gameserver"   <-- ТОЧНОЕ совпадение с нашим сценарием
    }
}
```
Строка **`"Matchmaking failed.  We never heard from gameserver"`** — буквальный, явно закодированный Valve путь для случая "сервер из резервации не ответил". Это не гипотеза — это то, что разработчики сами предусмотрели как ожидаемый исход.

### [HIGH CONFIDENCE]

- Наблюдаемые ~21с в Live Test #2, вероятнее всего — СУММА нескольких попыток по **1.0 секунде** (константа найдена буквально в `sub_1008A2A0`), т.е. ~20 повторных OOB-запросов с интервалом ~1с, после чего фиксируется "never heard from gameserver". Точный retry-limit (вероятно поле `this+26`) не декомпилирован до конца.
- Целевой адрес (`a2` в `sub_1008A4F0`) архитектурно ДОЛЖЕН происходить из `direct_udp_ip`/`direct_udp_port`/`server_address` нашего 9107 (единственные адресные поля в системе к этому моменту), но прямой dataflow через ВСЕ промежуточные слои не прослежен пошагово.
- `a2`/`vtbl+4(a2)` в client.dll's `sub_103F7C00`, вероятно — вызов ВИРТУАЛЬНОГО МЕТОДА самого объекта `CServerMsg_CheckReservation`/`CServerMsg` (не чтение сырого поля структуры) — то есть "статус" получается через полиморфный API самого OOB-объекта.

### [UNKNOWN]

- Буквальное имя enum-константы "4" — не найдено нигде как именованный символ; вывод остаётся структурным (единственный путь к успеху), не текстуально подтверждённым.
- Точный retry-limit и hard-timeout OOB-запроса — не декомпилирован (нужно происхождение `a8`-параметра `sub_1008A350`).
- Прямой dataflow `direct_udp_ip/direct_udp_port` → аргумент `sub_1008A4F0`'s `a2` — не прослежен пошагово.
- `sub_10098070`/`sub_100A5040`/`sub_1005DD00`/`sub_100A5050`/`sub_100A50C0` (методы `CAsyncOperation_ReserveServer`) — не декомпилированы, могут содержать более прямую связь с адресом.
- `vtbl+0x10` (слот 4) У ОБОИХ `CNetSupportImpl` и `CAsyncOperation_ReserveServer` = `@_guard_check_icall_nop` (CFG-заглушка) — метод не переопределён ни в одном из двух классов в этой сборке; назначение базового метода не установлено.
- `vtbl+0x60` буквально (слот 24, `CNetSupportImpl`, `sub_1024BDE0`) — НЕ тот метод, что вызывает client.dll (там байт-смещение 0x3C=слот15). Сам `sub_1024BDE0` — функция классификации/резолва адреса (loopback/localhost-проверки), вероятно используется где-то в этой же системе (похоже на то же поле `*(a2+28)` из `sub_1008A4F0`), но явная xref-связь не подтверждена.
- Формат ожидаемого ОТВЕТА сервера на opcode `33` — функция, парсящая входящий ответ, не найдена/не декомпилирована в этой сессии.

### ROOT CAUSE / PROVEN / NOT PROVEN — сводка §22

```
ROOT CAUSE:
Клиент отправляет кастомный connectionless OOB UDP-запрос (opcode 33/0x21) на адрес из
MatchmakingGC2ClientReserve (архитектурно; dataflow не прослежен на 100% пошагово) с
интервалом повтора ~1.0с (константа подтверждена в коде). Наш стаб-адрес 127.0.0.1:27015
ничего не отвечает, поэтому после ~N попыток (~21с) фиксируется явный, штатный код Valve:
"Matchmaking failed.  We never heard from gameserver" -- это ЗАЛОЖЕННОЕ, а не аварийное поведение.

PROVEN:
Полная call chain от INETSUPPORT_003 до аллокации CServerMsg_CheckReservation (с RTTI на
каждом уровне); константа таймаута 1.0с/попытка; opcode пакета 33; связь с legacy
CBaseClientState::ReserveServer/CAsyncOperation_ReserveServer; буквальная строка "never
heard from gameserver" как явно закодированный failure-путь.

NOT PROVEN:
Буквальное значение enum "4"; точный retry-limit; пошаговый dataflow IP/порта из 9107 до
фактического пакета; формат ожидаемого ответа сервера; поведение CAsyncOperation_ReserveServer.

EXACT ENGINE CHAIN:
CreateInterface("INETSUPPORT_003") → CNetSupportImpl (off_1059A9C4) →
vtbl+0x3C(слот15)=sub_1024BC90 → sub_10098300 → sub_1008A350 (new CServerMsg_CheckReservation,
timeout=1.0s) → sub_1008A4F0 (build+send OOB opcode 33) → [~21s безответных ретраев] →
sub_1008A3E0 ("We never heard from gameserver") → sub_103F7C00 (client.dll, gate4 fails)

STATUS FIELD:
a2+4 в client.dll -- не сырое поле, вероятно вызов virtual-метода объекта CServerMsg-иерархии
(не подтверждено буквально)

STATUS 4:
Не найдено как именованная константа; структурно -- единственный путь к "queue connect"

NETWORK PROTOCOL:
Connectionless OOB UDP (маркер 0xFFFFFFFF + opcode 33/0x21), кастомный, НЕ стандартный A2S_*

NEXT MINIMAL TEST:
Декомпилировать sub_10098070/sub_100A50C0 (CAsyncOperation_ReserveServer) для подтверждения
адреса; либо поднять минимальный UDP-эхо на 127.0.0.1:27015, отвечающий ЛЮБЫМИ байтами на
входящий connectionless-пакет, и проверить, меняется ли исход (без изменения нашего DLL).
```

**Does a real server need to answer this request? YES (HIGH CONFIDENCE, не 100% CONFIRMED)** — явно закодированный путь `"We never heard from gameserver"` доказывает, что клиент архитектурно ждёт ответ ИМЕННО от адреса резервации; не доказано, что нужен полноценный `srcds` — кастомный opcode 33 предполагает МИНИМАЛЬНЫЙ протокольный ответ, но точный ожидаемый формат не декомпилирован.

---

## 23. ReserveServer / opcode 33 request-response protocol — SUCCESS PATH НАЙДЕН

RE строго read-only, продолжение §22 в том же `engine.dll` (scratchpad-копия). Git/исходники/DLL проекта не менялись, новый live-тест не запускался.

### [BINARY CONFIRMED] — receive-path и точная success-транзакция

**Главный диспетчер входящих connectionless-пакетов — `sub_1008E7C0`** (`engine.dll`, 8437 байт, содержит строку `"Bad connectionless packet ( CL '%c') from %s.\n"` в `default:`-ветке). Это `switch` по декодированному из битового потока байту-опкоду (`v233`). **Полный список обработанных case-значений**: `0, 37, 57, 65, 66, 73, 94, 105, 106, 108, 112, 116`.

**⚠️ КЛЮЧЕВАЯ ПОПРАВКА к §22**: **opcode `33` (0x21) в этом switch ОТСУТСТВУЕТ.** Это означает, что 33 — опкод ЗАПРОСА (client→server, подтверждено в §22 через `sub_1008A4F0`), а СЕРВЕР должен ответить ДРУГИМ опкодом, который клиент способен обработать. Из найденного списка, **case `37` (0x25, ASCII `'%'`) вызывает `sub_1008A610`** — функция, лежащая в том же адресном диапазоне (`0x1008Axxxx`), что и весь код `CServerMsg_CheckReservation` из §22, и ссылающаяся на тот же `dword_138EF1C4` (протокольная константа, использованная при ПОСТРОЕНИИ исходного запроса). **Это и есть response-handler.**

**Полная декомпиляция `sub_1008A610`** (`case 37`) — цепочка проверок и success-транзакция:
```c
int __thiscall sub_1008A610(_DWORD *this, int a2, int a3, int a4, int a5)
{
    if (a4 != dword_138EF1C4) return a4;         // (1) протокольная константа должна совпасть с запросом
    if (GetStatus(this) != 0) return ...;         // (2) this->vtbl[1]() == GetStatus(); должен быть ещё 0 (pending)
    if (*(a2+28) != *(this+11)) return ...;       // (3) reservationid из пакета должен == сохранённому this+11
    // ветвление 1..3 внутри reservationid-совпадения (детали не влияют на success-путь)
    if (a5 != *(this+22)) return ...;             // (4) ВТОРОЙ correlation-токен (this+22) тоже должен совпасть
    // читает из оставшегося битового потока (a3):
    v37 = read_32bit(a3);   // "reservation number" -- используется только для DevMsg-логирования
    v36 = read_8bit(a3);    // "accepted"/awaiting count (0 = ВСЕ подтвердили, 127 = спец.значение, иначе частично)
    v26 = read_8bit(a3);    // "needed"/total count
    if (v36) {
        DevMsg("Server reservation%u is awaiting %d/%d\n", v37, v36, v26);
        if (v36 == 127) { *(this+2) = 3; callback(this); }         // частичный/неизвестный статус
        else { store partial counts; callback(this); }             // ждём ещё игроков
    } else {
        DevMsg("Server confirmed all players reservation%u/%d\n", v37, v26);
        if (GetStatus(this) == 0) {
            *(this+2) = 4;                          // <-- SUCCESS!!! ИМЕННО ЗДЕСЬ
            *(QWORD*)(this+10) = v26 << 8;
            callback(this);                          // вызывает client.dll's sub_103F7C00 через сохранённый указатель
        }
    }
}
```

**`*(this+2) = 4` — БУКВАЛЬНОЕ, ПРЯМОЕ ДОКАЗАТЕЛЬСТВО значения "status 4"**: устанавливается ИМЕННО когда `v36 == 0` ("accepted count" в ответе сервера равен нулю, т.е. **сервер подтвердил, что ВСЕ игроки резервации присутствуют/приняты** — DevMsg буквально называет это `"Server confirmed all players reservation%u/%d"`). Это закрывает главный открытый вопрос §21/§22 — **status 4 = "сервер подтвердил всех игроков резервации"**, не абстрактный "успех сети", а конкретно-протокольное подтверждение от game-сервера.

**Второй похожий обработчик — `sub_1008AB50` (`case 94`, 0x5E)** — структурно идентичен (тот же протокол-чек, тот же `this+11`/`this+22` correlation), но ПРОЩЕ: сразу ставит `*(this+2)=4`, дополнительно считает RTT (`(currentTime - this+12) * 1000.0`, миллисекунды) и сохраняет в `this+10`. Похоже на **ответ типа "Ping"/более простое прямое подтверждение** (класс-сосед `CServerMsg_Ping` из §22) — не обязательно тот же протокол, что `CheckReservation`, но использует ИДЕНТИЧНУЮ архитектуру correlation+success.

### [HIGH CONFIDENCE]

- **Correlation-поля `this+11` и `this+22`** (у `CServerMsg_CheckReservation`) — это, вероятно, **reservationid** (или его часть) и **challenge/nonce**, полученные при ПОСТРОЕНИИ запроса (§22, `sub_1008A2A0`/`sub_1008A350`) и требуемые для ПРИЁМА ответа — то есть сервер ДОЛЖЕН эхом вернуть эти же значения в своём ответе, иначе пакет будет проигнорирован (защита от спуфинга/устаревших ответов).
- Полный ожидаемый **формат успешного ответа (case 37)**, помимо генерик-заголовка (0xFFFFFFFF + opcode `37`), включает как минимум: 32-битное протокольное поле (=`dword_138EF1C4`), 32-битный reservationid-эхо, доп. 32-битный correlation-токен-эхо, 32-битный "reservation number", 1 байт "accepted count" (**должен быть `0`** для успеха), 1 байт "needed count".
- Полный header ДО этих полей (что именно составляет `a2`, `a4`, `a5` в терминах байтовых смещений от начала UDP-payload) НЕ прослежен пошагово с самого начала пакета — известно только, что диспетчер (`sub_1008E7C0`) читает opcode как первый декодированный байт битового потока.

### [NOT PROVEN]

- Точный retry-limit (количество попыток) — не найден как отдельная числовая константа; подтверждён только интервал 1.0с/попытка (§22). ~21с из Live Test #2 остаётся согласующимся, но не арифметически точно равным N×1.0с без точного N.
- Полный побайтовый packet layout ЗАПРОСА (opcode 33) и ОТВЕТА (opcode 37) от самого первого байта UDP payload — реконструирован ЧАСТИЧНО (порядок полей ПОСЛЕ заголовка подтверждён для case 37, но точные битовые смещения/размеры каждого поля от начала пакета — нет).
- Является ли `sub_1008AB50`(case 94) реально альтернативным путём для ТОГО ЖЕ `CServerMsg_CheckReservation`, или это отдельный, несвязанный тип сообщения (`CServerMsg_Ping` или иной) — не подтверждено (нет RTTI-проверки, что `this` в обоих случаях один и тот же класс).
- Точное числовое значение `dword_138EF1C4` (что это за протокольная константа — версия протокола резервации?) — не прочитано явно (не критично для протокола, но полезно для реализации responder'а).

### Ответ на практический вопрос — можно ли реализовать минимальный UDP responder?

**ДА, теоретически возможно на основе найденного протокола**, но с оговорками:
```
REQUEST (client → server, opcode 0x21/33, подтверждено §22):
  0xFFFFFFFF (connectionless marker)
  0x21 (opcode)
  dword_138EF1C4 (протокольная константа, точное числовое значение НЕ прочитано)
  reservationid-поля
  correlation-токен
  [UNKNOWN: точный побайтовый layout до этих полей]

RESPONSE (server → client, opcode 0x25/37, чтобы получить status=4):
  0xFFFFFFFF (connectionless marker)
  0x25 (opcode 37)
  dword_138EF1C4 (ТОЧНО ТА ЖЕ константа, что была в запросе -- responder должен либо знать её,
                   либо эхом отразить то, что видел в запросе)
  reservationid (эхо из запроса, должен совпасть с this+11 клиента)
  correlation-токен (эхо, должен совпасть с this+22 клиента)
  32-бит "reservation number" (для лога, не влияет на успех)
  1 байт "accepted count" = 0x00  <-- ОБЯЗАТЕЛЬНО ноль для success
  1 байт "needed count" (любое значение, используется только для лога/QueueConnect helper_time)

REQUIRED FIELDS: protocol-constant match, reservationid match, correlation-token match, accepted-count==0
EXPECTED STATUS: this+2 = 4 (у CServerMsg_CheckReservation) -> callback -> client.dll sub_103F7C00 -> "queue connect"
```
**Ограничение**: без знания ТОЧНОГО значения `dword_138EF1C4` и точного побайтового layout запроса (откуда взять reservationid/correlation-токен из ВХОДЯЩЕГО от клиента пакета, чтобы их эхом вернуть), написать responder "вслепую" нельзя — но это **вопрос ещё одной сессии RE (декомпилировать начало `sub_1008E7C0` до свитча, и прочитать `dword_138EF1C4`'s значение), не архитектурный тупик.**

---

```
ROOT CAUSE:
Наш стаб-сервер 127.0.0.1:27015 не отвечает на connectionless UDP-запрос opcode 33 (0x21) --
и это ЕДИНСТВЕННАЯ причина. Найден ТОЧНЫЙ success-path: сервер должен ответить opcode 37 (0x25)
с полями, эхом повторяющими протокольную константу/reservationid/correlation-токен клиента,
и byte "accepted count" == 0. Это устанавливает *(this+2)=4 на CServerMsg_CheckReservation,
что и есть тот самый "status 4", запускающий queue connect в client.dll.

PROVEN:
Полный список обработанных connectionless-опкодов (0,37,57,65,66,73,94,105,106,108,112,116);
opcode 33 (запрос) НЕ входит в этот список (значит это чисто исходящий опкод); opcode 37 =
response-handler sub_1008A610 с буквальным *(this+2)=4 при "accepted count"==0, буквальные
DevMsg-строки "Server confirmed all players reservation%u/%d" и "Server reservation%u is
awaiting %d/%d"; correlation по this+11 (reservationid) и this+22 (доп. токен); идентичный
success-паттерн в соседнем sub_1008AB50 (case 94).

NOT PROVEN:
Точный retry-limit; полный побайтовый packet layout от начала UDP payload; числовое значение
dword_138EF1C4; принадлежность case 94 именно к CheckReservation, а не к соседнему CServerMsg_Ping.

REQUEST:
0xFFFFFFFF + opcode 0x21(33) + protocol-const(dword_138EF1C4) + reservationid + token + [UNKNOWN prefix layout]

RESPONSE:
0xFFFFFFFF + opcode 0x25(37) + тот же protocol-const + эхо reservationid + эхо token +
reservation-number(32-бит) + accepted-count(1 байт, ДОЛЖЕН быть 0) + needed-count(1 байт)

SUCCESS CONDITION:
protocol-const совпал AND reservationid совпал AND token совпал AND accepted-count==0
  => *(this+2)=4 => callback => client.dll sub_103F7C00 => "queue connect"

RETRY:
Интервал 1.0с/попытка подтверждён (§22, sub_1008A2A0); точный лимит попыток НЕ найден как
явная константа; ~21с из Live Test #2 согласуется, но не доказан арифметически как N×1.0с.

MINIMAL RESPONDER:
YES (архитектурно, с оговоркой: нужно ещё дореверсить dword_138EF1C4 и точный header-layout
запроса, чтобы знать, ЧТО именно эхом возвращать -- это не тупик, а следующий конкретный шаг RE)

EXACT SUCCESS CHAIN:
UDP response (opcode 37, accepted_count=0)
→ sub_1008E7C0 (connectionless dispatcher, case 37)
→ sub_1008A610 (validates protocol-const + reservationid + token)
→ *(this+2) = 4  [CServerMsg_CheckReservation status]
→ callback(this) → client.dll sub_103F7C00 (a2 = this, vtbl+4(a2) == 4 теперь ИСТИНА)
→ "Server reservation check %p queue connect" → QueueConnect KV → connect
```

---

## 24. Exact reservation opcode 33/37 packet layout

RE строго read-only, продолжение §22/§23 в том же `engine.dll` (scratchpad). Git/исходники/DLL не менялись, live-тест не запускался.

### [BINARY CONFIRMED] — `dword_138EF1C4` (протокольная константа)

**Найден writer — `sub_10282500`**: функция открывает **`steam.inf`**, парсит ключ версии (строка "ClientVersion"-подобная, через `sub_101DECD0`/поиск по `byte_10627808`/`unk_10627815`), **удаляет точки** из строки версии (`if (i != 46) *v8++ = i;` — `46` = ASCII `'.'`), затем **`dword_138EF1C4 = sub_103EFEA1(v9, v28)`** — парсит очищенную от точек строку версии как целое число.

**Итог: `dword_138EF1C4` = числовое представление `ClientVersion` из `steam.inf` без точек** — то есть ТОТ ЖЕ механизм, что версию строит наш собственный `gc_client.cpp`/`BuildMatchmakingHello` (`required_appid_version`). **Кросс-подтверждение** (не предположение): в обоих Live Test #1 и #2 наш собственный `[MM-TEST]`-лог УЖЕ показывал `client_version=13805` — это ТОЧНО тот же паттерн (версия `1.38.0.5` → без точек → `13805`). **С высочайшей уверенностью `dword_138EF1C4` = `13805`** (числовое значение из памяти в статическом дампе `0xFFFFFFFF` — это НЕ реальное runtime-значение, а незаполненный placeholder до вызова `sub_10282500` при инициализации сети; `is_loaded=False` подтверждает это как uninitialized-данные на диске).

**Значение НЕ msg-специфичный "magic"** — используется идентично в 34+ функциях по всему сетевому коду `engine.dll` (не только в `CheckReservation`) — это ГЛОБАЛЬНАЯ константа протокольной версии клиента, проверяемая почти во всех connectionless-сообщениях.

### [BINARY CONFIRMED] — порядок полей в теле ответа opcode 37 (`sub_1008A610`)

Полная декомпиляция (§23) даёт точный ПОРЯДОК и РАЗМЕР полей, прочитанных `sub_1008A610` из битового потока (`a3`), **после** того, как диспетчер (`sub_1008E7C0`, case 37) уже вычленил `a4`/`a5`:
```
a4  (32 бита, читается ПЕРЕД вызовом sub_1008A610, внутри case-37 обвязки диспетчера) = protocol-const, сравнивается с dword_138EF1C4 (=13805)
a5  (32 бита, читается там же следом)                                                = correlation-токен, сравнивается с *(this+22)
--- (внутри sub_1008A610, из оставшегося битового потока a3) ---
v37 (32 бита)  = "reservation number" -- используется ТОЛЬКО для DevMsg-лога, не участвует в success-условии
v36 (8 бит)    = "accepted count" -- 0 = ВСЕ подтверждены (SUCCESS), 127 = спец.значение (partial/unknown), 1-126 = частично
v26 (8 бит)    = "needed count" -- используется только для лога и (при success) сохраняется в this+10
```
**`*(a2+28)` (reservationid, сравнивается с `*(this+11)`)** — читается из ОТДЕЛЬНОГО параметра `a2` внутри `sub_1008A610`, который на call-site — это `v4` (не сырой пакет, а `*(outer_a2+52)`, 16-байтовый OWORD, скопированный диспетчером в самом начале функции, до свитча). Это, вероятно, **уже распарсенная диспетчером структура-обёртка "источник пакета"** (netadr_t-подобная, с доп. полями вроде reservationid, распарсенными РАНЬШЕ свитча, общими для нескольких opcode) — а НЕ поле, специфичное для case 37 в сыром UDP payload. **Точный побайтовый offset этого reservationid-поля от начала UDP-пакета НЕ прослежен** (потребовалась бы декомпиляция самого начала `sub_1008E7C0`, до свитча, что не выполнено в этой сессии из-за размера функции — 8437 байт/300+ переменных).

### [HIGH CONFIDENCE]

- **Полный список известных opcode'ов диспетчера** (§23): `0, 37, 57, 65, 66, 73, 94, 105, 106, 108, 112, 116`. Opcode `33` (запрос) НЕ входит — подтверждает, что 33 чисто исходящий.
- Структура запроса (opcode 33, `sub_1008A4F0`, §23) в том же порядке констант: маркер `0xFFFFFFFF` → opcode `33` (1 байт) → `dword_138EF1C4`=13805 (32 бита) → доп. 32-битное поле запроса (`a4` внутри `sub_1008A4F0`) → `*(this+26)` (32 бита) → `*(this+24)`/`*(this+25)` (по 32 бита каждое) → 64-битное значение через `CreateInterface`-объект (вероятно локальный SteamID или timestamp).
- Структура ответа (opcode 37) в порядке: маркер `0xFFFFFFFF` → opcode `37` → [нераспутанный общий header, включающий reservationid] → protocol-const (32 бита, =13805) → token (32 бита) → reservation-number (32 бита) → accepted-count (1 байт) → needed-count (1 байт).

### [NOT PROVEN]

- Точный побайтовый layout от САМОГО начала UDP payload (offset 0) до момента, когда диспетчер уже выделил opcode/a2/a4/a5 — не декомпилирован (функция `sub_1008E7C0` слишком велика для полной построчной декомпиляции в бюджет этой сессии).
- Значение `*(this+24)`/`*(this+25)`/`*(this+26)` (что именно сериализуется в запросе на этих полях) — установлена только позиция, не точный смысл каждого.
- Endianness полей — Source engine традиционно little-endian (x86), декомпилированный код не даёт оснований предполагать иное, но явного подтверждения через network-byte-order вызовов (`htonl` и т.п.) не найдено — **предполагается little-endian по умолчанию платформы, не подтверждено отдельно**.

### [OFFICIAL SERVER CHECK] — прямой ответ на вопрос §8 пользователя

```
OFFICIAL SERVER CHECK:
NO evidence found in the examined reservation path.

EXACT RVA:
Строка 'is_official_valve_server' существует в бинарнике @ 0x10486d97,
НО имеет 0 (ноль) code/data xrefs -- ничто в текущей сборке engine.dll её не читает.

WHAT IS CHECKED:
Ничего -- строка орфанная (вероятно, остаток отключённой/устаревшей функциональности,
или ConVar/KeyValue-имя, регистрируемое механизмом, не распознанным статическим xref-анализом IDA).

SOURCE OF VALUE:
N/A

CONNECTED TO RESERVATION:
NO -- вся цепочка success-условия (sub_1008A2A0, sub_1008A350, sub_1008A3E0, sub_1008A4F0,
sub_1008A610, sub_1008AB50, sub_10098300 -- ВСЕ полностью декомпилированные функции этой
и предыдущей сессии) не содержит НИ ОДНОГО вызова, связанного со Steam-аутентификацией,
GSLT, ServerSteamID, "official"/"secure"/"insecure" флагами. Единственное условие успеха --
protocol-const match + reservationid match + token match + accepted-count==0 (§23).

EXECUTECOMMAND IN THIS PATH:
NO -- ни ExecuteCommand, ни ClientCmd/ClientCmd_Unrestricted не встречаются ни в одной из
декомпилированных функций reservation-check цепочки (client.dll ИЛИ engine.dll). Механизм
ExecuteCommand/ConsoleCommand (GameInterfaceAPI, раздел 3) -- отдельный, несвязанный
transport для Panorama JS → engine команд, не пересекается с этим бинарным протоколом.

EVIDENCE:
Полная декомпиляция 7 функций success/failure-цепочки (§22/§23/§24), ни одна не читает
SteamID/GSLT/server-flags; отдельный string-scan по 15+ ключевым словам
(official/untrusted/insecure/GSLT/ServerSteamID/SteamGameServer/secure/trusted) нашёл
строки ТОЛЬКО в НЕСВЯЗАННЫХ участках (SteamGameServer login/VAC-ban сообщения на СТОРОНЕ
СЕРВЕРА при запуске dedicated server, не в клиентском reservation-check пути).
```

**No evidence of an official-vs-non-official server check was found in the examined reservation path.**

---

```
PROTOCOL CONSTANT:
dword_138EF1C4 = числовое ClientVersion из steam.inf без точек = 13805 (HIGH CONFIDENCE,
кросс-подтверждено собственными Live Test #1/#2 логами, где client_version=13805)

REQUEST 0x21:
0xFFFFFFFF + opcode(1б)=0x21 + protocol-const(32б)=13805 + доп.поле(32б) +
this+26(32б) + this+24(32б) + this+25(32б) + 64-бит(SteamID/timestamp?) [часть offsets UNKNOWN]

RESPONSE 0x25:
0xFFFFFFFF + opcode(1б)=0x25 + [общий header, вкл. reservationid, offset UNKNOWN] +
protocol-const(32б)=13805 + token(32б) + reservation-number(32б) +
accepted-count(1б, ДОЛЖЕН=0) + needed-count(1б)

CORRELATION:
protocol-const (=13805, точное совпадение) + reservationid (*(a2+28)==*(this+11)) +
token (a5==*(this+22)) -- ВСЕ ТРИ валидируются явными сравнениями, ни одно не является
только логированием.

SERVER-SIDE SENDER:
Не найден в этой сессии -- строки "Server confirmed all players"/"Server reservation...
awaiting" являются CLIENT-SIDE DevMsg (лог получателя ответа), не найдена парная
SERVER-SIDE функция, которая СТРОИТ opcode 37 (это следующий шаг RE, не выполнен).

MINIMAL RESPONDER SPEC:
LISTEN: 127.0.0.1:27015 UDP
REQUEST: [layout выше, offsets частично UNKNOWN]
MATCH: reservationid = эхо из запроса; token = эхо из запроса; protocol = 13805 (известно точно)
RESPONSE: [layout выше, offsets частично UNKNOWN]
accepted_count = 0x00
needed_count = любое (для лога)
СТАТУС: спецификация НЕ полна для написания кода -- не хватает точных byte-offset'ов
заголовка (см. NOT PROVEN) -- код responder'а НЕ писать, как и требовалось.
```

---

## 25. Exact reservation opcode 33/37 packet layout — byte-precise (raw disassembly, не только псевдокод)

RE строго read-only, продолжение §22-24, `engine.dll` (scratchpad). Git/исходники/DLL не менялись, live-тест не запускался. **Впервые в этой серии — анализ на уровне RAW-дизассемблера (не только Hex-Rays псевдокод)**, что даёт байт-точные offset'ы и устраняет двусмысленность более ранних декомпиляций.

### [CONFIRMED] — таблица REQUEST (opcode 0x21), по raw disasm `sub_1008A4F0`

| packet offset | size | field | source | notes |
|---|---|---|---|---|
| 0 | 4 | `0xFFFFFFFF` | constant | connectionless-маркер, стандартный Source engine |
| 4 | 1 | `0x21` (33) | constant | opcode; подтверждено `push 8`(width)+`push 21h`(value) перед `call sub_100C1120` |
| 5 | 4 | protocol constant | `dword_138EF1C4` | = 13805 (см. §24); `push 20h`(width=32)+`push dword_138EF1C4` перед `call sub_102EA700` |
| 9 | 4 | `arg_8` — 4-й параметр метода `Send`/`sub_1008A4F0` | caller-supplied | **семантика UNKNOWN** — caller функции не найден (только vtable/data xref, нет прямого call site) |
| 13 | 4 | `[this+0x68]` (byte 104, dword-idx 26) | поле объекта, установлено конструктором `sub_1008A350` как параметр `a8` | семантика UNKNOWN (была предположена как "retry-limit", не подтверждено) |
| 17 | 4 | `[this+0x60]` (byte 96, dword-idx 24) | поле объекта, = указатель на client-side callback-объект (`a6` конструктора, §22) | вероятно используется как opaque correlation-cookie, а НЕ как значимые данные — сырой указатель в сеть, что типично для локально-генерируемого токена |
| 21 | 4 | `[this+0x64]` (byte 100, dword-idx 25) | поле объекта, = `a7` конструктора | семантика UNKNOWN |
| 25 | 4 | 64-битное значение, младшие 32 бита | через `CreateInterface`-подобный вызов (`dword_10669F64` → vtbl+8) | HIGH CONFIDENCE: локальный SteamID64 (низкая половина) — паттерн типичен для Source engine identity-полей в запросах к серверу |
| 29 | 4 | 64-битное значение, старшие 32 бита | тот же источник | HIGH CONFIDENCE: SteamID64 старшая половина |

**Итого 33 байта payload** (после IP/UDP заголовков). Все поля **выравнены по байтам** (8 и 32 бита, без "рваных" битовых полей) — подтверждено прямыми `push <width>; push <value>; call sub_102EA700/sub_100C1120` парами в raw disasm, а не только псевдокодом.

### [CONFIRMED] — таблица RESPONSE (opcode 0x25), по raw disasm `sub_1008A610`

Параметры самой функции (thiscall, `this`=`edi`=ecx на входе): `arg_0`("a2", wrapper-структура источника пакета), `arg_4`("a3", указатель на bf_read с ОСТАВШИМСЯ потоком после того, как диспетчер `sub_1008E7C0` уже вычленил opcode+protocol-const+token), `arg_8`("a4", protocol-const, УЖЕ прочитан диспетчером ДО вызова), `arg_C`("a5", token, УЖЕ прочитан диспетчером ДО вызова).

| проверка/поле | точный offset | validation | подтверждение |
|---|---|---|---|
| protocol constant | `arg_8` (переданный параметр, не offset в packet напрямую) | `cmp eax, dword_138EF1C4` @ `0x1008a61b`; несовпадение → `jnz loc_1008A9D7` (тихий выход) | raw disasm, прямое сравнение |
| GetStatus()==0 (pending) | вызов `[edi]+4` (vtbl slot 1) | `call dword ptr [eax+4]`; `test eax,eax; jnz` (выход если статус≠0) | raw disasm @ `0x1008a627-0x1008a62e` |
| reservationid | `[this+0x2C]` (byte 44, dword-idx 11) | `cmp eax,[ecx+1Ch]` где `ecx=edi+0x10` → эффективно `[edi+0x2C]`; сравнивается с `[arg_0+0x1C]` (offset 28 в wrapper-структуре источника) | raw disasm @ `0x1008a637-0x1008a640`, byte-точно |
| token | `[this+0x58]` (byte 88, dword-idx 22) | `cmp eax,[edi+58h]` где `eax=arg_C` | raw disasm @ `0x1008a675-0x1008a67b`, byte-точно |
| reservation-number | из `arg_4`(bf_read), 32 бита | стандартный bf_read 32-bit extraction (генерик битовый цикл, offset в самом UDP-пакете НЕ прослежен явно, т.к. это продолжение потока после diспетчер-consumed части) | raw disasm, паттерн идентичен множеству других bf_read-циклов в этом же файле |
| accepted count | из `arg_4`, 8 бит, регистр `bl`/`ebx` | `test eax,eax` где `eax`=ранее декодированное значение accepted-count (переиспользованный `[ebp+arg_0]` слот!) — **0 = success-ветка**, **0x7F(127) = спец.значение**, иначе partial | raw disasm @ `0x1008a936-0x1008a997`, byte-точно, включая явное `cmp [ebp+arg_0], 7Fh` @ `0x1008a993` |
| needed count | из `arg_4`, 8 бит | сохраняется в `[edi+0x50]`/`[edi+0x54]` вместе с accepted-count (см. ниже) | raw disasm |
| **STATUS = SUCCESS** | `[edi+0x08]` (byte 8, dword-idx 2) | **`mov dword ptr [edi+8], 4`** @ `0x1008a965` — БУКВАЛЬНАЯ, единственная запись значения 4 в этом поле во всей функции | raw disasm, 100% точно |
| STATUS = partial/unknown | тот же `[edi+0x08]` | `mov dword ptr [edi+8], 3` @ `0x1008a99c`, срабатывает когда accepted_count==0x7F | raw disasm |
| accepted/needed storage (для UI-лога) | `[edi+0x50]`/`[edi+0x54]` (byte 80/84) | запись после success/partial веток | raw disasm |
| **Callback invocation** | указатель на client-side callback-объект в `[edi+0x0C]` (byte 12, dword-idx 3) | `mov ecx,[edi+0Ch]; mov eax,[ecx]; push edi; call [eax]` — вызывает vtbl-slot-0 callback-объекта, **передавая `edi` (сам CServerMsg_CheckReservation) как аргумент** | raw disasm @ `0x1008a975-0x1008a978`; ЭТО ТОЧНО СОВПАДАЕТ с client.dll's `sub_103F7C00(this=callback_obj, a2=CServerMsg_ptr)` (§21) — `a2==this+128` на стороне client.dll и есть сравнение с ЭТИМ САМЫМ переданным указателем |

**Замыкание петли через оба бинарника (client.dll + engine.dll), подтверждено byte-precise raw disasm с обеих сторон** — это самый сильный результат всей серии RE: механизм callback от engine.dll к client.dll теперь прослежен буквально до конкретных инструкций на обоих концах.

### [HIGH CONFIDENCE]

- Поля request offset 25-32 (64-битное значение) — вероятно SteamID64 (не подтверждено буквальным именем, но паттерн `CreateInterface`+`vtbl+8` типичен для `ISteamUser::GetSteamID()`-подобного вызова).
- Offset 17-20 request-пакета (client-callback-pointer как "cookie") — соответствует полю `this+0x58`(token) ответа ТОЛЬКО архитектурно (оба "opaque correlation" поля), но **прямого 1:1 сопоставления which-request-field-becomes-which-response-field НЕ подтверждено** — сервер мог бы использовать ЛЮБОЕ поле запроса как источник эхо-токена, самый вероятный кандидат — offset 17 (client-callback-pointer), но не доказано.

### [NOT PROVEN]

- Семантика полей request offset 9 (arg_8), 13 (this+0x68), 21 (this+0x64) — только их СУЩЕСТВОВАНИЕ И РАЗМЕР подтверждены byte-precise, не их СМЫСЛ.
- Caller функции `sub_1008A4F0` (кто именно и с какими параметрами вызывает Send) — НЕ найден (только vtable-xref, 0 прямых code-call-сайтов) — необходим для полной семантики offset 9.
- Точный побайтовый offset "reservation-number"/accepted/needed-count полей ОТ НАЧАЛА UDP-пакета (offset 0) — известен их ПОРЯДОК относительно друг друга и относительно protocol-const/token (которые сам диспетчер прочитал РАНЬШЕ, до вызова case-37 хэндлера), но не абсолютный offset от начала пакета, т.к. часть header'а разбирается КОДОМ ВНЕ декомпилированных в этой сессии функций (сам диспетчер `sub_1008E7C0` слишком велик для полной построчной декомпиляции).
- Прямое сопоставление полей `MatchmakingGC2ClientReserve` (GC-протобуф, наш 9107) → конкретные offset'ы request-пакета — архитектурно вероятно (reservationid/direct_udp_ip/port используются ГДЕ-ТО в этой цепочке), но пошаговый dataflow от протобуф-полей до КОНКРЕТНОГО байта в UDP-пакете не прослежен до конца (обрывается на уровне client.dll's `this+40/44` reservationid копирования, §18-21).
- Server-side implementation (кто СТРОИТ opcode 37 на стороне сервера) — поиск в исходных деревьях запущен как фоновая read-only задача (см. ниже), результат на момент записи этого раздела ещё не получен.

### Server-side implementation — поиск в source trees (фоновая задача)

Запущен read-only поиск (subagent, `Explore`) по обоим доступным деревьям (`ida_deobfuscated_grok`, `cstrike15_src-master`) на предмет: `CServerMsg_CheckReservation`, `CServerMsg_Ping`, `CServerMsg` (базовый класс), `CAsyncOperation_ReserveServer`, `CNetSupportImpl`/`INETSUPPORT_003`, opcode `0x21`/`33`/`0x25`/`37` рядом с "reservation", строки `"Server confirmed all players reservation"`/`"Server reservation"`/`"is awaiting"`/`"We never heard from gameserver"`, `CheckReservation` как подстрока, `ReserveServerForQueuedGame`, и файлы вида `*servermsg*`/`net_chan*`/`*clientstate*`. **Результат не готов на момент записи этой секции — будет добавлен отдельным дополнением (§25a) при получении, без изменения уже записанных здесь фактов.**

### Итоговая сводка (строго по требуемому формату)

```
PROTOCOL CONSTANT:
dword_138EF1C4 = 13805 (ClientVersion из steam.inf без точек, HIGH CONFIDENCE/кросс-подтверждено
собственными Live Test логами; сам механизм вычисления -- BINARY CONFIRMED через sub_10282500)

REQUEST 0x21 (33 байта payload, все поля byte-aligned, little-endian x86):
  +0  4б  0xFFFFFFFF
  +4  1б  0x21
  +5  4б  protocol-const (13805)
  +9  4б  arg_8 (caller param, семантика UNKNOWN)
  +13 4б  this+0x68 (семантика UNKNOWN)
  +17 4б  this+0x60 (= client-callback-pointer, вероятный correlation cookie)
  +21 4б  this+0x64 (семантика UNKNOWN)
  +25 4б  SteamID64 low32 (HIGH CONFIDENCE)
  +29 4б  SteamID64 high32 (HIGH CONFIDENCE)

RESPONSE 0x25 (частично известный layout -- порядок confirmed, абсолютные offset'ы от начала
UDP payload НЕ до конца прослежены, т.к. часть header'а разбирает недекомпилированный
диспетчер sub_1008E7C0):
  0xFFFFFFFF + 0x25 + [часть заголовка, разбираемая диспетчером: protocol-const(32б),
  token(32б), + поле для сравнения с this+0x2C=reservationid] + reservation-number(32б) +
  accepted-count(8б, ДОЛЖЕН=0 для success, 0x7F=спец.значение) + needed-count(8б)

CORRELATION (byte-precise):
  this+0x2C (reservationid) == wrapper[+0x1C]
  this+0x58 (token) == arg_C
  arg_8 == dword_138EF1C4 (protocol-const)
  Все три -- ЯВНО ВАЛИДИРУЮТСЯ (cmp+jnz), не только логируются.

SUCCESS TRANSITION (100% byte-exact):
  mov dword ptr [this+8], 4    @ engine.dll 0x1008a965
  (срабатывает только когда accepted-count==0, ПОСЛЕ прохождения всех трёх correlation-проверок)

CALLBACK (100% byte-exact, замыкает петлю engine.dll -> client.dll):
  mov ecx,[this+0xC]; call [ecx-vtable-slot-0](this)
  -- соответствует client.dll's sub_103F7C00(this=callback_obj, a2=CServerMsg_ptr)

SERVER-SIDE SENDER:
Не найден в engine.dll в этой сессии (не декомпилирован); поиск по source trees запущен
как фоновая задача, результат ожидается отдельным дополнением.

MINIMAL RESPONDER SPEC:
Request offset'ы 0-8 (marker+opcode+protocol-const) -- ПОЛНОСТЬЮ известны и достаточны для
распознавания входящего запроса. Response offset'ы 0-8 (marker+opcode+protocol-const) --
полностью известны для построения ответа. НО: response ДОЛЖЕН эхом вернуть reservationid и
token, чьи ТОЧНЫЕ offset'ы В ЗАПРОСЕ не сопоставлены 1:1 с полями ответа (см. HIGH CONFIDENCE
выше) -- responder всё ещё НЕ может быть написан вслепую без этого последнего сопоставления,
либо без decompile диспетчера sub_1008E7C0 целиком для получения абсолютных offset'ов ответа.
СТАТУС: близко к завершению, но не 100% -- responder код по-прежнему НЕ пишется.
```

### §25a — SOURCE CONFIRMED дополнение (фоновый поиск вернулся)

Фоновый read-only поиск по обоим деревьям вернулся и нашёл ТОЧНУЮ исходную реализацию. **Приоритет по требованию пользователя: retail `engine.dll` (binary) > старый source** — там, где source и binary согласуются, это усиливает уверенность; где расходятся, доверяем binary. Ничего из ранее записанного binary-confirmed материала (§22-25) этим не отменяется, только дополняется именами/структурой.

**Файлы** (идентичны в обоих деревьях): `engine/baseclientstate.{h,cpp}`, `engine/baseserver.cpp`, `engine/net_support.{h,cpp}`, `public/engine/inetsupport.h`, `common/proto_oob.h`, `common/protocol.h`.

**Opcode-имена — SOURCE CONFIRMED, точно совпадает с binary**:
```c
// common/proto_oob.h (побайтово идентично в обоих деревьях)
#define A2S_RESERVE_CHECK           '!'   // = 0x21 = 33 -- ТОЧНО совпадает с нашим binary-найденным opcode запроса
#define S2A_RESERVE_CHECK_RESPONSE  '%'   // = 0x25 = 37 -- ТОЧНО совпадает с нашим binary-найденным opcode ответа
#define A2S_PING                    '$'
#define S2A_PING_RESPONSE           '^'   // = 0x5E = 94
```
**Резолвит открытый вопрос §24**: case `94` в диспетчере (`sub_1008AB50`) — это **`S2A_PING_RESPONSE`, обработчик `CServerMsg_Ping`, НЕ `CServerMsg_CheckReservation`** — они архитектурно родственные (общий `CServerMsg`), но ОТДЕЛЬНЫЕ сообщения. Ранее это было [NOT PROVEN], теперь SOURCE CONFIRMED (структурная идентичность подтверждена и в binary — sub_1008AB50 структурно идентичен sub_1008A610, что уже отмечалось).

**`CServerMsg` — полное определение базового класса (SOURCE CONFIRMED)**:
```cpp
class CServerMsg : public IMatchAsyncOperation
{
public:
    bool IsFinished() { return m_eState > AOS_ABORTING; }
    explicit CServerMsg( CBaseClientState *pParent, IMatchAsyncOperationCallback *pCallback,
        const ns_address& serverAdr, int socket, uint32 maxAttempts, double timeout );
    void Update( void );
    bool IsValidResponse( const ns_address &from, uint32 token );
    void ResponseReceived( uint64 result );
    virtual void SendMsg( const ns_address& serverAdr, int socket, uint32 token ) = 0;
};
```
**Резолвит несколько [NOT PROVEN] из §22-25**:
- Наше binary-найденное поле `this+0x58` ("token") — теперь **SOURCE CONFIRMED по имени**: параметр буквально называется `token`, используется в `IsValidResponse(from, token)` и `SendMsg(serverAdr, socket, token)`.
- **`maxAttempts` — реальный, именованный конструкторский параметр** (retry-limit из §22/24 [NOT PROVEN] теперь подтверждён КАК КОНЦЕПЦИЯ — это не "магическое" число, а явный аргумент конструктора). Конкретное числовое значение, переданное при создании `CServerMsg_CheckReservation` (`new CServerMsg_CheckReservation(...)` @ `baseclientstate.cpp:3603` в Tree A / `:3513` в Tree B), НЕ зафиксировано поиском (агенту не давалось задание читать сами аргументы вызова) — как конкретное число это остаётся не 100% подтверждённым, но МЕХАНИЗМ (явный, ограниченный retry-limit, отдельный от timeout) — да.
- `timeout` — отдельный, ЯВНЫЙ double-параметр конструктора — подтверждает наше binary-находку `0x3FF0000000000000`=1.0 (§22) как ИМЕННО ЭТОТ параметр, не что-то ещё.

**`CAsyncOperation_ReserveServer` — полное определение (SOURCE CONFIRMED)**:
```cpp
class CAsyncOperation_ReserveServer : public IMatchAsyncOperation
{
public:
    explicit CAsyncOperation_ReserveServer( CBaseClientState *pParent )
        : m_eState( AOS_RUNNING ), m_pParent( pParent ) { m_numGameSlotsForReservation = 0; }
    virtual bool IsFinished() { return m_eState > AOS_ABORTING; }
    virtual AsyncOperationState_t GetState() { return m_eState; }
    virtual uint64 GetResult();
    virtual uint64 GetResultExtraInfo() { return m_numGameSlotsForReservation; }
    virtual void Release() {
        if ( m_pParent && m_pParent->m_pServerReservationOperation == this ) {
            m_pParent->m_pServerReservationOperation = NULL;
            m_pParent->m_pServerReservationCallback = NULL;
        }
        delete this;
    }
public:
    AsyncOperationState_t m_eState;   // <-- наше binary "this+1"(dword-idx) = *(this+4 byte), CONFIRMED имя и тип
    ns_address m_adr;
    CBaseClientState *m_pParent;
    uint32 m_numGameSlotsForReservation;   // <-- наше binary "this+11"/GetResultExtraInfo, CONFIRMED
};
```
Это ТОЧНО резолвит §22's `sub_1005DD00`=`GetStatus()`→**реально `m_eState` accessor** и `sub_100A5050`=**реально `GetResultExtraInfo()`/`m_numGameSlotsForReservation`**.

**`AsyncOperationState_t` enum** — SOURCE CONFIRMED частично (не полный список значений, но реальные имена встреченных состояний): `AOS_RUNNING`, `AOS_ABORTING`, `AOS_FAILED` (из `NET_CheckReservationFailedOrAborted`'s `switch`, где `case AOS_FAILED: pszDebug = "Matchmaking failed";`). Полный enum (все числовые значения) НЕ зафиксирован поиском.

**Строки-логи — SOURCE CONFIRMED, ДОСЛОВНОЕ совпадение с нашим binary дампом**:
- `DevMsg("Server confirmed all players reservation%u/%d\n", uiReservationStage, numTotalClientsInReservation);` — Tree A (2021-era) `baseclientstate.cpp:413`, **ДОСЛОВНО совпадает** с нашей binary-строкой из §23 (два `%`-параметра: `%u`+`%d`).
- Tree B (2015 Hydra) имеет УКОРОЧЕННУЮ версию БЕЗ второго параметра (`"...reservation%u\n"`, только `uiReservationStage`) — **наш retail `engine.dll` соответствует линии Tree A (2021), НЕ Tree B (2015)** — важный, ранее не устанавливавшийся факт о происхождении/версии кода в нашей конкретной сборке.
- `"Matchmaking failed"` — найдена ТОЛЬКО в Tree A (`engine/net_ws.cpp:3372`, внутри `NET_CheckReservationFailedOrAborted`), **отсутствует в Tree B полностью** — вторичное подтверждение, что наш binary ближе к линии 2021-source, чем к 2015.
- `"We never heard from gameserver"` (наша §23-находка из `sub_1008A3E0`) — **НЕ найдена НИ В ОДНОМ из source tree** — это подтверждает, что `sub_1008A3E0` (client.dll's failure-reason formatter) — код ВНЕ `CServerMsg`-семейства, отдельная, более высокоуровневая логика (`CAsyncOperation_ReserveServer`-related, а не `CServerMsg_CheckReservation`), которая не была включена ни в один из доступных source-дропов (аналогично прочим "вырезанным" местам, раздел 3).

**Сервер-side sender — SOURCE CONFIRMED имя функции, тело НЕ прочитано**:
```
engine/baseserver.cpp: case A2S_RESERVE_CHECK: ReplyReservationCheckRequest( packet->from, msg );
```
(Tree A `:1232`, Tree B `:1186`). **`ReplyReservationCheckRequest`** — это и есть искомая server-side функция, строящая `S2A_RESERVE_CHECK_RESPONSE`(0x25). Её ТЕЛО не было прочитано этим поиском (агенту не давалось задание декомпилировать/цитировать функцию целиком) — это прямой, конкретный next-step для любого, кто продолжит эту работу (искать `ReplyReservationCheckRequest` в `engine/baseserver.cpp` в обоих деревьях и/или найти её бинарный аналог в **server-side** части `engine.dll`, которую мы НЕ исследовали в этой сессии — все наши RE-находки §22-25 были со стороны client receive-path).

**`CBaseClientState`-поля, подтверждающие callback-механизм**: `m_pServerReservationOperation`, `m_pServerReservationCallback` — ИМЕНОВАННЫЕ поля, подтверждают наш binary-найденный (raw disasm) паттерн "callback через `this+0x0C`" (§25) — это, вероятно, именно `m_pServerReservationCallback` или аналогичное поле у `CServerMsg_CheckReservation`, реализующее `IMatchAsyncOperationCallback`.

### Обновлённый итог

```
PROTOCOL CONSTANT: 13805 (без изменений, см. §24)

REQUEST 0x21 = A2S_RESERVE_CHECK ('!') -- SOURCE CONFIRMED имя опкода, точное совпадение с binary
RESPONSE 0x25 = S2A_RESERVE_CHECK_RESPONSE ('%') -- SOURCE CONFIRMED имя опкода, точное совпадение

TOKEN CORRELATION: SOURCE CONFIRMED по имени (`IsValidResponse(from, token)`, `SendMsg(..., token)`)
RESERVATIONID CORRELATION: остаётся binary-confirmed offset (this+0x2C), source не даёт отдельного имени

RETRY: maxAttempts -- SOURCE CONFIRMED как явный именованный конструкторский параметр
        (не просто внутренний счётчик); TIMEOUT=1.0с -- SOURCE CONFIRMED как отдельный double-параметр
        того же конструктора. Конкретное числовое значение maxAttempts НЕ зафиксировано.

SERVER-SIDE SENDER: ReplyReservationCheckRequest() @ engine/baseserver.cpp -- SOURCE CONFIRMED
        имя и call site, ТЕЛО функции не прочитано (next step, не выполнено в этой сессии).

VERSION LINEAGE: наш retail engine.dll соответствует 2021-era (Tree A) source lineage,
        не 2015 Hydra (Tree B) -- подтверждено дословным совпадением DevMsg-формат-строки
        и наличием "Matchmaking failed"/NET_CheckReservationFailedOrAborted только в Tree A.

CASE 94 RESOLVED: S2A_PING_RESPONSE (CServerMsg_Ping), НЕ CheckReservation -- закрывает
        последний [NOT PROVEN] пункт §24 про принадлежность case 94.
```

**MINIMAL RESPONDER SPEC по-прежнему НЕ пишется** — главный оставшийся пробел (точный побайтовый layout ответа от начала UDP payload, и тело `ReplyReservationCheckRequest`) не закрыт даже с учётом source-находок, поскольку сам source не даёт wire-level байтовой раскладки (это C++ высокого уровня, `msg.WriteByte`/`msg.ReadByte`-стиль, не сырые offset'ы) — для полной byte-exact спецификации всё ещё нужна либо декомпиляция `ReplyReservationCheckRequest`'s бинарного аналога (если он в этом же `engine.dll` — вероятно да, т.к. это un-dedicated listen-server-capable движок), либо построчная декомпиляция начала `sub_1008E7C0` (диспетчер, 8437 байт, не выполнено).

---

## 26. `ReplyReservationCheckRequest` — полное тело найдено, protocol ПОЛНОСТЬЮ восстановлен из source

Прямой, целевой read-only поиск (`Grep`+`Read`, не RE-агент) нашёл **полное тело функции** в обоих source tree. Это закрывает практически все оставшиеся [NOT PROVEN] из §24-25. Git не трогался, код/DLL проекта не менялись, live-тест не запускался.

### SERVER SOURCE FOUND

```
file:   engine/baseserver.cpp
class:  CBaseServer
func:   void CBaseServer::ReplyReservationCheckRequest( const ns_address &adr, bf_read &msgIn )
        (декларация: engine/baseserver.h:212, virtual)
caller: CBaseServer::ProcessConnectionlessPacket (или аналог) — switch по opcode,
        case A2S_RESERVE_CHECK: ReplyReservationCheckRequest( packet->from, msg );
        Tree A (2021, ida_deobfuscated_grok): baseserver.cpp:1232, тело функции :2165-2308
        Tree B (2015, cstrike15_src-master):  baseserver.cpp:1186, тело функции :2124-~2230
callee: GetHostVersion(), GetReservationCookie(), sv_mmqueue_reservation (ConVar),
        m_arrReservationPlayers (CUtlVector<QueueMatchPlayer_t>), CSteamID::GetAccountID(),
        serverGameDLL->ReportGCQueuedMatchStart(...), NET_SendPacketToNsAddress (Tree A) /
        NET_SendPacket (Tree B)
```

**Логика (не копия кода, анализ)**:
1. Отбрасывает пакет, если `ReadLong() != GetHostVersion()` (protocol mismatch → молчаливый return, НИКАКОГО ответа вообще).
2. Читает `token`, `uiReservationStage`, 64-битный `nReservationCookie`, 64-битный `uiClientSteamID`.
3. `reservationMatch = (nReservationCookie == GetReservationCookie())` — сравнение с локальным cookie сервера (тот самый `sv_mmqueue_reservation` cookie из разделов 3/6/18, формат `"Q<cookie_hex>,..."`).
4. Если `sv_mmqueue_reservation` начинается с `'Q'` (queued) — ищет клиента в `m_arrReservationPlayers` по `AccountID` (из SteamID), обновляет его `token`/`stage`, считает `uiActualAwaitingClients` = сколько игроков ещё НЕ достигли текущего stage. Если клиент найден — `uiAwaitingClients = uiActualAwaitingClients`. Если `uiActualAwaitingClients==0` (ВСЕ достигли stage) и это первое такое подтверждение — вызывает `serverGameDLL->ReportGCQueuedMatchStart(...)` (та самая функция из раздела 6/project446!) и продлевает `m_flReservationExpiryTime`.
5. Если `sv_mmqueue_reservation` начинается с `'G'` (in-progress/joinable) — **безусловно** `uiAwaitingClients = 0` (мгновенный success для ЛЮБОГО клиента с верным cookie, БЕЗ проверки списка игроков).
6. `uiAwaitingClients` по умолчанию = `0x7F` (127) — тот самый "спец.значение" из §23/24!
7. Строит и шлёт ответ (см. ниже), и ДОПОЛНИТЕЛЬНО рассылает тот же ответ ВСЕМ игрокам в `m_arrReservationPlayers`, если `uiAwaitingClients==0` (чтобы все клиенты узнали о завершении одновременно).

### 0x21 REQUEST FORMAT (из `msgIn.Read*()` вызовов, ПОЛНОСТЬЮ подтверждено, порядок = порядок чтения)

| offset | size | field | source | notes |
|---|---|---|---|---|
| 0 | 4 | `0xFFFFFFFF` | `CONNECTIONLESS_HEADER` | стандартный маркер |
| 4 | 1 | `0x21` | `A2S_RESERVE_CHECK` | опкод |
| 5 | 4 | `GetHostVersion()` | `msgIn.ReadLong()` | protocol version; **ЕСЛИ НЕ СОВПАДАЕТ — сервер молчит, ответа НЕТ ВООБЩЕ** |
| 9 | 4 | `token` | `msgIn.ReadLong()` | **резолвит §25's "arg_8, semantics UNKNOWN" → это TOKEN** |
| 13 | 4 | `uiReservationStage` | `msgIn.ReadLong()` | **резолвит §25's "this+0x68, semantics UNKNOWN" → это RESERVATION STAGE** |
| 17 | 8 | `nReservationCookie` | `msgIn.ReadLongLong()` | **резолвит §25's ДВА поля "this+0x60"+"this+0x64" (semantics UNKNOWN) → это ОДНО 64-битное поле, cookie сервера (`sv_mmqueue_reservation`)** |
| 25 | 8 | `uiClientSteamID` | `msgIn.ReadLongLong()` | подтверждает §25's HIGH CONFIDENCE "SteamID64" — теперь CONFIRMED по имени и как ОДНО 64-битное поле (не 2×32, но эквивалентно по факту) |

**Итого 33 байта** — ТОЧНО совпадает с байт-точным подсчётом из raw disasm §25 (4+1+4+4+4+8+8=33). Полное совпадение количества и размера полей между source (`Read*` вызовы) и binary (raw disasm `push <width>` последовательность) — не осталось ни одного неучтённого байта.

### 0x25 RESPONSE FORMAT (из `msg.Write*()` вызовов, ПОЛНОСТЬЮ подтверждено)

| offset | size | field | source | notes |
|---|---|---|---|---|
| 0 | 4 | `0xFFFFFFFF` | `CONNECTIONLESS_HEADER` | |
| 4 | 1 | `0x25` | `S2A_RESERVE_CHECK_RESPONSE` | |
| 5 | 4 | `GetHostVersion()` | `msg.WriteLong(...)` | тот же protocol-version, что и в запросе |
| 9 | 4 | `token` | `msg.WriteLong(token)` | **ЭХО значения из запроса**, не пересчитывается |
| 13 | 4 | `uiReservationStage` | `msg.WriteLong(uiReservationStage)` | **ЭХО значения из запроса** — это и есть "reservation number" из §23/25 |
| 17 | 1 | `uiAwaitingClients` | `msg.WriteByte(...)` | **0 = SUCCESS**, `0x7F`(127) = no-match/default, 1..126 = частично |
| 18 | 1 | `uiTotalClientsInReservation` | `msg.WriteByte(...)` | **только в Tree A (2021)** — см. RETAIL BINARY CROSS-CHECK ниже |

**Итого 19 байт** (Tree A/2021) или **18 байт** (Tree B/2015, поле `uiTotalClientsInReservation` отсутствует целиком — переменная даже не объявлена в Tree B).

### RETAIL BINARY CROSS-CHECK

| field | server source (Tree A/2021) | retail parser (`sub_1008A610`, §25 raw disasm) | совпадает? | confidence |
|---|---|---|---|---|
| header+opcode | `0xFFFFFFFF`+`0x25` | (проверяется диспетчером `sub_1008E7C0` до вызова case37) | ДА | CONFIRMED |
| protocol version | `GetHostVersion()` @ offset 5 | `arg_8`, `cmp eax,dword_138EF1C4` | ДА (по позиции и роли) | CONFIRMED |
| token | `token` @ offset 9 | `arg_C`, `cmp eax,[edi+0x58]` | ДА | CONFIRMED |
| reservationStage / "reservation number" | `uiReservationStage` @ offset 13 | 32-битное поле, читаемое из `arg_4`(bf_read), используется ТОЛЬКО для DevMsg-лога `"...reservation%u..."` | ДА — %u в логе = `uiReservationStage`, эхо-логика подтверждена | CONFIRMED |
| awaiting/accepted count | `uiAwaitingClients` @ offset 17 | 8-битное поле `bl`, `test eax,eax`(==0 → success), `cmp...,7Fh`(==127 → status=3) | ДА, включая спец.значение 0x7F | CONFIRMED |
| total/needed count | `uiTotalClientsInReservation` @ offset 18 | 8-битное поле, сохраняется в `this+0x50/0x54` вместе с accepted-count | ДА | CONFIRMED |
| **reservationCookie** | НЕ включён в исходящий ОТВЕТ вообще (только в запросе, проверяется на сервере) | binary НЕ читает cookie-поле из ОТВЕТА (нечего читать — сервер его не шлёт обратно) | **н/д** | Резолвит открытый вопрос §25: моя более ранняя гипотеза "this+0x2C = reservationid, сравнивается с полем ответа" была **НЕВЕРНОЙ** — сервер cookie обратно не шлёт, значит клиентская проверка `[edi+0x2C]` из raw disasm §25 сравнивает НЕ reservationid из пакета, а что-то другое (вероятно исходный адрес отправителя пакета, сверяемый с ожидаемым адресом сервера, либо внутренний sequence/session id диспетчера — не пакетное поле). **Явная переоценка/поправка предыдущего раздела.** |

**Расхождение source-версий (Tree A vs Tree B) объяснено, не выбрано молча**: Tree B (2015 Hydra) не имеет байта `uiTotalClientsInReservation` — ответ там 18, а не 19 байт. Наш retail `engine.dll` (согласно §25a) читает ДВА байта после awaiting-count (`this+0x50`/`this+0x54` оба заполняются) — **значит наш retail binary соответствует Tree A (2021) 19-байтовому формату**, не Tree B. Это уже отмечалось в §25a по DevMsg-строкам, здесь — независимое структурное подтверждение тем же выводом через сам протокол.

### CONFIRMED

- Полный, точный, байт-в-байт формат запроса (33 байта) и ответа (19 байт, retail/2021-линия) — SOURCE CONFIRMED, независимо подтверждено byte-count через binary raw disasm (§25).
- Условие успеха на СЕРВЕРНОЙ стороне: `uiReservationStage != 0 && reservationCookie совпал && clientSteamID != 0 && (sv_mmqueue_reservation[0]=='Q' && все игроки достигли stage) || (sv_mmqueue_reservation[0]=='G')`.
- `uiAwaitingClients=0` — единственное условие для клиентского `status=4` (полностью совпадает с §25's binary-находкой `accepted-count==0`).
- Если protocol version не совпал — сервер НЕ отвечает вообще (это ОБЪЯСНЯЕТ часть "no response" сценариев, отдельно от "сервер не слушает вообще").
- `ReportGCQueuedMatchStart` (project446-находка, раздел 6) вызывается ИМЕННО отсюда — сервер сам решает, когда все подтвердили, и уведомляет GC.

### HIGH CONFIDENCE

- `GetHostVersion()` численно равен нашему `dword_138EF1C4`=13805 — архитектурно то же самое понятие (protocol/build version), но их буквальное числовое равенство не проверено вызовом обеих функций напрямую (разумно предполагать совпадение, раз протокол работает между клиентом и сервером одной версии игры).
- `'G'`-ветка (`sv_mmqueue_reservation` начинается с `'G'`) даёт **безусловный** success (`uiAwaitingClients=0`) без проверки списка игроков — для минимального responder'а это ЗНАЧИТЕЛЬНО проще реализовать, чем полная `'Q'`-ветка с учётом других игроков.

### NOT PROVEN

- Буквальное числовое равенство `GetHostVersion()` (сервер) и `dword_138EF1C4` (клиент) — не вызваны обе функции бок о бок для сравнения.
- Истинная семантика клиентского `this+0x2C`/`[edi+0x2C]` сравнения (§25) — пересмотрена как НЕ reservationid (см. таблицу выше), но новая гипотеза (адрес отправителя / внутренний session id) тоже не подтверждена декомпиляцией диспетчера `sub_1008E7C0`'s начала (по-прежнему не выполнена ни в одной сессии).
- NET_SendPacketToNsAddress vs NET_SendPacket — чисто переименование между версиями (§25a), не влияет на wire-формат.

### MINIMAL RESPONDER SPEC (полная, для реализации без угадывания)

```
LISTEN: UDP порт из нашего 9107 (direct_udp_port), адрес = direct_udp_ip

ПРИЁМ (ожидаем от клиента, opcode 0x21, 33 байта):
  +0  u32 0xFFFFFFFF
  +4  u8  0x21
  +5  u32 protocol_version   (сравнить с ожидаемым; если не совпал -- НЕ отвечать вообще,
                               это штатное поведение самого retail сервера)
  +9  u32 token              (сохранить -- эхом вернуть в ответе)
  +13 u32 reservation_stage  (сохранить -- эхом вернуть в ответе)
  +17 u64 reservation_cookie (сравнить со СВОИМ известным cookie -- этот cookie мы САМИ
                               задаём/знаем, т.к. управляем reservationid в нашем 9107)
  +25 u64 client_steam_id    (можно использовать для лога/учёта, не обязательно для ответа)

ОТПРАВКА (ответ, opcode 0x25, 19 байт -- retail/2021-формат):
  +0  u32 0xFFFFFFFF
  +4  u8  0x25
  +5  u32 protocol_version   (ТО ЖЕ значение, что пришло в запросе -- просто эхо)
  +9  u32 token              (эхо из запроса)
  +13 u32 reservation_stage  (эхо из запроса)
  +17 u8  awaiting_clients = 0x00   <-- ОБЯЗАТЕЛЬНО ноль для немедленного success
  +18 u8  total_clients_in_reservation = 1  (или реальное число игроков, не критично для успеха)

УСЛОВИЕ УСПЕХА НА КЛИЕНТЕ: protocol_version совпал AND token совпал (эхо) AND awaiting_clients==0
  → status=4 → client.dll queue connect (§21/§25)

УПРОЩЕНИЕ: не обязательно эмулировать полную 'Q'-логику с списком игроков (см. HIGH CONFIDENCE) --
  достаточно ВСЕГДА отвечать awaiting_clients=0 при совпадении protocol_version, ровно как
  делает retail-сервер в 'G'-ветке.

ОСТАЮЩИЙСЯ РИСК (см. NOT PROVEN): клиентская проверка [this+0x2C] в engine.dll, чья точная
  роль не установлена -- если это проверка исходящего IP-адреса отправителя пакета
  (наиболее вероятная гипотеза), то responder ДОЛЖЕН отвечать именно с того же адреса/порта,
  на который клиент отправил запрос (что естественно для simple UDP echo responder) -- риск
  низкий, но не 0% подтверждён.
```

Спецификация теперь ПОЛНАЯ и byte-exact для request/response полей, происходящих напрямую из `ReplyReservationCheckRequest`. Единственный остаточный неопределённый пункт (`this+0x2C` client-side проверка) — с высокой вероятностью совпадает с естественным поведением любого корректного UDP-responder'а (ответ с того же адреса, куда пришёл запрос), поэтому НЕ должен быть блокером для реализации, но остаётся формально не 100%-но подтверждённым до отдельной проверки.

---

## 27. STATE AT COMPACTION (checkpoint перед `/compact`)

Этот раздел — снимок состояния перед сжатием контекста. Ничего нового не исследовалось при его записи, код не менялся. Все факты ниже уже были получены в предыдущих RE-раундах (§18-26) плюс read-only проверке этого раунда (`ReserveServerForQueuedGame`/`SetReservationCookie`/`SvMmQueueReservationChanged`, все — `engine/baseserver.cpp`, прочитаны напрямую, не переоткрывались заново).

### Архитектурное решение этого раунда

Выбран **вариант A**: настоящий CS:GO 2021 dedicated server должен сам отвечать на `A2S_RESERVE_CHECK` своим штатным `CBaseServer::ReplyReservationCheckRequest` (§26) — НЕ самодельный `TestMM`-responder. Причина: `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (9107) несёт только ОДНО адресное поле (`direct_udp_ip`/`direct_udp_port`/`server_address`, §2), используемое клиентом И для `A2S_RESERVE_CHECK`-таргета, И для финального `QueueConnect`-адреса (`"adronline"`, §21, `sub_103F7C00`: `sub_103EF6C0(v32, this+96)` — то же поле `this+96`, что было установлено при обработке 9107 и никогда не перезаписывается содержимым `0x25`-ответа). Раздельные fake-responder + real-server endpoint'ы физически несовместимы с этим протоколом — доказано на уровне схемы, не гипотеза.

### [CONFIRMED] — `sv_mmqueue_reservation` / cookie / reservation flow (`engine/baseserver.cpp`, оба source tree идентичны построчно в этой части)

**Формат payload, ожидаемый `ReserveServerForQueuedGame`** (`baseserver.cpp:4159-4215`):
```c
bool CBaseServer::ReserveServerForQueuedGame( char const *szReservationPayload )
{
    if (!szReservationPayload) return false;
    switch (szReservationPayload[0]) {
        case 'Q':   // Queued competitive, locked game from the start
        case 'G':   // Joinable in progress game
            break;
        case 'R':   // sscanf(payload+1, "%p", &m_pnReservationCookieSession) -- другое назначение, не для нас
            ...
            return true;
        default:
            return false;
    }
    if (sv_ShutDown_WasRequested()) return false;   // сервер в режиме shutdown -- не резервируется

    uint64 uiReservationCookie = 0, uiMatchID = 0; int32 bReserve = 0;
    sscanf(szReservationPayload+1, "%llx,%llx,%d:", &uiReservationCookie, &uiMatchID, &bReserve);
    if (!uiReservationCookie || !uiMatchID) return false;   // ОБА поля обязаны быть ненулевыми

    m_nMatchId = uiMatchID;
    if (bReserve) {
        if (!IsReserved() || (GetReservationCookie() == uiReservationCookie)) {
            m_flReservationExpiryTime = net_time + sv_mmqueue_reservation_timeout.GetFloat();
            sv_mmqueue_reservation.SetValue(szReservationPayload);
            SetReservationCookie(uiReservationCookie, "ReserveServerForQueuedGame: %s", szReservationPayload);
            return true;
        }
        return false;
    } else {
        // bReserve=0 -- запрос СНЯТИЯ резервации, требует совпадения cookie
        ...
    }
}
```
**Итоговый формат строки**: `"G<cookie_hex>,<matchid_hex>,1:"` (для немедленного "joinable"-режима без списка игроков, `'G'`-ветка §26) либо `"Q<cookie_hex>,<matchid_hex>,1:[accountid_hex]..."` (полная очередь с игроками, раздел 3/18). `bReserve` должен быть `1` (не `0`) чтобы реально зарезервировать.

**`m_nReservationCookie` — местоположение и единственный путь установки**:
```c
uint64 CBaseServer::GetReservationCookie() const { return m_nReservationCookie; }   // baseserver.cpp:4154-4157, просто геттер кэша

void CBaseServer::SetReservationCookie( uint64 uiCookie, char const *pchReasonFormat, ... )   // baseserver.cpp:4217-4296
{
    if (uiCookie != m_nReservationCookie) {
        ... (парсинг списка игроков из строки для 'Q'-случая, лог) ...
    }
    m_nReservationCookie = uiCookie;   // <-- ЕДИНСТВЕННОЕ место записи этого поля во всём файле
    UpdateGameData();
    sv_hosting_lobby.SetValue(IsReserved());
}
```
`SetReservationCookie()` вызывается **только** из `ReserveServerForQueuedGame()` (строка 4199). Больше нигде в `baseserver.cpp` эта функция не вызывается.

**Почему консоль/RCON НЕ работает — точная причина, не гипотеза**:
```c
// baseserver.cpp:236-241
static void SvMmQueueReservationChanged( IConVar *pConVar, const char *pOldValue, float flOldValue )
{
    if ( serverGameDLL )
        serverGameDLL->UpdateGCInformation();   // <-- ЕДИНСТВЕННОЕ действие колбэка, cookie не трогает
}
ConVar sv_mmqueue_reservation( "sv_mmqueue_reservation", "", FCVAR_DEVELOPMENTONLY | FCVAR_DONTRECORD,
    "Server queue reservation", SvMmQueueReservationChanged );
```
Ручная установка ConVar через консоль/rcon (`sv_mmqueue_reservation "G...,...,1:"`) меняет **только** отображаемое строковое значение ConVar'а и триггерит `UpdateGCInformation()` — **`m_nReservationCookie` при этом не меняется**, остаётся тем, что было (скорее всего `0`). Значит `ReplyReservationCheckRequest`'s проверка `reservationMatch = (nReservationCookie == GetReservationCookie())` (§26) провалится для ЛЮБОГО входящего запроса.

**Две отдельные, не пересекающиеся цепочки**:
```
Цепочка A (косметическая, НЕ устанавливает cookie):
  sv_mmqueue_reservation (ConVar, ручное consle/rcon изменение)
    → SvMmQueueReservationChanged (callback, baseserver.cpp:236)
    → serverGameDLL->UpdateGCInformation()   [уведомление game-DLL, не резервация]

Цепочка B (единственная реальная, устанавливает cookie):
  IVEngineServer::ReserveServerForQueuedGame(payload)   [public/eiface.h:525, чистый virtual]
    → vengineserver_impl.cpp:1609-1611: bool CVEngineServer::ReserveServerForQueuedGame(...)
        { return sv.ReserveServerForQueuedGame(szReservationPayload); }
    → CBaseServer::ReserveServerForQueuedGame (baseserver.cpp:4159)
    → SetReservationCookie(uiReservationCookie, ...) (baseserver.cpp:4217)
    → m_nReservationCookie = uiCookie   (единственная запись)
```

**Подтверждение отсутствия консольной команды**: полнотекстовый поиск `CON_COMMAND`/`ConCommand` рядом с "reserv" по всему `ida_deobfuscated_grok/src` дал только: `threadpool_cycle_reserve` (`engine/host.cpp:687`, не связан с матчмейкингом) и **закомментированный** `// CON_COMMAND( sv_unreserve, "Clears any lobby reservation for this server\n" )` (`baseserver.cpp:4556`) — то есть Valve намеренно НЕ экспонировали управление резервацией в консоль. `IVEngineServer::ReserveServerForQueuedGame` — интерфейс, доступный только программно (из `server.dll`/game-DLL кода или из процесса, имеющего доступ к этому интерфейсу, как это делает `project446/csgo_gc.dll`'s `sub_10088500`, раздел 6).

### [CONFIRMED] — сводка протокола `0x21`/`0x25` (полная версия — §26, здесь только контрольные цифры для быстрого доступа)

```
GameServerCookieId = 0x293A206F6C6C6548   (csgo_gc/gc_const_csgo.h:6, уже используется в
                                             OnMatchmakingStart как reservationid в нашем 9107)

REQUEST  A2S_RESERVE_CHECK  (0x21, '!', 33 байта):
  +0  u32 0xFFFFFFFF (CONNECTIONLESS_HEADER)
  +4  u8  0x21
  +5  u32 protocol/host version  (GetHostVersion() на сервере; dword_138EF1C4=13805 на клиенте)
  +9  u32 token
  +13 u32 reservation_stage
  +17 u64 reservation_cookie   (сравнивается с GetReservationCookie()/m_nReservationCookie)
  +25 u64 client_steam_id

RESPONSE S2A_RESERVE_CHECK_RESPONSE  (0x25, '%', 19 байт, retail/2021-формат):
  +0  u32 0xFFFFFFFF
  +4  u8  0x25
  +5  u32 protocol/host version (echo)
  +9  u32 token (echo)
  +13 u32 reservation_stage (echo)
  +17 u8  awaiting_clients   -- 0 = SUCCESS -> status=4 на клиенте; 0x7F(127) = no-match/default
  +18 u8  total_clients_in_reservation

Условие успешного 0x25 (engine/baseserver.cpp:2165-2308, ReplyReservationCheckRequest):
  msgIn.ReadLong()==GetHostVersion()  AND  reservationCookie==GetReservationCookie()  AND
  clientSteamID!=0  AND  ( sv_mmqueue_reservation[0]=='G'  [безусловно awaiting=0]
                            ИЛИ  sv_mmqueue_reservation[0]=='Q' И все игроки в
                                 m_arrReservationPlayers достигли reservation_stage )
  Если protocol version не совпал -- сервер НЕ отвечает вообще (молчаливый return).

Server-side sender (SOURCE CONFIRMED, тело прочитано полностью):
  file:  engine/baseserver.cpp
  class: CBaseServer
  func:  void CBaseServer::ReplyReservationCheckRequest(const ns_address &adr, bf_read &msgIn)
  Tree A (2021, ida_deobfuscated_grok): объявление baseserver.h:212, caller :1232, тело :2165-2308
  Tree B (2015, cstrike15_src-master):  caller :1186, тело :2124-~2230 (БЕЗ поля
         uiTotalClientsInReservation -- ответ 18 байт, не 19; наш retail engine.dll
         соответствует Tree A/19-байтовому формату, подтверждено независимо в §25a и §26)
```

### [CONFIRMED] — состояние кода проекта на момент компакции

- `csgo_gc/test_mm.h`, `csgo_gc/test_mm.cpp` — **созданы, остаются в проекте и в `CMakeLists.txt`** (компилируются в ту же `csgo_gc.dll`, `test_mm.cpp.obj` подтверждён в сборке), но **НЕ вызываются** ни из какого runtime-пути (вызов `TestMM::EnsureStarted()` удалён из `gc_client.cpp`, инклюд `#include "test_mm.h"` тоже удалён оттуда). Оставлены явно для будущих protocol-only тестов.
- `csgo_gc/gc_client.cpp`, `ClientGC::OnMatchmakingStart` — читает `game_type` из реального 9101, декодирует `eGame = game_type & 0xF` (раздел 20), обрабатывает только `eGame==7` (Casual), берёт адрес/порт **из `GetConfig().TestServerAddress()`/`TestServerPort()`** (не хардкод), кладёт их в `direct_udp_ip`/`direct_udp_port`/`server_address` нашего 9107, шлёт `SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve, reserve)`. `reservationid = GameServerCookieId`.
- `csgo_gc/config.h`/`config.cpp` — добавлены `GCConfig::TestServerAddress()`/`TestServerPort()`, читаются из новой KV-секции `"matchmaking" { "test_server_address" "test_server_port" }` (см. `examples/config.txt`). Дефолты `127.0.0.1`/`27015`, сохраняются даже если секция отсутствует в файле.
- `csgo_gc/CMakeLists.txt` — `test_mm.cpp` в списке исходников, `ws2_32` линкуется на Windows (нужно для сокет-кода в `test_mm.cpp`, хоть он сейчас и не вызывается).
- **Сборка последний раз проходила чисто**: 0 errors, 0 warnings, `csgo_gc.dll` собрана в `Build/release/csgo_gc/csgo_gc.dll` (последний билд после удаления вызова `TestMM::EnsureStarted`, timestamp самый свежий на момент записи этого раздела).
- **DLL НЕ копировалась** в `D:\SteamLibrary\steamapps\common\csgo legacy` ни разу за всю последнюю серию правок (пользователь ставит вручную).
- **Live-тест НЕ проводился** для текущей (пост-TestMM-removal) версии кода.
- **Git НЕ трогался**: только `git status --short` для проверки; ни одного `commit`/`push`/`branch`/`reset`/`revert` за всю эту серию сообщений. Ветка остаётся `experimental-native-mm` (последний известный коммит `2455dcf Create RESEARCH_FINDINGS.md`, всё текущее — untracked/modified working tree).

### ГЛАВНЫЙ ТЕКУЩИЙ BLOCKER

Реальный dedicated server не ответит на `0x21`, потому что `m_nReservationCookie` на нём равен `0` (или чему угодно, не совпадающему с `GameServerCookieId`), а способа установить его кроме программного вызова `IVEngineServer::ReserveServerForQueuedGame()` не существует (см. [CONFIRMED] выше — консольной команды нет, ConVar-callback cookie не трогает). **У нашего проекта сейчас НЕТ кода, вызывающего этот API** — ни в `gc_server.cpp` (`ServerGC`), ни где-либо ещё.

### СЛЕДУЮЩИЙ ЭТАП (не начат, только зафиксирован как план)

**Read-only аудит server-side reservation path**, приоритет:
1. `project446/csgo_gc.dll`'s `sub_100694A0`... нет, конкретно **`sub_10088500`** (RVA `0x10088500`, раздел 6) — уже частично декомпилирован ранее в этой сессии, подтверждён как вызывающий `IVEngineServer::ReserveServerForQueuedGame` НАПРЯМУЮ (не через ConVar) — нужно ДОДЕКОМПИЛИРОВАТЬ его полностью (в разделе 6 отмечен как "TODO / NOT YET REVERSED — только частично декомпилирован"), чтобы увидеть ТОЧНЫЙ формат payload-строки, которую project446 реально передаёт (сравнить с форматом из §27 выше).
2. Найти, как и откуда `project446` получает интерфейс `IVEngineServer` (со стороны `ServerGC`/своего `csgo_gc.dll`, инжектированного в `server.dll`/`srcds`-процесс) — вероятно через `CreateInterface`, аналогично `INETSUPPORT_003` паттерну из §22.
3. Наш собственный `csgo_gc/gc_server.cpp` (`ServerGC`) — read-only проверка, есть ли там уже ЛЮБОЙ код, обращающийся к `IVEngineServer` (вероятно нет, не проверялось явно в этой сессии) — определить, что именно нужно будет добавить (это уже КОД, не RE, и потребует явного разрешения пользователя перед изменением).
4. Проверить `sub_100BADE0` (раздел 6, `ReportGCQueuedMatchStart` vtable-hook) — возможно, связан с тем же местом, где project446 решает ВЫЗВАТЬ `ReserveServerForQueuedGame` (т.е. могут быть частью одной и той же цепочки логики "когда игроки готовы -> резервируем сервер").

### Confidence levels для этого раздела

- `sv_mmqueue_reservation` payload-формат `"G/Q<hex>,<hex>,<0|1>:..."` — **CONFIRMED** (source, `baseserver.cpp:4184-4189`, читается напрямую `sscanf`).
- `m_nReservationCookie` устанавливается только через `SetReservationCookie`, вызываемую только из `ReserveServerForQueuedGame` — **CONFIRMED** (полное чтение файла, единственные вхождения).
- Консольная команда для резервации отсутствует — **CONFIRMED** (полнотекстовый поиск `CON_COMMAND`/`ConCommand` по всему дереву, единственные совпадения нерелевантны/закомментированы).
- Наш retail `engine.dll` использует именно этот (2021/Tree A) вариант функции, не Hydra/2015 — **HIGH CONFIDENCE** (независимое подтверждение через длину ответа и DevMsg-текст, §25a/§26), не собрано напрямую (сам retail `engine.dll` не грепался на этот конкретный кусок текста в этом раунде — уверенность идёт от предыдущих раундов).
- `project446`'s `sub_10088500` вызывает `IVEngineServer::ReserveServerForQueuedGame` — **CONFIRMED** (раздел 6, но параметры/формат payload — **TODO**, не декомпилировано полностью).
- Наш `gc_server.cpp` не содержит вызовов `IVEngineServer` — **НЕ ПРОВЕРЕНО в этой сессии** (предположение по общему знанию проекта, требует read-only проверки как первый шаг следующего этапа).

---

## 28. Server-side reservation path — полный аудит `project446` (`sub_10088500` + вызывающая цепочка)

Read-only IDA-декомпиляция (`idalib`, `p446_csgo_gc.dll.i64`, тот же образ раздела 6), полное тело `sub_10088500` (1788 байт) и всей вызывающей цепочки восстановлено. Скрипты/выводы в scratchpad: `query_reserve_chain.py`(`_out.txt`), `query_ab08_xrefs.py`(`_out.txt`), `query_cookie_resolve.py`(`_out.txt`), `query_payload_format.py`(`_out.txt`), `query_qg_builders.py`(`_out.txt`).

### 28.1 Полная цепочка `ServerGC → ... → ReserveServerForQueuedGame() → SetReservationCookie()` — **CONFIRMED**

```
[GC] получает 9105 (MatchmakingGC2ServerReserve) от backend
  ↓
sub_1009B750 (0x1009B750, 9837 байт, полностью виден) — главный 9105/9106 relay-обработчик
  - парсит match_id, accounts[], game_type, flags из 9105
  - сохраняет в per-reservation struct (this-указатель `a1`)
  - лог "[GC] Stored 9105: match_id=%llu accounts=%d game_type=%u flags=%u"
  ↓ (только если game_type ∈ {8 Competitive, 10 Wingman} — см. 28.3, DangerZone(13) не покрыт этим конкретным срезом декомпиляции)
sub_10094120 (0x10094120, 1408 байт, ПОЛНОСТЬЮ декомпилирован) — "native Q reserve" builder
  1. cookie = sub_1008FD20(reservationStruct, &outBuf)   [= ResolveReservationCookie, см. 28.2]
  2. matchid = *(struct+32) (QWORD); если 0 → fallback: matchid = cookie
  3. payload = sprintf("Q%llx,%llx,%d:", cookie, matchid, accountCount)   [точный формат, см. 28.4]
  4. добавляет "[acctid_hex]" / "[0]" токены для команды 1, затем команды 2 (см. 28.5)
  5. gated по env var GC_NATIVE_RESERVE (TLS-кэшированная проверка, sub_1053F3D7)
  6. push payload в EngineSystem-очередь через sub_100882A0 (push-вариант #3, тег "Server"/type=2)
  7. лог "[GC] native Q reserve: %s (%d CT + %d T, padded to %d+%d, ip=%s cookie=%llx)"
  ↓
sub_10088500 (0x10088500, 1788 байт, ПОЛНОСТЬЮ декомпилирован) — "EngineSystem" command processor
  - вызывается с периодического тика из sub_10095E10 / sub_100F5D00 / sub_100F66F0 (3 разных call site)
  - извлекает команду из кольцевого буфера (128 слотов, dword_1074A614/618/61C/620), проверяет "stale" (generation counter)
  - switch на *(cmd+24) [command type]:
      case 0 → резолвит "VEngineClient015"‥"012" через CreateInterface(engine.dll) — клиентская команда, НЕ используется для reservation
      case 2 → **вызывает vtable-слот 149 (offset 0x254=596 байт) интерфейса IVEngineServer**: `((this,payload) → bool)(dword_1074AB08, payload)`
               лог "EngineSystem: ReserveServerForQueuedGame(\"%.64s\") -> %d"
      case 3 → резолвит "INETSUPPORT_003" (dword_1074AB7C), если уже резолвлен — тоже вызывает тот же слот 149 ("fresh reservation" путь)
      default → sub_10087E50 (не декомпилирован в этом раунде, не критично для reservation)
  - retry-логика: до 5 повторов с интервалом 1000мс при неуспехе ("Command deferred (retry %d/%d in %ums)"), после 5 — "Command failed after %d retries"
  ↓
dword_1074AB08 (кэш указателя на IVEngineServer) резолвится в sub_10087D70 (0x10087D70, ПОЛНОСТЬЮ декомпилирован):
  GetModuleHandleA("engine.dll") → GetProcAddress(.., "CreateInterface") →
  пробует по очереди: "VEngineServer021", "VEngineServer022", "VEngineServer023", "VEngineServer020"
  лог при успехе: "EngineSystem: IVEngineServer resolved via \"%s\" (ServerCommand vtbl[%u])" (слот 38 = ServerCommand, отдельная санити-проверка, НЕ тот же слот что ReserveServerForQueuedGame)
  ↓
IVEngineServer::ReserveServerForQueuedGame(payload)   [vtable slot 149, offset 596]
  ↓ [уже CONFIRMED в §27, engine/baseserver.cpp:4159-4296, source]
CVEngineServer::ReserveServerForQueuedGame → CBaseServer::ReserveServerForQueuedGame → SetReservationCookie → m_nReservationCookie = cookie
```

**Важно**: `sub_10088500` — это НЕ единственная "reserve"-функция в привычном смысле, а универсальный **command-queue processor** ("EngineSystem") с несколькими типами команд (VEngineClient-резолв, ReserveServerForQueuedGame-вызов, INETSUPPORT_003-резолв). Раздел 6 ошибочно описывал его как "функцию, вызывающую ReserveServerForQueuedGame" — это верно только для одной из веток (case 2/3), сама функция гораздо шире. Это уточнение раздела 6, не противоречие.

### 28.2 Источник cookie — `sub_1008FD20` (ResolveReservationCookie) — **CONFIRMED** (полностью декомпилирован, 440 байт)

Два независимых источника, в зависимости от состояния reservation-struct:

1. **Основной путь (singleton, на весь процесс GC)**: `qword_1074B0A0` — резолвится ОДИН РАЗ через `sub_1008F840()` ("Derived SDR reservation cookie from identity: 0x%llx (steamid=%llu)") и дальше кэшируется/переиспользуется для всех резерваций. Концептуально аналогично нашему собственному фиксированному `GameServerCookieId` — только у project446 это ВЫЧИСЛЯЕМОЕ (из identity/SteamID), а не hardcoded значение.
2. **Альтернативный путь (per-reservation, "SDR rotate")**: если у reservation-struct установлен флаг `+260 & 0x40` И `+80` (QWORD, вероятно `encryption_key`) `> 1` — derive через `sub_1007C750(encryption_key)` (не декомпилирован полностью, тело не изучалось — **TODO**), при изменении логирует `"[GC] Reservation cookie changed %llx -> %llx (backend re-registered IP)"` и НЕМЕДЛЕННО шлёт `"G<cookie>,<cookie>,0:"` refresh через `sub_10088130` (это и есть путь, который в §6 назывался `"[GC] Reservation cookie changed..."`).

**MEDIUM CONFIDENCE**: `sub_1007C750` (encryption_key→cookie hash) не декомпилирован — точный алгоритм неизвестен, но не критичен для нашей архитектуры (см. 28.7).

### 28.3 Ограничение по game_type — **CONFIRMED** (прямое чтение `sub_10094120`)

```c
result = *(unsigned __int8 *)(a1 + 40);   // game_type byte
if ( result == 8 || result == 10 )        // ТОЛЬКО Competitive(8) и Wingman(10)
{ ... строит Q-reservation ... }
return result;                            // иначе — no-op, функция ничего не делает
```
DangerZone(13) **НЕ обрабатывается этой конкретной функцией** несмотря на то, что раздел 6/7 указывает `{8,10,13}` как полную accept-матрицу project446 на GC-стороне. Это значит DangerZone либо обрабатывается отдельной сестринской функцией (не найдена в этом раунде), либо project446 сознательно не поддерживает native-reserve для него (fallback на что-то иное). **NOT PROVEN**, не критично для нашего Casual-теста (game_type=7, вне этой матрицы вообще — см. 28.7).

### 28.4 Точный формат payload — **CONFIRMED** (byte-exact, прямое чтение `sub_10094120` строка 95 и `sub_100946A0` строка 307)

```c
sub_1005F370(v45, 64, "Q%llx,%llx,%d:", cookie, matchid, accountCount);   // основная Q-резервация
sub_1005F370(&v19, 80, "G%llx,%llx,1:", cookie, matchid_or_cookie);       // G-refresh (cookie-only rotate)
```

⚠️ **Уточнение к §27/документированному формату**: третье поле после `<matchid_hex>,` в Q-варианте — это **не строго булев ranked-флаг `<0|1>`**, а **сырой account count** (`v30 = *(a1+8)`), который в реальности для валидных матчей всегда `>0`, то есть функционально ведёт себя как truthy-флаг при парсинге на движке (сравнение `!= 0` в `SetReservationCookie`), но семантически это количество, не boolean. Для G-варианта это поле захардкожено как литеральная `1`. Наша реализация должна использовать `1` (или реальный account count) — не полагаться на строгую `0/1`-семантику.

Строки `"Q%llx,%llx,%d:"` (`0x1065619c`) и `"G%llx,%llx,1:"` (`0x10656200`) — уникальные, единственные вхождения, xref подтверждён напрямую.

### 28.5 Account ID список — **CONFIRMED** (прямое чтение `sub_10094120`, строки 102-144)

- После заголовка `"Q<cookie>,<matchid>,<count>:"` добавляются bracket-токены `"[%x]"` (accountID в hex) или `"[0]"` для пустого слота.
- Список разбит на ДВЕ команды (team split): cap на команду = `5` для Competitive(8), `2` для Wingman(10) (`v4 = (result==10) ? 2 : 5`), либо `accountCount/2` если больше капа.
- Итоговый лог: `"[GC] native Q reserve: %s (%d CT + %d T, padded to %d+%d, ip=%s cookie=%llx)"` — явно называет их "CT"/"T" (команды CS), подтверждая что это игроки, разбитые по сторонам.
- Формат согласуется с `baseserver.cpp`'s `SetReservationCookie`, которая парсит `[%x]` токены как account ID (§27, `baseserver.cpp:4184-4189` — общий паттерн, caster ID в `{%x}` фигурных скобках отдельно НЕ встречен в этом срезе декомпиляции — **NOT PROVEN**, возможно не используется в native-Q пути или обрабатывается за пределами прочитанного участка функции).

### 28.6 `IVEngineServer::ReserveServerForQueuedGame` вызывается АСИНХРОННО через очередь, не напрямую — **CONFIRMED**

- Push (`sub_100882A0`/`sub_10087FB0`/`sub_10088130` — три варианта push в один и тот же 128-слотовый кольцевой буфер, различаются, по-видимому, только тем как строится/хранится command-объект) НЕ вызывает движок сразу — только кладёт команду в очередь.
- Реальный вызов происходит на следующем тике из `sub_10088500`, вызываемого из 3 разных call site (`sub_10095E10`, `sub_100F5D00`, `sub_100F66F0` — вероятно: периодический GameFrame-тик, явный flush, и/или shutdown path; не декомпилированы в этом раунде — **TODO**, не критично).
- При неуспехе — retry до 5 раз, 1000мс между попытками, затем drop с логом "Command failed after 5 retries".
- 9106 (`MatchmakingServerReservationResponse`) отправляется backend'у из `sub_1009B750` **независимо** от того, успел ли `sub_10088500` уже реально выполнить резервацию — это optimistic/fire-and-forget с точки зрения GC↔backend протокола; фактическая успешность резервации подтверждается только логом `"ReserveServerForQueuedGame(...) -> %d"`.

### 28.7 `ReportGCQueuedMatchStart` — НЕ часть reservation-setup цепочки — **CONFIRMED** (уточнение раздела 6/§8)

`sub_100BADE0`'s vtable-хук на `ReportGCQueuedMatchStart` (`IServerGameDLL`, слот 47 по умолчанию) декомпилирован в §6 и в этом раунде повторно проверен по вызывающим цепочкам — **не имеет никакой связи с sub_10088500/sub_10094120/sub_1009B750**. Это отдельный, более ПОЗДНИЙ сигнал: сервер сам (в `server.dll`) решает, что игроки готовы/валидированы, и уведомляет GC через этот hook — используется для драйва 9107/ready-сигнала, НЕ для инициации резервации. Резервация инициируется раньше и независимо, сразу по получении 9105. Порядок из §8 подтверждён:  `9105 → (сразу) ReserveServerForQueuedGame → 9106` ... позже отдельно ... `ReportGCQueuedMatchStart → 9107`.

### 28.8 Наш `csgo_gc/gc_server.cpp` и общая инфраструктура — **CONFIRMED** (прямая проверка, не RE)

- `grep IVEngineServer|VEngineServer|CreateInterface|ReserveServerForQueuedGame|ServerGameDLL` по `gc_server.cpp` → **0 совпадений**. У нас сейчас нет абсолютно никакого кода, обращающегося к `IVEngineServer`.
- Единственный существующий `CreateInterface`-хук в проекте — `Hk_CreateInterface` в `csgo_gc/steam_hook.cpp:878`, но он инлайн-хукает **`steamclient.dll`'s CreateInterface** и обрабатывает только `"SteamClient0XX"` версии (Steam API proxy layer, `proxy/steamclientproxyXXX.h`). Это архитектурно НЕ связано с `engine.dll`'s собственным экспортом `CreateInterface` — резолв `IVEngineServer` потребует **отдельного**, независимого вызова (`GetModuleHandleA("engine.dll")` → `GetProcAddress(.., "CreateInterface")` → пробовать версии `VEngineServer023`/`022`/`021`/`020`, аналогично паттерну project446's `sub_10087D70`), не пересекающегося с существующим Steam-хуком.
- Наш проект — ClientGC (`csgo_gc.dll`, инжектируется в `csgo.exe`), а не отдельный ServerGC-процесс, инжектируемый в `srcds`/`server.dll` — но `IVEngineServer` доступен из ЛЮБОГО модуля процесса, где загружен `engine.dll` (наш собственный тестовый dedicated server — тот же процесс `csgo.exe` — listen-server, `engine.dll` там же). Значит теоретически резолвить `IVEngineServer` можно из ТОГО ЖЕ `csgo_gc.dll`, что уже инжектирован, если тест ведётся через listen-server (не отдельный `srcds.exe`) — это соответствует текущей тестовой конфигурации (`matchmaking.test_server_address=127.0.0.1`, локальный listen-server).

### 28.9 Итоговые ответы на 8 вопросов пользователя

1. **Цепочка**: `ClientGC::On9105Received → BuildNativeQReservePayload("Q<cookie>,<matchid>,<count>:[accts]...") → EngineSystemQueue.Push(type=2) → EngineSystemTick → CreateInterface(engine.dll,"VEngineServerXXX") → vtable[149] → IVEngineServer::ReserveServerForQueuedGame(payload) → CBaseServer::ReserveServerForQueuedGame → SetReservationCookie → m_nReservationCookie`. — **CONFIRMED** (28.1).
2. **Источник cookie**: singleton, вычисленный один раз из identity/SteamID (`sub_1008F840`), ИЛИ per-reservation derive из `encryption_key` при SDR-rotate. — **CONFIRMED** источники, **TODO** точный алгоритм derive (28.2).
3. **Источник match ID**: поле `+32` в reservation-struct (заполняется из 9105), с fallback на сам cookie если 0. — **CONFIRMED** (28.1, 28.2).
4. **Формат payload**: `"Q<cookie_hex>,<matchid_hex>,<accountCount>:[acct1_hex][acct2_hex]...[0]..."`, разбит на 2 команды (team1/team2), капы 5(Competitive)/2(Wingman). — **CONFIRMED byte-exact** (28.4, 28.5).
5. **Account IDs**: все игроки обеих команд, hex, в bracket-токенах, `[0]` для пустых слотов; caster ID (`{%x}`) в этом срезе не встречен. — **CONFIRMED** список, **NOT PROVEN** caster-поле (28.5).
6. **`bReserve` значения**: у project446 нет отдельного bool-параметра "bReserve" — есть только payload-строка целиком, где ведущий символ `'Q'`/`'G'` сам определяет режим (Q=полная резервация, G=только cookie-refresh). Отдельного bReserve-аргумента в сигнатуре `ReserveServerForQueuedGame(char const*)` нет — сигнатура подтверждена (§27, `eiface.h:525`) как принимающая ТОЛЬКО `const char*`. — **CONFIRMED**.
7. **Когда вызывается**: сразу по получении 9105 (до какого-либо accept/ready-сигнала), асинхронно через command-queue с retry. НЕ связано с `ReportGCQueuedMatchStart`. — **CONFIRMED** (28.6, 28.7).
8. **Какой бинарник/класс должен это реализовывать у нас**: наш `ClientGC` (`csgo_gc.dll`, уже инжектирован в `csgo.exe`) МОЖЕТ резолвить `IVEngineServer` напрямую в том же процессе при условии, что тестовый сервер — listen-server в том же `csgo.exe` (текущая тестовая конфигурация это и есть). Отдельный `ServerGC`/инжект в `srcds.exe` НЕ обязателен для текущего теста. — **HIGH CONFIDENCE** (28.8, зависит от текущей тестовой топологии, не проверено на реальном отдельном dedicated-сервере).

### 28.10 Минимальный объём будущих изменений (без кода, только план)

Если/когда пользователь одобрит переход к реализации:
1. Новый independent resolver: `GetModuleHandleA("engine.dll")` → `GetProcAddress(.., "CreateInterface")` → try `"VEngineServer023"`.."VEngineServer020"`, кэшировать указатель (аналог `sub_10087D70`, НЕ пересекается с `Hk_CreateInterface`).
2. Вызов vtable-слота, соответствующего `ReserveServerForQueuedGame` в eiface.h нашей версии engine (нужно посчитать точный слот для retail `engine.dll` 2021 — project446's смещение 596/149 получено на ИХ сборке, может отличаться на нашей; требует отдельной read-only проверки vtable retail `engine.dll` — **TODO**, не проверялось в этом раунде).
3. Построение payload-строки `"Q<cookie>,<matchid>,1:[accountid_hex]:"` — используя наш существующий `GameServerCookieId` как cookie (простейший вариант, без per-reservation rotation) и match_id из нашего собственного 9101/9107-контекста.
4. Вызов должен происходить в момент выполнения нашего кода — по месту, где сейчас стоит закомментированный/удалённый `TestMM::EnsureStarted()` в `OnMatchmakingStart` (или позже, при получении реального 9105-эквивалента, если у нас будет server-side relay) — **архитектурное решение, требует обсуждения с пользователем**, не решается только RE.
5. Никаких изменений `engine.dll`, никакого fake-responder, никакого UDP — полностью соответствует ограничениям, поставленным пользователем.

### Confidence levels §28

- **CONFIRMED**: полная цепочка 28.1, источники cookie 28.2 (кроме точного алгоритма derive), формат payload 28.4-28.5 (кроме caster-поля), асинхронность вызова 28.6, независимость `ReportGCQueuedMatchStart` 28.7, отсутствие `IVEngineServer`-кода в нашем `gc_server.cpp` 28.8, сигнатура `ReserveServerForQueuedGame` без `bReserve` 28.9.6.
- **HIGH CONFIDENCE**: наша топология (listen-server в том же процессе) позволяет резолвить `IVEngineServer` без отдельного ServerGC-инжекта (28.8/28.9.8) — логически обосновано, не протестировано вживую.
- **MEDIUM/TODO**: `sub_1007C750` (encryption_key→cookie hash) не декомпилирован; `sub_10087E50`/default-case очереди не декомпилированы; 3 call site тика `sub_10088500` не декомпилированы; caster ID (`{%x}`) поле не встречено в native-Q пути; точный vtable-слот `ReserveServerForQueuedGame` для НАШЕЙ retail-сборки `engine.dll` (не project446's) не проверен; game_type=13 (DangerZone) обработка не найдена в `sub_10094120`.

Никаких изменений исходников/сборки/копирования DLL/Git-операций в этом раунде не производилось — только чтение (`idalib` read-only декомпиляция, `grep` по своему коду).

---

## 29. Можем ли мы вызвать `IVEngineServer::ReserveServerForQueuedGame()` напрямую из нашего `csgo_gc.dll`? — read-only аудит retail `engine.dll` + наш код

Скрипты/выводы в scratchpad: `query_engine_vengineserver.py`, `query_cveengineserver_vtable.py`, `query_dump_vtable.py`, `query_vengineserver_impl_lines.py`, `query_slot148_deep.py`, `query_confirm_slot149.py`, `query_threadsafety_check.py` (все `_out.txt` рядом). База — тот же `engine.dll.i64` (retail, RVA-пространство подтверждено в §22-26/§27 как совпадающее с нашим реальным клиентом по DevMsg-строкам/форматам ответов).

### 29.1 Retail `engine.dll` — какие `VEngineServer0xx` реально зарегистрированы — **CONFIRMED**

Полнотекстовый поиск строк `'VEngineServer'`/`'VEngineClient'` по **всему** `engine.dll` дал:
```
0x10494d6c: 'VEngineServer023'          -- ЕДИНСТВЕННАЯ версия, зарегистрированная в CreateInterface-таблице
0x104650ac: 'VEngineClient014'          -- аналогично, единственная версия
0x105a9a9c: '.?AVCVEngineServer@@'      -- RTTI: класс-реализация
0x105a9abc: '.?AVIVEngineServer@@'      -- RTTI: чистый интерфейс
```
В отличие от `project446`, который перебирает **4 варианта** (`"VEngineServer021"`,`"022"`,`"023"`,`"020"`) на случай разных билдов движка, наш ретейл `engine.dll` реализует **ровно одну** версию — `VEngineServer023`. Это НЕ расхождение — `project446`'s код просто написан с запасом на другие движки/билды; для НАШЕГО процесса единственный рабочий вариант — `"VEngineServer023"`.

**Кросс-проверка версии через исходники**: `public/eiface.h:80` в уже использовавшемся все сессии source-дереве (`ida_deobfuscated_grok`) содержит `#define INTERFACEVERSION_VENGINESERVER "VEngineServer023"` — **точное текстовое совпадение** с тем, что реально зарегистрировано в бинарнике. Это независимое подтверждение (третье по счёту в этом проекте, после §25a/§26/§27), что данное source-дерево ABI-совместимо с нашим конкретным retail-билдом для этого интерфейса.

### 29.2 Точный vtable slot `ReserveServerForQueuedGame` в НАШЕМ retail `engine.dll` — **CONFIRMED** (прямая дизассемблирование + RTTI, НЕ по leaked source)

Метод восстановления: RTTI Complete Object Locator (`??_R4CVEngineServer@@6B@` @ `0x104f7e2c`) имеет единственный xref @ `0x10494e04` (дворд-слот "-4" перед началом vtable, стандартный MSVC RTTI паттерн) → **vtable `CVEngineServer` начинается с `0x10494e08`**.

Дамп и декомпиляция слотов 146-151 (offset 584-604):

| slot | offset | target | что это |
|---|---|---|---|
| 146 | 0x248 | `sub_101B6710` | не relevant (float-based, не тот метод) |
| 147 | 0x24c | `sub_101B5050` | `SendUserMessage`-подобный (не проверялось детально) |
| **148** | **0x250** | `sub_101B5090` | `EnsureInstanceBaseline` — **CONFIRMED** (вызывает `sub_101A94F0`, тело которого содержит буквальную строку `"SV_EnsureInstanceBaseline"`) |
| **149** | **0x254** | `sub_101B50F0` | **`ReserveServerForQueuedGame`** — **CONFIRMED** (см. ниже) |
| 150 | 0x258 | `sub_101B5130` | не relevant (memset+сетевой сбор данных, не тот метод) |
| 151 | 0x25c | `sub_101B52D0` | не relevant |

**Slot 149 (offset `0x254` = decimal `596`) — ТОЧНОЕ совпадение со слотом, который `project446`'s `sub_10088500` использует (§28.1)**. Байт-эксэктное ABI совпадает между двумя независимо скомпилированными GC (`project446` и наш retail `engine.dll`) — **никакого расхождения нет**.

Полная цепочка доказательств для slot 149:
```c
// vtable[149] thunk, raw disasm 0x101b50f0:
push ebp; mov ebp,esp; push ebx; push esi
mov esi, [ebp+arg_0]     // esi = второй арг (const char* payload)
push edi; push esi
mov edi, ecx             // ecx = this (стандартный __thiscall this)
call sub_101BFAA0        // sub_101BFAA0(this, payload) -> bool
mov bl, al
test bl,bl; jz skip
test esi,esi; jz skip
cmp byte ptr [esi], 52h  // 'R' -- если payload НЕ начинается с 'R'...
jz skip
mov eax,[edi]; mov ecx,edi
push "removeallids\n"
call dword ptr [eax+98h] // vtable+0x98=152=slot 38 -- ServerCommand
skip:
pop edi; pop esi; mov al,bl; pop ebx; pop ebp; retn 4
```
- Сигнатура: `bool __thiscall(void *this, const char *payload)` — **точное совпадение** с `eiface.h:525`: `virtual bool ReserveServerForQueuedGame( char const *szReservationPayload ) = 0;`.
- Побочный эффект вызывает `ServerCommand("removeallids\n")` через **vtable-слот 38** (`0x98`/4=38) — **точное совпадение** с независимой находкой `project446`'s `sub_10087D70`, который резолвит `IVEngineServer` и делает sanity-check именно через `"ServerCommand vtbl[%u]"` со значением **38**. Два независимых источника (наш retail-бинарник и чужой community-форк) сходятся на одном и том же слоте для ДВУХ РАЗНЫХ методов (149 и 38) — статистически исключает совпадение.

### 29.3 `sub_101BFAA0` = фактическое тело `CBaseServer::ReserveServerForQueuedGame` — **CONFIRMED** (byte-exact парсинг)

```c
bool __thiscall sub_101BFAA0(void *this, _BYTE *a2)   // a2 = payload string
{
  if (!a2) return 0;
  if (*a2 != 'G'(0x47) && *a2 != 'Q'(0x51))
  {
    if (*a2 == 'R'(0x52)) { sub_100A5FF0(this, a2+1, "%p", &unk_107A6D70); return 1; }  // 3-й режим 'R', НЕ документирован ранее -- NOT PROVEN что это, не нужен для нашего MVP
    return 0;
  }
  if (byte_138FFA40) { Warning("Rejecting reservation because sv_shutdown was requested.\n"); return 0; }  // новый факт: есть gate по sv_shutdown
  // парсинг "%llx,%llx,%d:" сразу после первого символа -- ТОЧНОЕ совпадение с project446's "Q%llx,%llx,%d:"/"G%llx,%llx,1:" (§28.4)
  sub_100A5FF0(this, a2+1, "%llx,%llx,%d:", &ArgList /* cookie, matchid, count */);
  if (!cookie || !matchid) return 0;
  qword_107A6DE0 = matchid;
  if (!sub_101A9480(&dword_107A6A78) || dword_107A6D68 != cookie_lo || dword_107A6D6C != cookie_hi)
      return ...;
  sub_101C0050();   // "commit" -- записывает cookie в dword_107A6D68/dword_107A6D6C (= m_nReservationCookie, 64-бит, разбит на 2 DWORD)
  return 1;
}
```
Это **прямое дизассемблирование retail-бинарника**, не leaked source — подтверждает и дополняет §27's source-based анализ:
- Формат `'G'/'Q'` + `"%llx,%llx,%d:"` — **byte-exact подтверждён в бинарнике**, полностью совпадает с `project446`'s builder-строками из §28.4.
- Найден **третий режим `'R'`** (0x52), парсящий `"%p"` (сырой указатель) вместо hex-пары — не документирован раньше нигде в этом проекте. **NOT PROVEN** что именно он делает (вероятно быстрый "rearm по кэшированному указателю", не относится к нашему MVP — мы используем `'G'`).
- Найден gate по `sv_shutdown` (`byte_138FFA40`) — новый факт, не критичен для теста (сервер не в процессе шатдауна).
- `m_nReservationCookie` хранится как **пара DWORD** (`dword_107A6D68`=lo, `dword_107A6D6C`=hi), сравнение и запись **без какой-либо блокировки/мьютекса** (см. 29.5) — прямое соответствие source-документированному `SetReservationCookie`.

### 29.4 Вызов `engine.dll!CreateInterface` из нашего инжектированного `csgo_gc.dll` — архитектурно и по бинарям — **CONFIRMED READY**

- `CreateInterface` — обычный **именованный экспорт** `engine.dll` (`0x102e4d70`), никакого специального контекста/потока для самого вызова не требуется — это плоский `GetProcAddress`+вызов, идентично тому, как наш существующий `Hk_CreateInterface` (`steam_hook.cpp:878`) уже успешно резолвит интерфейсы из `steamclient.dll` во время нормальной работы игры.
- Регистрация интерфейсов (`s_pInterfaceRegs`-таблица) заполняется статическими конструкторами **при загрузке `engine.dll`** — то есть к моменту, когда наш `csgo_gc.dll` (инжектированный ПОСЛЕ старта игры и получающий колбэки Steam API) вообще начинает исполняться, `engine.dll` уже полностью загружен и таблица интерфейсов уже заполнена. Никакой отдельной "engine initialization" с нашей стороны не требуется.
- Наш проект — `ClientGC`, инжектированный в `csgo.exe`. Текущая тестовая топология (`matchmaking.test_server_address=127.0.0.1`) — **listen-server внутри того же процесса `csgo.exe`**, то есть `engine.dll` в этом процессе одновременно обслуживает и клиентскую, и серверную (`sv`) стороны. `IVEngineServer` — глобальный интерфейс движка, доступный из ЛЮБОГО кода в процессе, где загружен `engine.dll`, независимо от того, что его вызывает клиентский или серверный код нашего DLL — **не нужен отдельный инжект в `srcds.exe`/отдельный `ServerGC`-процесс для ЭТОЙ тестовой конфигурации**.
- Отличие от отдельного `srcds.exe`: там `engine.dll` (точнее, `engine_srv.so`/`.dll`, линуксовый/дедовский вариант) грузится в ДРУГОМ процессе — потребовался бы отдельный инжект нашего DLL именно в `srcds.exe`, что сейчас НЕ является нашей тестовой топологией и не рассматривается для MVP.

### 29.5 Требуется ли специальный поток/контекст — **CONFIRMED BLOCKER** (главный результат этого аудита)

Проверка threading-модели нашего собственного `SharedGC` (`gc_shared.cpp:4-49`):
```cpp
void SharedGC::StartThread() { m_thread = std::thread{ &SharedGC::WorkerThread, this }; }
void SharedGC::WorkerThread() { ... HandleEvent(...) ... }  // цикл на ОТДЕЛЬНОМ std::thread
```
`ClientGC::OnMatchmakingStart` (как и вообще ЛЮБОЙ обработчик GC-сообщения) выполняется **на выделенном фоновом `std::thread`** (`SharedGC::WorkerThread`), НЕ на главном потоке игры/движка. Это подтверждено прямым чтением `gc_shared.cpp` — не предположение.

Проверка `sub_101BFAA0`/`sub_101A9480`/`sub_101C0050` (весь путь `ReserveServerForQueuedGame` → commit, §29.3) на предмет блокировок — **декомпилировано полностью, НИ ОДНОЙ блокировки/мьютекса не найдено**: `dword_107A6D68`/`dword_107A6D6C`/`qword_107A6DE0` читаются и пишутся напрямую, без `CThreadFastMutex`/`InterlockedX`. Для сравнения — СОСЕДНИЙ метод по vtable (slot 148, `EnsureInstanceBaseline`) **явно использует** `CThreadFastMutex::Lock`/`InterlockedCompareExchange` вокруг похожего паттерна — то есть отсутствие блокировки в slot 149 не случайность/недосмотр RE, а реальное свойство кода: `ReserveServerForQueuedGame` **спроектирован как single-threaded, вызываемый только с главного потока сервера** (стандартное допущение Source engine: вся игровая/серверная логика тикает на одном потоке).

**Вывод**: прямой вызов `ReserveServerForQueuedGame` из `OnMatchmakingStart` (как сейчас написан наш код) означал бы вызов из фонового `SharedGC::WorkerThread` — **гонка данных** с главным потоком, который параллельно читает/пишет те же глобалы движка во время обычного тика (обработка `A2S_RESERVE_CHECK`/сетевые пакеты и т.д., см. §22-26). Это **не гипотетическая, а подтверждённая архитектурная проблема** — именно поэтому у `project446` ЕСТЬ отдельный async command-queue (`sub_10088500`, тикаемый из `GameFrame`-хука, §28.6) вместо прямого вызова из места получения 9105 — это не стилистический выбор, а необходимость из-за той же самой thread-safety проблемы.

**У нашего проекта сейчас НЕТ никакой инфраструктуры main-thread dispatch** (`grep "GameFrame\|MainThread"` по всему `csgo_gc/` — 0 совпадений). Это недостающий кусок архитектуры, не просто "неизвестный факт" — но именно из-за этого раздел 5 (критерий готовности) не может быть безусловным "READY".

### 29.6 Payload для нашего Casual MVP — источник каждого значения — **CONFIRMED** (ничего не придумано)

| Поле | Значение | Источник |
|---|---|---|
| `cookie` | `GameServerCookieId` (`0x293A206F6C6C6548`, `gc_const_csgo.h`) | **CONFIRMED** — уже используется у нас в `gc_client.cpp:421,481` как `reservationid` и в `ClientRequestJoinServerData`/`MatchmakingGC2ClientReserve`, отправляемых клиенту. Клиент положит именно это значение в свой `A2S_RESERVE_CHECK` (§25/26/27) — сервер ДОЛЖЕН иметь `m_nReservationCookie == GameServerCookieId`, иначе проверка не пройдёт. Других источников cookie в проекте нет и не требуется — не рекомендуется копировать `project446`'s SDR-derived схему (§28.2), она сложнее и не нужна для локального теста с фиксированным cookie. |
| `match_id` | `GameServerCookieId` (тот же, как fallback) | **CONFIRMED источник паттерна** — `sub_10094120` (§28.1, project446) явно имеет fallback "`matchid==0` → `matchid=cookie`" при отсутствии реального match ID; у нас нет backend'а с реальным match ID (9105 не реализован), поэтому легитимно переиспользовать cookie как match_id для MVP — это не догадка, а задокументированное реальное поведение зрелого стороннего GC в ТОЙ ЖЕ ситуации (нет отдельного match ID). |
| `accountCount` / account-список | `1`, `[AccountId()]` | **CONFIRMED** — `CMsgGCCStrike15_v2_MatchmakingStart.account_ids` (`cstrike15_gcmessages.proto:292`, `repeated uint32`) уже приходит нам в `request.account_ids()` (видно в существующем логе `OnMatchmakingStart`, `request.account_ids_size()`); для соло-Casual там будет ровно 1 запись = SteamID32 локального игрока — либо взять это поле напрямую из `request`, либо использовать уже существующий `AccountId()` (`gc_client.h:53`, `m_steamId & 0xffffffff`) — оба должны совпадать для соло-теста. |
| пустые слоты | `[0]` | **CONFIRMED** формат из §28.5 (project446's `sub_10094120`: `else { v11="[0]"; }`), согласуется с парсингом `[%x]` в движке (§27, `baseserver.cpp`). |
| `mode` (ведущий символ) | `'G'` | **CONFIRMED** как валидный, отдельный от `'Q'` режим — оба явно проверяются в `sub_101BFAA0` (`*a2=='G'||*a2=='Q'`), с идентичной последующей обработкой (парсинг один и тот же). `'Q'` семантически используется `project446` для ПОЛНОЙ резервации нового матча (с team-split account-листом), `'G'` — для cookie-only refresh (§28.2/28.4). Для MVP с одним локальным игроком оба режима технически парсятся одинаково этим конкретным участком кода — **рекомендация: использовать `'Q'`**, т.к. он семантически соответствует "новая резервация", а не "refresh" (`'G'` в `project446` всегда шлётся ВСЛЕД за уже существующей `'Q'`-резервацией, не вместо неё) — это архитектурное решение, не RE-факт, требует подтверждения пользователем перед кодом. |
| Итоговый payload (предлагаемый, НЕ финализировано) | `"Q<GameServerCookieId_hex>,<GameServerCookieId_hex>,1:[<AccountId()_hex>][0]:"` | Собрано из подтверждённых частей выше; **точный терминатор после account-листа** (нужен ли финальный `:` / `{caster}` даже без кастера) — **NOT PROVEN до конца** (§28.5 отметил, что caster-поле `{%x}` не встречено в исследованном участке `sub_10094120`; сам `sub_100A5FF0`/парсер в движке НЕ декомпилирован до конца токенизации, только заголовок "%llx,%llx,%d:"). |
| Совпадёт ли cookie с `m_nReservationCookie` | Да | **CONFIRMED** напрямую в 29.3 — `sub_101C0050` пишет разобранный cookie в `dword_107A6D68`/`dword_107A6D6C`, что и есть (по §27's source-цитате) `m_nReservationCookie`. |

### 29.7 Наш `OnMatchmakingStart` — что есть, чего не хватает — **CONFIRMED** (прямое чтение `gc_client.cpp`)

Уже есть на момент 9101 (`gc_client.cpp:444-491`):
- `request.game_type()`, `eGame` — фильтр на Casual уже реализован.
- `request.account_ids()`/`account_ids_size()` — уже логируется (строка 458), не используется дальше пока.
- `GameServerCookieId` — уже используется как reservationid в 9107 (строка 481).
- `GetConfig().TestServerAddress()/TestServerPort()` — уже настроено на реальный dedicated server (127.0.0.1:27015 по умолчанию).
- `AccountId()` (`gc_client.h:53`) — доступен на любом методе `ClientGC`.

Чего НЕ хватает для вызова резервации (архитектурно, не RE):
1. Резолвер `IVEngineServer` (`GetModuleHandleA("engine.dll")`→`GetProcAddress(..,"CreateInterface")`→`"VEngineServer023"`), аналог `sub_10087D70` — **отсутствует полностью**, не спутать с существующим `Hk_CreateInterface` (тот хукает `steamclient.dll`, не резолвит новые интерфейсы из `engine.dll`).
2. Вызов vtable-слота 149 (bool(this,const char*)) — **отсутствует**.
3. **Механизм диспатча на главный поток** — **отсутствует полностью** (29.5) — это единственный ПОДТВЕРЖДЁННЫЙ блокирующий пробел, не просто "неизвестный факт".
4. Явное решение по match_id-заглушке и `'G'` vs `'Q'` — архитектурное, не RE (29.6, последняя строка).

### 29.8 Итоговые ответы на вопросы аудита

1. **ABI**: `IVEngineServer` = `VEngineServer023` (единственная версия в retail `engine.dll`), vtable slot **149** (offset `0x254`=596), сигнатура `bool(const char*)` — **CONFIRMED прямым дизассемблированием + RTTI**, НЕ только по leaked source, и byte-exact совпадает с независимой находкой `project446`.
2. **Вызов из нашего DLL**: `CreateInterface` резолвится обычным экспортом, без спецконтекста, движок уже инициализирован к моменту работы нашего кода, listen-server топология делает отдельный `srcds`-инжект ненужным — **CONFIRMED READY** архитектурно.
   НО: **вызов должен происходить с главного потока** — у нас сейчас GC работает на отдельном `std::thread`, и сама функция `ReserveServerForQueuedGame` не имеет внутренней синхронизации — **CONFIRMED BLOCKER**, требует нового кода (main-thread dispatch), которого пока нет.
3. **Payload**: все компоненты (cookie, account-список, формат `Q/G<hex>,<hex>,<d>:[...]`) — **CONFIRMED источники**, кроме точного терминатора после account-листа (**NOT PROVEN**) и выбора `'Q'` vs `'G'` (архитектурное решение, не факт).
4. **Наш GC**: все нужные входные данные (`account_ids`, `GameServerCookieId`, тестовый адрес сервера) уже присутствуют в `OnMatchmakingStart`; не хватает резолвера интерфейса, самого вызова и — критично — main-thread dispatch механизма.

### 29.9 Критерий готовности

# **B — BLOCKED**

Не READY по одной конкретной, подтверждённой причине: **отсутствует механизм вызова `ReserveServerForQueuedGame` с главного потока движка**. ABI (интерфейс/версия/vtable-слот/сигнатура), формат payload и все необходимые входные данные (cookie, account id, тестовый сервер) — все **CONFIRMED** и готовы к использованию. Единственный блокер — архитектурный, не RE-пробел:

**Что конкретно нужно доисследовать/решить перед кодом**:
1. Как встроить main-thread dispatch в наш `ClientGC`, инжектированный в `csgo.exe` — нужен НОВЫЙ read-only RE раунд: найти подходящую, стабильную, часто вызываемую с главного потока функцию в `client.dll`/`engine.dll` для хука (аналог `project446`'s `IServerGameDLL::GameFrame`, slot 4 — но это серверный `IServerGameDLL`, а не движковый; альтернативно — клиентский `ClientDLL::Frame`/аналог, или существующий наш собственный клиентский message-pump хук, если такой уже есть — не проверялось в этой сессии). Либо это, либо подтвердить (RE или прямым тестом), что вызов с фонового потока НА САМОМ ДЕЛЕ безопасен в нашем конкретном односерверном/одноклиентском тестовом сценарии (риск гонки формально есть, но при отсутствии одновременного сетевого трафика к серверу в момент вызова вероятность практического наблюдаемого сбоя может быть низкой — это НЕ равно "безопасно", просто снижает вероятность проявления бага).
2. Финализировать `'Q'` vs `'G'` и точный терминатор payload — решение пользователя + при необходимости добить декомпиляцию `sub_100A5FF0` (парсер токенов после заголовка) для 100% уверенности в формате хвоста строки.

Всё остальное — ABI, payload-структура, источники данных — готово хоть сейчас; блокер узкий и конкретный, не "множество неизвестных".

Никаких изменений исходников/сборки/копирования DLL/Git-операций в этом раунде не производилось — только чтение (`idalib`-дизассемблирование retail `engine.dll`, чтение source-дерева, `grep` по своему коду).

---

## 30. Main-thread dispatch для `ReserveServerForQueuedGame` — найден готовый существующий механизм

Read-only продолжение §29 (единственный найденный там блокер). Скрипты/выводы: `query_p446_tick_chain.py`, `query_runcallbacks_caller.py`, `query_runcallbacks_trace.py` (соответствующие `_out.txt` в scratchpad).

### 30.0 Главный вывод

**Отдельный новый хук в `client.dll`/`engine.dll` НЕ нужен.** У нашего проекта уже есть готовая, работающая, main-thread-гарантированная точка диспатча — `Hk_SteamAPI_RunCallbacks`/`Hk_SteamGameServer_RunCallbacks` (`csgo_gc/steam_hook.cpp:1052,1118`), которые уже перехватывают `SteamAPI_RunCallbacks()`/`SteamGameServer_RunCallbacks()` и уже дренируют межпоточную очередь `SharedGC::PostToHost`/`GetHostEvents`. Это ТОТ ЖЕ САМЫЙ паттерн, который независимо использует `project446` для вызова `ReserveServerForQueuedGame`.

### 30.1 `project446`: где именно `sub_10088500` вызывается относительно `GameFrame` — **CONFIRMED**

Три caller'а `sub_10088500` (EngineSystem queue processor, §28.1), полностью прослежены:

| caller | вызывается из | что это |
|---|---|---|
| `sub_10095E10` | **`sub_100BAD70`** (единственный caller) | Это САМ **GameFrame-хук-трамплин** — тело хука, поставленного в `sub_100BADE0` (§6) на `IServerGameDLL::GameFrame`, **vtable slot 4, хук #1, безусловный** ("Хук #1: GameFrame... безусловно"). |
| `sub_100F5D00` | callers не найдены в этом раунде (0 xrefs) | возможно entry point/экспорт, не тиковая функция — не критично |
| `sub_100F66F0` | **это и есть тело `Hk_SteamGameServer_RunCallbacks`** | Подтверждено буквальной строкой внутри функции: `"[Hook] Hk_SteamGameServer_RunCallbacks: FIRST CALL confirmed\n"` (`byte_1074C41C`-guarded, логируется один раз при первом срабатывании хука). Первая содержательная строка тела — **прямой вызов `sub_10088500(v9)`** (см. код ниже). |

```c
// sub_100F66F0 == тело Hk_SteamGameServer_RunCallbacks project446
int sub_100F66F0()
{
  if (!byte_1074C41C) { byte_1074C41C = 1; log("[Hook] Hk_SteamGameServer_RunCallbacks: FIRST CALL confirmed\n"); }
  dword_1074B9A8();
  sub_10215360();
  sub_100E6DA0();
  result = sub_10088500(v9);   // <-- ТИК EngineSystem-очереди (включая ReserveServerForQueuedGame) ПРЯМО ЗДЕСЬ
  ... (остальное -- не относится к резервации, обработка входящих GC/WS событий)
}
```

**Вывод**: `project446` тикает свою reservation-очередь ДВАЖДЫ избыточно — и из `IServerGameDLL::GameFrame`-хука (`sub_10095E10`←`sub_100BAD70`), и напрямую из `Hk_SteamGameServer_RunCallbacks` (`sub_100F66F0`). Это не архитектурная случайность, а сознательная избыточность ("что сработает раньше/чаще — то и продренирует очередь"). **`Hk_SteamGameServer_RunCallbacks` — САМ ПО СЕБЕ уже достаточная и рабочая точка** — `project446` явно на неё полагается напрямую, без завязки только на `GameFrame`.

### 30.2 Гарантированно ли `SteamAPI_RunCallbacks`/`SteamGameServer_RunCallbacks` вызываются с главного потока — **CONFIRMED прямым RE** (не только по документации Valve)

Полнотекстовый поиск по retail `engine.dll` (не по leaked source):
```
SteamAPI_RunCallbacks       (import) -- callers: sub_10219560, sub_1021BEE0
SteamGameServer_RunCallbacks (import) -- callers: sub_101A8AE0, sub_10213490
```

- **`sub_10219560`** содержит VProf-скоуп с буквальным именем `"CSteam3Client::RunFrame"` (`CVProfile::EnterScope(..., "CSteam3Client::RunFrame", ..., "Steam", ...)`) — прямое RE-подтверждение имени метода. Вызывает `SteamAPI_RunCallbacks()` безусловно каждый вызов. Единственный caller — `sub_1021A730` (offset `0x1021ad3d`).
- **`sub_10213490`** содержит VProf-скоуп `"CHLTVServer::RunFrame"` — вызывает `SteamGameServer_RunCallbacks()` условно (rate-limited раз в `0.1` сек через `qword_138FFA48`-таймстамп, и только если `dword_107A6A7C < 2`). Тот же caller — `sub_1021A730` (offset `0x1021ae33`).
- **Оба (`CSteam3Client::RunFrame` и `CHLTVServer::RunFrame`) вызываются из ОДНОЙ И ТОЙ ЖЕ родительской функции `sub_1021A730`** — что означает: они гарантированно выполняются на **одном и том же стеке вызовов, на одном и том же потоке, в рамках одного и того же engine-кадра**. `sub_1021A730` — по контексту (единая точка, откуда исходят и клиентский, и HLTV-серверный per-frame тик) — практически наверняка это верхнеуровневая функция главного цикла движка (`Host_RunFrame`/`CEngine::Frame`-эквивалент в этой сборке; точное имя не восстановлено — не критично, поведение важнее имени).
- `sub_101A8AE0` (второй caller `SteamGameServer_RunCallbacks`) вызывается из `sub_10219330` — отдельная ветка, не декомпилирована до конца в этом раунде (не критично, т.к. `sub_10213490`'s путь уже даёт достаточное доказательство).

**Итог**: это **прямое RE-подтверждение**, не просто ссылка на документацию Steamworks API ("вызывайте `SteamAPI_RunCallbacks` из потока рендера") — обе функции реально находятся внутри main-engine-frame функций одного и того же кадра движка.

### 30.3 Наш собственный `csgo_gc.dll` — что уже есть — **CONFIRMED** (прямое чтение `steam_hook.cpp`)

- **`Hk_CreateInterface`** (`steam_hook.cpp:878`) — хукает `steamclient.dll`'s `CreateInterface`, служит только для подмены `SteamClient0XX` интерфейсов Steam API. Не относится к main-thread диспатчу, не трогать/не путать (уже отмечено в §28.8).
- **`Hk_SteamAPI_RunCallbacks`** (`steam_hook.cpp:1052-1116`) — УЖЕ хукнут через funchook (`INLINE_HOOK(SteamAPI_RunCallbacks)`, `:1347`), УЖЕ дренирует `s_clientGC->m_gc.GetHostEvents(events)` каждый вызов (т.е. каждый engine-кадр). Связан с `s_clientGC` (`ClientGC`) — той же GC-инстанцией, чей `OnMatchmakingStart` мы правим.
- **`Hk_SteamGameServer_RunCallbacks`** (`steam_hook.cpp:1118-1170`) — УЖЕ хукнут (`INLINE_HOOK(SteamGameServer_RunCallbacks)`, `:1350`), УЖЕ дренирует `s_serverGC->m_gc.GetHostEvents(events)`. Связан с `s_serverGC` (`ServerGC`) — отдельной GC-инстанцией для серверной стороны listen-сервера.
- **Никакого существующего hook/callback в `client.dll`/`engine.dll` (помимо перехвата `steamclient.dll` и двух `RunCallbacks`-функций) у нас нет** — `grep "GameFrame\|FrameStageNotify\|CThreadedJob\|CJobMgr"` по `csgo_gc/` — 0 совпадений (кроме уже найденного). Новый engine/client-хук **не требуется** — искомый механизм уже есть.
- Межпоточная очередь `SharedGC::PostToHost` → `GetHostEvents` (`gc_shared.cpp:51-79`, mutex-protected `std::vector<EventData>`) — уже реализована и уже используется для `HostEvent::Message`/`NetMessage`/`MicroTransactionResponse`. Это ГОТОВЫЙ command-queue примитив, эквивалент `project446`'s `sub_10088500`'s кольцевого буфера, просто под другим именем.

### 30.4 Топология: `ClientGC` vs `ServerGC` — какой хук использовать — **HIGH CONFIDENCE** (архитектурный вывод, не финальное решение)

- `OnMatchmakingStart` — метод `ClientGC` (`gc_client.cpp`), выполняется на `WorkerThread` ЭКЗЕМПЛЯРА `s_clientGC`.
- `s_clientGC` и `s_serverGC` создаются НЕЗАВИСИМО, по признаку `pipe == s_serverSteamPipe` (`steam_hook.cpp:190-206`) — для listen-server топологии (наша тестовая конфигурация) **оба существуют одновременно** в одном процессе/DLL.
- По §30.2, `Hk_SteamAPI_RunCallbacks` (client) и `Hk_SteamGameServer_RunCallbacks` (server) оба гарантированно вызываются с одного и того же главного потока движка (общий родитель `sub_1021A730`) — то есть **для нашей listen-server топологии оба варианта одинаково потокобезопасны** для вызова `ReserveServerForQueuedGame`.
- Архитектурно более уместен `Hk_SteamGameServer_RunCallbacks` (серверный хук, серверный API) — именно его выбрал `project446` (§30.1). Но `OnMatchmakingStart` физически находится в `ClientGC`, а не `ServerGC` — потребуется либо (а) передать команду из `ClientGC`'s очереди в `ServerGC`'s очередь (два globals `s_clientGC`/`s_serverGC` в одном файле уже взаимно видны, это не RE-вопрос, а вопрос дизайна кода), либо (б) вызвать резервацию прямо из `Hk_SteamAPI_RunCallbacks`, что тоже потокобезопасно по §30.2, просто семантически "с клиентской стороны". **Это архитектурное решение для этапа реализации, не блокирующий RE-вопрос** — оба пути технически исправны.

### 30.5 Ответ на пункт 4 задания

# **A — READY**

- **DLL**: не требуется новый хук ни в `client.dll`, ни в `engine.dll` — используется уже существующий, уже хукнутый экспорт `steamclient.dll`'s `SteamAPI_RunCallbacks`/`SteamGameServer_RunCallbacks`.
- **Функция**: `Hk_SteamAPI_RunCallbacks` и/или `Hk_SteamGameServer_RunCallbacks`.
- **Расположение**: `csgo_gc/steam_hook.cpp:1052` и `:1118` (наш проект, уже существует, уже установлен через `funchook`/`INLINE_HOOK` в `:1347-1350`).
- **RVA (retail `engine.dll`, откуда реально идёт вызов на главном потоке)**: `sub_10219560` (`CSteam3Client::RunFrame`, RVA `0x10219560`) вызывает `SteamAPI_RunCallbacks` в `0x10219586`(≈); `sub_10213490` (`CHLTVServer::RunFrame`, RVA `0x10213490`) вызывает `SteamGameServer_RunCallbacks` в `0x102135fa`. Общий родитель обеих — `sub_1021A730`.
- **Почему гарантированно main-thread**: прямое RE — обе `RunFrame`-функции вызываются из одной родительской функции `sub_1021A730` в одном и том же engine-кадре (§30.2); дополнительно подтверждено независимо тем, что `project446` полагается ИМЕННО на `Hk_SteamGameServer_RunCallbacks` для того же самого вызова (§30.1).
- **Как `WorkerThread` передаст команду**: уже существующий примитив `SharedGC::PostToHost(HostEvent, id, data, size)` → `GetHostEvents()` (`gc_shared.cpp`) — `ClientGC::OnMatchmakingStart` (на `WorkerThread`) вызовет `PostToHost` с новым типом события (например, `HostEvent::ReserveServerForQueuedGame`, несущим payload-строку), а `Hk_SteamAPI_RunCallbacks`/`Hk_SteamGameServer_RunCallbacks` (уже дренирующие эту же очередь каждый кадр) добавят новый `case`, вызывающий резолвленный `IVEngineServer::ReserveServerForQueuedGame(payload)` (§29) синхронно на главном потоке. Никакого нового scheduler'а/потока/таймера не требуется — ни со стороны движка, ни со стороны нашего кода.

### Confidence levels §30

- **CONFIRMED**: `project446` тикает reservation-очередь и из `GameFrame`-хука, и напрямую из `Hk_SteamGameServer_RunCallbacks` (30.1); `SteamAPI_RunCallbacks`/`SteamGameServer_RunCallbacks` вызываются из per-frame client/HLTV-server tick функций с общим родителем в retail `engine.dll` (30.2, прямое RE); наши собственные `Hk_SteamAPI_RunCallbacks`/`Hk_SteamGameServer_RunCallbacks` уже существуют, уже хукнуты, уже дренируют готовую межпоточную очередь (30.3).
- **HIGH CONFIDENCE**: `sub_1021A730` — главная per-frame функция движка (по поведению/контексту, точное имя не восстановлено); выбор между client-хуком и server-хуком для конкретно НАШЕЙ листен-серверной топологии — оба технически потокобезопасны, финальный выбор — вопрос дизайна кода, не RE (30.4).
- **TODO/не критично**: точное имя `sub_1021A730` не восстановлено (RTTI/VProf-строка не найдена для НЕЁ САМОЙ, только для её "детей"); второй caller `SteamGameServer_RunCallbacks` (`sub_101A8AE0`←`sub_10219330`) не декомпилирован до конца — не нужно, т.к. `sub_10213490`'s путь уже даёт достаточное доказательство; `sub_100F5D00` (второй из трёх caller'ов `sub_10088500` в project446) не имеет найденных caller'ов в этом раунде — не критично, вывод не зависит от него.

Никаких изменений исходников/сборки/копирования DLL/Git-операций в этом раунде не производилось — только чтение (`idalib`-дизассемблирование retail `engine.dll` и `p446_csgo_gc.dll`, чтение своего `steam_hook.cpp`/`gc_shared.cpp`, `grep` по своему коду).

---

## 31. Реализация: минимальный server-side reservation bridge (первый код после §26-30)

Первая имплементация после серии read-only аудитов. Собрано и проверено, **живой тест не запускался** (по инструкции пользователя). DLL в игру не копировалась (только в проектный `Build/release/`, как и раньше — это существующее поведение `CMakeLists.txt`'s `OUTDIR`, не игровая папка). Git не трогался.

### 31.1 Изменённые файлы

| файл | что изменено |
|---|---|
| `csgo_gc/gc_shared.h` | новый `HostEvent::ReserveServerForQueuedGame` в существующем enum |
| `csgo_gc/gc_client.cpp` | `BuildReservationPayload()` (новый helper) + вызов `PostToHost(HostEvent::ReserveServerForQueuedGame, ...)` в конце `OnMatchmakingStart` |
| `csgo_gc/steam_hook.cpp` | `ResolveVEngineServer()` + `DispatchReserveServerForQueuedGame()` (новые static-функции) + новый `case` в `Hk_SteamAPI_RunCallbacks`'s событийном цикле |
| `csgo_gc/platform.h` | новый `Platform::ResolveModuleInterface(moduleName, interfaceVersion)` |
| `csgo_gc/platform_windows.cpp` | реализация `ResolveModuleInterface` (`GetModuleHandleA`+`GetProcAddress("CreateInterface")`) |
| `csgo_gc/platform_unix.cpp` | заглушка `ResolveModuleInterface` (возвращает `nullptr` — платформа не поддерживается, комментарий поясняет почему) |

`test_mm.cpp`/`test_mm.h` — **не тронуты**, по-прежнему компилируются в DLL, по-прежнему не вызываются нигде (`TestMM::EnsureStarted` не вызывается).

### 31.2 WorkerThread → main-thread мост

`ClientGC::OnMatchmakingStart` (выполняется на `SharedGC::WorkerThread`, §29.5) теперь, после отправки существующего 9107 (`MatchmakingGC2ClientReserve`, логика/поля не менялись), строит payload-строку и кладёт её в уже существующую очередь:
```cpp
std::string reservationPayload = BuildReservationPayload(AccountId());
Platform::Print("[MM] Queueing server reservation on host thread\n");
PostToHost(HostEvent::ReserveServerForQueuedGame, 0,
    reservationPayload.data(), static_cast<uint32_t>(reservationPayload.size()));
```
`PostToHost`/`GetHostEvents` (`gc_shared.cpp`) — **не переписывались**, использованы как есть (mutex-protected `std::vector<EventData>`).

Дренаж происходит в уже существующем `Hk_SteamAPI_RunCallbacks` (`steam_hook.cpp`, подтверждённая main-thread точка, §30.2/30.5), новый `case` в уже существующем `switch` по `HostEvent`:
```cpp
case HostEvent::ReserveServerForQueuedGame:
    DispatchReserveServerForQueuedGame(event.buffer);
    break;
```
Выбран именно `Hk_SteamAPI_RunCallbacks` (не `Hk_SteamGameServer_RunCallbacks`), т.к. `OnMatchmakingStart` физически принадлежит `ClientGC`, чья очередь дренируется именно этим хуком (`s_clientGC->m_gc.GetHostEvents`) — минимальное изменение без межпоточной/меж-инстансной передачи между `ClientGC` и `ServerGC`. §30.4 подтвердил, что оба хука одинаково потокобезопасны для нашей listen-server топологии — выбор `Hk_SteamAPI_RunCallbacks` архитектурно оправдан минимальностью изменения, не был RE-необходимостью.

### 31.3 Резолв `VEngineServer023` и вызов slot 149

`Platform::ResolveModuleInterface("engine.dll", "VEngineServer023")` (новая кросс-платформенная функция) резолвится **один раз** (статический кэш в `ResolveVEngineServer()`, `steam_hook.cpp`) через `GetModuleHandleA`+`GetProcAddress("CreateInterface")` — реализация только в `platform_windows.cpp` (использует `windows.h`, уже безопасно изолирован там же, где и весь остальной Win32-код проекта); `platform_unix.cpp` возвращает `nullptr`.

⚠️ **Найденная и исправленная в процессе проблема (не RE, инженерная)**: первая попытка добавить `#include <windows.h>` прямо в `steam_hook.cpp` сломала сборку — `windows.h`'s `SendMessage` макро (из `winuser.h`) переименовывает каждое вхождение идентификатора `SendMessage` в `SendMessageA`, а `steam_hook.cpp` и Steam SDK's `proxy/steamclientproxy0XX.h` **буквально содержат метод с именем `SendMessage`** (сама GC-прокси-логика). Это вызвало >100 синтаксических ошибок в `proxy/steamclientproxy010.h`/`011.h`. Исправлено переносом всей Win32-специфики (`GetModuleHandleA`/`GetProcAddress`) в `platform_windows.cpp` (уже безопасно включает `windows.h` изолированно, до/без Steam SDK заголовков) и вызовом через кросс-платформенный `Platform::ResolveModuleInterface`, так что `steam_hook.cpp` больше не включает `windows.h` вообще.

Вызов (только под `#ifdef _WIN32`, т.к. `__thiscall`-синтаксис не кросс-платформенный):
```cpp
using ReserveServerForQueuedGame_t = bool(__thiscall *)(void *, const char *);
void **vtable = *reinterpret_cast<void ***>(engineServer);
auto reserveFn = reinterpret_cast<ReserveServerForQueuedGame_t>(vtable[149]);
bool result = reserveFn(engineServer, payloadString.c_str());
```
Slot `149` (offset `0x254`/596) — тот самый, подтверждённый прямым дизассемблированием retail `engine.dll` в §29.2/29.3, без fallback на другие версии интерфейса (по инструкции — retail 2021 регистрирует только `VEngineServer023`, §29.1).

Диагностические логи — все три запрошенных добавлены дословно: `"[MM] Queueing server reservation on host thread"` (в `gc_client.cpp`), `"[MM] Calling IVEngineServer::ReserveServerForQueuedGame"` и `"[MM] ReserveServerForQueuedGame result: %d"` (в `steam_hook.cpp`), плюс `"[MM] VEngineServer023 not found"` (если резолв интерфейса не удался) и `"[MM] ReserveServerForQueuedGame failed"` (если вызов вернул `false`) — согласно п.6 задания, без fallback на другую версию интерфейса и без падения процесса в обоих случаях (функция просто `return`'ится).

### 31.4 Payload

`BuildReservationPayload(uint32_t localAccountId)` (`gc_client.cpp`) строит:
```
"G%llx,%llx,%u:[%x]"  →  G<GameServerCookieId_hex>,<GameServerCookieId_hex>,1:[<AccountId()_hex>]
```
- `'G'` — по явному указанию пользователя в задании (оба `'G'`/`'Q'` парсятся идентично в `sub_101BFAA0`, §29.3, так что это безопасный выбор согласно RE).
- cookie = `GameServerCookieId` — тот же существующий проектный константный cookie, уже используемый в `MatchmakingGC2ClientReserve.reservationid` (не менялось) и `ClientRequestJoinServerData.reservationid` (не менялось) — клиент получит тот же cookie, что сервер теперь установит.
- match_id = тот же `GameServerCookieId` (fallback, §28.2 — нет реального match id без server-side 9105 relay, тот же паттерн что у `project446`).
- accountCount = `1`, единственный account = `AccountId()` (тот же `m_steamId & 0xffffffff`, уже используемый в `SendRankUpdate`/`BuildMatchmakingHello`).
- Никаких дополнительных полей (team-split, caster) не добавлено — не требуется для одного локального игрока, не запрашивалось.

### 31.5 9107 (`MatchmakingGC2ClientReserve`) — не тронут

`reservationid`/`server_address`/`direct_udp_ip`/`direct_udp_port`/`map` — все поля и их построение оставлены как есть (см. неизменённый код перед новым блоком в `OnMatchmakingStart`). Reservation bridge — чисто дополнительный шаг ПОСЛЕ существующей отправки 9107, ничего в нём не заменяет.

### 31.6 Scope

Casual-фильтр (`eGame != 7` → ранний `return`) не менялся — reservation bridge физически недостижим для других game type, т.к. стоит после уже существующей проверки. Panorama/UI не трогались (никакие файлы вне `csgo_gc/` не редактировались).

### 31.7 Результат сборки

`build_local.bat` (ninja, MSVC x86, `x86-windows-static` triplet) — **0 errors, 0 новых warnings** (только предсуществующие `C4702: unreachable code` в vcpkg's `google/protobuf/repeated_ptr_field.h`, third-party, `-external:W0`, не связаны с этими изменениями). `test_mm.cpp.obj` компилируется (шаг `[16/26]`), в вызовы не включён. `csgo_gc.dll` собран, скопирован CMake-ом в проектный `Build/release/csgo_gc/` (не в игровую папку). Git не трогался (`git status --short` показывает только working-tree правки, 0 коммитов, ветка `experimental-native-mm`).

Живой тест **не запускался** — по явной инструкции пользователя, ждём отдельного разрешения.

---

## 32. Live Test #3 (раздельная топология) + read-only исследование reservation для отдельного `srcds.exe`

### 32.1 Live Test #3 — краткий итог (для связности; полная диагностика была дана в чате, не отдельным разделом)

Топология: `csgo.exe` (клиент, наш DLL) на одной машине, отдельно запущенный `srcds.exe` на `192.168.1.150:27016`. Лог (`csgo/console.log`, байт-в-байт сверен по времени с тестом) показал:
```
[GC] [MM] Calling IVEngineServer::ReserveServerForQueuedGame
-> Reservation cookie 293a206f6c6c6548:  reason ReserveServerForQueuedGame: G293a206f6c6c6548,293a206f6c6c6548,1:[3e9846d5]
[GC] [MM] ReserveServerForQueuedGame result: 1
removeallids:  filter removed for 0 user IDs
```
**Вся цепочка §28-31 подтвердилась рабочей на 100%** — payload распарсен, `SetReservationCookie` вызван (движковый лог строка 2, не наш), `ServerCommand("removeallids\n")` сработал (строка 4, ровно тот побочный эффект, что декомпилирован в §29.2 для vtable slot 149). Результат `1` (успех) — но **на `engine.dll` клиентского процесса**, не на `srcds.exe`. `IVEngineServer` — интерфейс уровня процесса; отдельный `srcds.exe` (другой процесс, возможно другая машина) своим `m_nReservationCookie` не тронут → `A2S_RESERVE_CHECK` на 192.168.1.150:27016 не мог пройти cookie-сверку на стороне srcds → `MatchmakingStop` → "Failed to connect to the match". Это ROOT CAUSE, подтверждённый экспериментально, ровно тот риск, что был явно помечен HIGH CONFIDENCE/не-CONFIRMED в §28.8/§29.4.

### 32.2 Задача этого раунда — read-only, без изменений кода

Как зарезервировать САМ `srcds.exe` (не клиента) тем же самым, уже подтверждённым API.

### 32.3 ABI не меняется между `csgo.exe` и `srcds.exe` — **CONFIRMED** (не RE, а прямая проверка файлов)

```
C:\...\csgo legacy\bin\engine.dll   -- ОДИН файл, используется И csgo.exe, И srcds.exe (Windows)
C:\...\csgo legacy\csgo\bin\server.dll -- аналогично, один файл
```
На Windows (в отличие от Linux, где есть отдельные `engine.so`/`engine_client.so`/`engine_ds_client.so`) `engine.dll` и `server.dll` **общие и байт-в-байт идентичные** для listen- и dedicated-режимов — это стандартная Source-архитектура (один бинарник, режим переключается флагом на уровне `IDedicatedServerAPI`/`CBaseServer`, а не отдельной сборкой). Значит: **все RE-факты §29 (VEngineServer023, vtable slot 149/offset 596, сигнатура, поведение `sub_101BFAA0`, отсутствие блокировок) применимы к `srcds.exe` БЕЗ каких-либо изменений или повторного RE** — это тот же самый код по тем же самым адресам.

`CBaseServer::ReserveServerForQueuedGame`/`SetReservationCookie`/`ReplyReservationCheckRequest` (§26/§27) — все определены на **базовом классе** `CBaseServer` в `engine/baseserver.cpp`, не в каком-либо listen-only или dedicated-only подклассе/файле — источник кода общий для обоих режимов по построению движка, не только по факту сборки в один .dll.

### 32.4 У нашего проекта УЖЕ ЕСТЬ вся инфраструктура для инжекта в `srcds.exe` — **CONFIRMED** (прямое чтение проекта, не RE)

Это оказалось не новой задачей, а **уже наполовину реализованной частью проекта**, которая ни разу не задействовалась в этой сессии:

- `launcher/CMakeLists.txt:43`: `launcher_target(srcds ON)` — отдельная цель сборки `srcds.exe`-лаунчера с `DEDICATED`-флагом, существует с начала проекта (не наш код в этой сессии).
- `launcher/launcher_win.cpp`: при `DEDICATED` грузит `bin/dedicated.dll`'s `DedicatedMain` (реальный Valve dedicated-server entry point, файл `bin/dedicated.dll` подтверждён присутствующим в игровой директории) вместо `bin/launcher.dll`'s `LauncherMain`. **В ОБОИХ режимах (`csgo`/`srcds`) грузится ОДИН И ТОТ ЖЕ `csgo_gc\csgo_gc.dll`** и вызывается `InstallGC(dedicated)` — `dedicated=true` для srcds-лаунчера, `false` для csgo-лаунчера (`launcher_win.cpp:172-186`).
- `csgo_gc/main.cpp` → `SteamHookInstall(dedicated)` (`steam_hook.cpp:1337`): при `dedicated=true` инициализирует `SteamGameServer_Init`/`SteamInternal_GameServer_Init` вместо `SteamAPI_Init` (`steam_hook.cpp:1271-1307`) — то есть **только серверный GC-pipe** (`s_serverSteamPipe`) активируется, что запускает конструктор `SteamGameCoordinatorProxy` с `m_server=true` → создаётся `s_serverGC` (`GCWrapper<ServerGC, NetworkingServer>`, `steam_hook.cpp:196`) — РОВНО ТОТ ЖЕ класс `ServerGC`, что уже существует в `gc_server.cpp`.
- `Hk_SteamGameServer_RunCallbacks` (`steam_hook.cpp:1118`) — уже хукнут, уже main-thread (§30.2 подтвердил прямым RE, что `SteamGameServer_RunCallbacks` вызывается из `CHLTVServer::RunFrame` в общем per-frame цикле движка — HLTV/gameserver-тик идёт в ЛЮБОМ режиме сервера, listen или dedicated, это часть обычного серверного тика, не listen-специфичная функция), уже дренирует `s_serverGC`'s host-event очередь тем же самым механизмом (`PostToHost`/`GetHostEvents`), что мы использовали для клиента в §31.
- **README.md** прямым текстом перечисляет "Dedicated server support" как уже существующую фичу проекта (строка 26) и явно предупреждает бэкапить `srcds.exe` перед использованием (строка 42) — это НЕ добавлено нами, это часть базового форка (mikkokko/csgo_gc).

**Вывод**: архитектурно всё уже готово для запуска `ServerGC` внутри отдельного `srcds.exe`; единственное, чего не хватает — самого reservation-кода внутри `gc_server.cpp` (по аналогии с тем, что мы добавили в `gc_client.cpp`/`steam_hook.cpp` для клиента в §31) и **фактического деплоя** нашего патченного `srcds.exe`+`csgo_gc.dll` НА МАШИНУ, где реально крутится `192.168.1.150:27016` (сейчас там, почти наверняка, стоковый непропатченный Valve `srcds.exe` — наш `srcds`-лаунчер ни разу не собирался в этом проекте, `build_local.bat` строит только `csgo_gc`/`csgo`-таргеты, не `srcds`).

### 32.5 Готовая точка интеграции в `ServerGC` — **CONFIRMED** (прямое чтение `gc_server.cpp`)

`ServerGC::HandleMessage` уже обрабатывает `k_EMsgGCServerHello → SendServerWelcome()` (`gc_server.cpp:61-63, 255-271`) — это надёжный, уже существующий, срабатывающий РОВНО ОДИН РАЗ при старте сигнал: "серверная игровая логика (`server.dll`) законнектилась к нашему фейковому GC". `SendServerWelcome()` уже кладёт `GameServerCookieId` в `CMsgCStrike15Welcome.gscookieid` (`gc_server.cpp:260`, `cstrike15_gcmessages.proto:879`, поле `optional uint64 gscookieid = 18`) — **интересное совпадение имени поля, но НЕ подтверждённый резервационный механизм** (нет доказательств, что `server.dll` использует это поле для `sv_mmqueue_reservation`/резервации; единственный подтверждённый способ установки `m_nReservationCookie` по-прежнему только `IVEngineServer::ReserveServerForQueuedGame()`, §27/29) — **NOT PROVEN**, не предлагаю полагаться на это поле.

Правильная минимальная точка — рядом с `SendServerWelcome()` (или сразу после неё, при первом же тике `Hk_SteamGameServer_RunCallbacks`): вызвать ТУ ЖЕ последовательность, что уже реализована для клиента в §31 — построить payload (`BuildReservationPayload`-подобная функция, тот же confirmed формат `"G<cookie_hex>,<cookie_hex>,1:[...]"`), `PostToHost(HostEvent::ReserveServerForQueuedGame, ...)`, и на main-thread стороне (`Hk_SteamGameServer_RunCallbacks`) добавить `case HostEvent::ReserveServerForQueuedGame: DispatchReserveServerForQueuedGame(event.buffer);` (сейчас там только `case Message`/`case NetMessage`, `default: assert(false)` — новый `case` придётся добавить). Поскольку у нас нет реального backend'а/9105 — сигнал "нужно резервировать" не приходит откуда-то извне, поэтому логично резервировать **безусловно, при старте сервера**, тем же фиксированным `GameServerCookieId`, что уже используется в 9107 клиентской стороной — сервер всегда готов принять ЛЮБОГО клиента с тем же фиксированным cookie, что соответствует духу всего проекта (нет динамического per-match cookie, есть один общий hardcoded).

### 32.6 `A2S_RESERVE_CHECK`/`0x25` на `srcds` — тот же самый код, что уже подтверждён в §22-27 — **CONFIRMED** (не требует нового RE)

`CBaseServer::ReplyReservationCheckRequest` (§26, полный текст запроса/ответа, `awaiting_clients`/`total_clients_in_reservation`) — часть общего сетевого стека `CBaseServer`, который слушает UDP независимо от режима хостинга. Единственное условие успеха (по всей нашей цепочке RE) — совпадение cookie в пришедшем `A2S_RESERVE_CHECK`-пакете с `m_nReservationCookie` сервера. Если `ReserveServerForQueuedGame` реально отработает на `srcds.exe` (что зависит только от деплоя, не от кода движка) — обработка `0x21`→`0x25` должна пройти идентично тому, что уже теоретически прослежено в §22-27 для листен-сервера, т.к. это один и тот же код.

### 32.7 Что уже есть / что нужно реализовать

| Компонент | Статус |
|---|---|
| ABI (`VEngineServer023`, slot 149, сигнатура) | **Готово**, применимо без изменений (§32.3) |
| Launcher-инжект в `srcds.exe` | **Готов в исходниках** (`launcher_target(srcds ON)`), но **ни разу не собирался** в этом проекте — TODO собрать `--target srcds` |
| `ServerGC`/dedicated-режим GC pipe | **Готово и уже работает** (`steam_hook.cpp` dedicated-ветки, не наш код) |
| Main-thread dispatch на сервере | **Готов** (`Hk_SteamGameServer_RunCallbacks`, тот же хук что и §30, но новый `case` для `ReserveServerForQueuedGame` ещё не добавлен) |
| Payload builder для сервера | **Нет** — нужна server-side версия `BuildReservationPayload` (переиспользовать тот же формат/cookie) |
| Точка триггера резервации | **Нет** — предлагается рядом с `SendServerWelcome()` |
| Деплой патченного `srcds.exe`+DLL на машину 192.168.1.150 | **Не сделано** — сейчас там почти наверняка стоковый Valve `srcds.exe` |

### 32.8 Минимальный план следующего патча (план, не реализация)

1. Собрать `srcds`-таргет лаунчера (`cmake --build ... --target srcds`) — сейчас не входит в `build_local.bat`.
2. В `gc_server.cpp`: добавить server-side построение того же payload-формата (переиспользуя `GameServerCookieId`) и вызов `PostToHost(HostEvent::ReserveServerForQueuedGame, ...)` — по аналогии с §31, без копирования клиентской логики 1-в-1 (нет `AccountId()`/`request.account_ids()` на сервере — нужно решить, что подставлять в account-список: либо `[0]`, либо вообще без него, раз он не строго обязателен даже для `'G'`-режима согласно §29 sanity-check).
3. В `steam_hook.cpp`: добавить `case HostEvent::ReserveServerForQueuedGame` в `Hk_SteamGameServer_RunCallbacks`'s switch, переиспользуя уже существующую `DispatchReserveServerForQueuedGame`/`ResolveVEngineServer` (эти функции уже cross-context safe, ничего в них менять не нужно).
4. Собрать, проверить 0 ошибок.
5. **Задеплоить** `srcds.exe` (с бэкапом оригинала, как предупреждает README) + `csgo_gc/csgo_gc.dll` + `csgo_gc/config.txt` НА МАШИНУ `192.168.1.150` — без этого шага код никак не попадёт в реально запущенный процесс, это не менее важно, чем сама реализация.
6. Live-тест с той же топологией.

Никаких изменений `gc_client.cpp`/`steam_hook.cpp`'s существующей клиентской логики/DLL/Git-операций в этом раунде не производилось — только чтение исходников проекта и файловой структуры игровой директории (не RE `engine.dll`, все нужные факты уже были получены в §22-30).

---

## 33. Реализация: server-side reservation для отдельного `srcds.exe` (по плану §32.8)

Клиентская reservation-логика (§28-31, `gc_client.cpp`, весь reservation-related код в `steam_hook.cpp` из §31) **не тронута** — подтверждено `git diff --stat` (изменения в `gc_client.cpp` — только старые, из §31, ничего нового в этом раунде).

### 33.1 Изменённые файлы

| файл | что изменено |
|---|---|
| `csgo_gc/gc_server.h` | новый приватный метод `ReserveServerForOurCookie()` |
| `csgo_gc/gc_server.cpp` | реализация `ReserveServerForOurCookie()` + вызов сразу после `SendServerWelcome()` в обработчике `k_EMsgGCServerHello` |
| `csgo_gc/steam_hook.cpp` | новый `case HostEvent::ReserveServerForQueuedGame` в `Hk_SteamGameServer_RunCallbacks`'s событийном цикле — переиспользует уже существующие `DispatchReserveServerForQueuedGame`/`ResolveVEngineServer` (§31), **ни строки дублирования** |

`gc_shared.h`, `platform.h/.cpp`, `test_mm.cpp/.h` — не тронуты (enum `HostEvent::ReserveServerForQueuedGame` и вся Win32-инфраструктура резолва интерфейса уже существовали с §31, переиспользованы как есть).

### 33.2 Синхронизация cookie клиент ↔ srcds

**Не добавлялся никакой новый config-параметр.** `GameServerCookieId` (`gc_const_csgo.h`) — compile-time константа (`constexpr uint64_t`), вкомпилированная в бинарник. `ClientGC` (`gc_client.cpp`) и `ServerGC` (`gc_server.cpp`) — это **один и тот же `csgo_gc.dll`**, просто загружаемый разными лаунчерами (`csgo`-лаунчер → `InstallGC(false)` → `ClientGC`; `srcds`-лаунчер → `InstallGC(true)` → `ServerGC`, см. §32.4). Значит, пока на клиентскую и серверную машину копируется **одна и та же сборка `csgo_gc.dll`**, cookie гарантированно идентичен побайтово — никакой ручной синхронизации/нового параметра не требуется. Это тот случай, что пользователь сам предвидел как более чистый вариант.

### 33.3 Payload — byte-exact проверка

```cpp
snprintf(buffer, sizeof(buffer), "G%llx,%llx,1:",
    static_cast<unsigned long long>(GameServerCookieId),
    static_cast<unsigned long long>(GameServerCookieId));
```
Для `GameServerCookieId = 0x293A206F6C6C6548` это даёт:
```
G293a206f6c6c6548,293a206f6c6c6548,1:
```
Сверено посимвольно с канонической `'G'`-формой из §27 (`baseserver.cpp`'s source-цитата, строка 1832: `"G<cookie_hex>,<matchid_hex>,1:"`, БЕЗ списка игроков) — **точное совпадение, без единого лишнего байта**. В отличие от клиентского payload (§31.4, который добавляет `[account_hex]` — единственный неподтверждённый нюанс, отмеченный в предыдущей sanity-check-сессии), серверный payload здесь — **чище**: на сервере нет реальных account-данных, поэтому мы используем МИНИМАЛЬНУЮ, безусловно подтверждённую документированную форму без каких-либо добавок. match_id = тот же cookie (тот же fallback-паттерн §28.2/§31.4, применённый уже дважды в проекте — согласованно).

### 33.4 ABI вызова `VEngineServer023` — не менялся, переиспользован как есть

`ResolveVEngineServer()`/`DispatchReserveServerForQueuedGame()` (`steam_hook.cpp`, добавлены в §31) — **ни одной изменённой строки** в этом раунде. Поскольку `bin/engine.dll` — один и тот же файл для `csgo.exe` и `srcds.exe` (§32.3, подтверждено файловой структурой игровой директории), vtable slot 149 / offset 596 / сигнатура `bool(__thiscall*)(void*, const char*)` остаются корректными без каких-либо условий/дополнительной проверки. Единственное отличие в вызове — источник события (`s_serverGC`'s очередь вместо `s_clientGC`'s), сам резолв интерфейса и вызов идентичны байт-в-байт тому, что уже подтверждено живым тестом в Live Test #3 (§32.1: `ReserveServerForQueuedGame result: 1`).

### 33.5 Точка триггера

`ServerGC::HandleMessage`, `case k_EMsgGCServerHello`:
```cpp
case k_EMsgGCServerHello:
    SendServerWelcome();
    ReserveServerForOurCookie();
    break;
```
Срабатывает один раз при подключении `server.dll`'s игровой логики к нашему (фейковому) GC — надёжный, ранний, не зависящий от наличия клиентов сигнал старта сервера, как и запрошено.

### 33.6 Результат сборки

- **`csgo_gc.dll`**: `build_local.bat` (полный прогон, включая `csgo`-лаунчер) — **0 errors, 0 новых warnings** (та же предсуществующая vcpkg protobuf-заметка, что и в §31, не связана с этими изменениями). Файл: `Build/release/csgo_gc/csgo_gc.dll`, пересобран.
- **`srcds.exe`**: собран отдельно (`cmake --build Build\build_ninja --target srcds`, ранее не входил в `build_local.bat`) — **0 errors** (`[3/3]` ninja-шагов, компиляция `launcher_win.cpp` с `-DDEDICATED`, линковка, `vcpkg z-applocal`, копирование в `Build/release/`). Файл: `Build/release/srcds.exe`.

Оба билда собраны в проектную `Build/release/`, **никуда в игровые директории не копировались** — по явному ограничению задания. Git не трогался.

### 33.7 Файлы для ручного деплоя

**Клиентская машина** (уже задеплоено ранее в этой сессии, не изменилось):
- `Build/release/csgo_gc/csgo_gc.dll` → `<client game dir>/csgo_gc/csgo_gc.dll` (нужно ПЕРЕЗАПИСАТЬ — содержит новый серверный код тоже, хоть он и не используется в `ClientGC`-режиме)
- `Build/release/csgo/csgo.exe` — не менялся, передеплой не обязателен

**Серверная машина** (`192.168.1.150`, НОВЫЙ деплой, ничего похожего там ещё нет):
- `Build/release/srcds.exe` → `<srcds install dir>/srcds.exe` (**сначала забэкапить оригинальный `srcds.exe`**, как явно предупреждает README — например переименовать в `srcds.exe.orig`)
- `Build/release/csgo_gc/csgo_gc.dll` → `<srcds install dir>/csgo_gc/csgo_gc.dll` (та же самая сборка, что и на клиенте — это и обеспечивает совпадение `GameServerCookieId`, §33.2)
- `csgo_gc/config.txt` на сервере не обязателен для этой конкретной фичи (мы не читаем `matchmaking.test_server_address/port` на серверной стороне), но если он отсутствует — `GCConfig` просто использует дефолты для всех остальных полей (`ranks`/`rarity_weights`/etc, не влияет на reservation).

После деплоя — следующий live-тест той же топологии (клиент + отдельный `srcds.exe` на 192.168.1.150:27016), ожидаем `ReserveServerForQueuedGame result: 1` теперь и в логе `srcds`, что должно позволить `A2S_RESERVE_CHECK`/`0x25` пройти успешно.

### 33.8 Аддендум: `SteamGameServer014` fallback (обнаружено при первом запуске патченного `srcds.exe`)

Патченный `srcds.exe` падал с `Could not get SteamGameServer014` — `GetSteamGameServer()` (`steam_hook.cpp`) пыталась только один, захардкоженный `STEAMGAMESERVER_INTERFACE_VERSION`, без фолбэка (в отличие от уже существующего клиентского паттерна `CHECK_STEAMCLIENT`/`SteamInterfaceProxy::GetInterface`, который перебирает несколько версий для `SteamClient`/`SteamUser`/etc.). В проекте уже были вендорены прокси-классы `SteamGameServerProxy010..014` (`proxy/steamgameserverproxy0XX.h`, уже `#include`-ились, просто не использовались для ЭТОГО резолва) — этого было достаточно, чтобы добавить аналогичный fallback без новых зависимостей.

**Важный нюанс, из-за которого наивный fallback был бы небезопасен**: сверил объявления `BLoggedOn()` во всех вендоренных `steam_old/isteamgameserver0XX.h` — это **9-й** virtual-метод (стабильный vtable slot) в версиях 011/012/013/014, но **3-й** в версии 010 (другой слот). Единственное место, где вообще используется `s_steamGameServer` (`Hk_SteamGameServer_RunCallbacks`, `s_steamGameServer->BLoggedOn()`) — поэтому наивно привести возвращённый `ISteamGameServer010*` к обычному `ISteamGameServer*` и звать `BLoggedOn()` было бы UB (вызов не того метода). Решение: `GetSteamGameServer()` теперь перебирает `"SteamGameServer014"→"013"→"012"→"011"→"010"` с логами (`"%s not available, trying %s\n"` / `"Using %s\n"`), запоминает флаг `s_steamGameServerIsV010`, а единственный вызов `BLoggedOn()` теперь ветвится на этот флаг (`reinterpret_cast<ISteamGameServer010*>` только для версии 010, обычный вызов для 011-014). Если ни одна версия не найдена — прежнее фатальное поведение (`Platform::Error`), но теперь текст говорит "tried 014 down to 010".

Файлы: только `csgo_gc/steam_hook.cpp` (та же логика инициализации, что уже была; клиентская reservation-логика и §31-33's reservation bridge не затронуты). Пересобрано (`build_local.bat`) — **0 errors, 0 новых warnings** (после того, как из `GetSteamGameServer` убран недостижимый `return nullptr;` после `Platform::Error` — тот `[[noreturn]]`, сам вызвал C4702 на первой попытке, исправлено).

---

## 34. Read-only разбор: почему второй/третий Casual matchmaking проваливается ("Failed to connect to the match")

Код НЕ менялся, сборка НЕ запускалась, Git не трогался. Единственный доступный источник — текущий `csgo/console.log` (2630 строк, 4 полных цикла `ClientGC spawned`/`ClientGC destroyed`, т.е. 4 перезапуска клиента, 6 попыток `MatchmakingStart` суммарно). **Логов первого успешного захода нет** — по словам пользователя они уже перезаписаны, и это подтверждается содержимым: даже САМАЯ ПЕРВАЯ попытка в этом файле (строка 606) уже проваливается тем же паттерном, что и все остальные — то есть успешный матч, о котором рассказал пользователь, произошёл ДО начала этого лог-файла. Логов `srcds` нет вообще (отдельная машина, недоступна из этой сессии) — все выводы про сторону `srcds` сделаны через код/§22-33, не через прямое чтение его лога, это явно помечено.

### 34.1 Полный разбор имеющегося `console.log` — **CONFIRMED** (прямое чтение)

| Сессия (`ClientGC spawned`→`destroyed`) | Попытки `MatchmakingStart` | Результат |
|---|---|---|
| #1, строки 468-625 | 1 (606) | `ReserveServerForQueuedGame result: 1` (наша, клиентская) → `MatchmakingStop` (615) → **нет `Connecting to...`** → попап (616) → выход |
| #2, строки 1098-1277 | 0 | matchmaking не запускался в этой сессии |
| #3, строки 1750-1989 | 3 (1881, 1961, 1972) | **Попытка #1 (1881-1930): `MatchmakingStop` (1895) → `Connecting to public(192.168.1.150:27016)` (1906) → `Connected to 192.168.1.150:27016` (1908) → загрузка `de_dust2`, `CSGO_GAME_UI_STATE_INGAME` (1923) → **буквально через несколько строк лога** `CSGO_GAME_UI_STATE_PAUSEMENU`→`MAINMENU` (1927-1930), popup `#GenericConfirmText_Label` не локализован (текст причины отключения не виден в логе). Попытки #2 (1961) и #3 (1972): `MatchmakingStop` → **нет `Connecting to...` вообще**, сразу попап/провал. |
| #4, строки 2462-2625 | 1 (2608) | Тот же провальный паттерн, что и #1: `MatchmakingStop` → нет `Connecting to...` |

**Ключевой факт из всего лога**: `"Connecting to public(192.168.1.150:27016)"`/`"Connected to 192.168.1.150:27016"` встречается **РОВНО ОДИН РАЗ** во всём файле (строки 1906-1908), из 6 суммарных попыток `MatchmakingStart`. Все остальные 5 попыток проваливаются, даже не доходя до строки `"Connecting to..."`.

**Важное уточнение, отменяющее прежнюю трактовку `MatchmakingStop`**: `k_EMsgGCCStrike15_v2_MatchmakingStop` появляется **во ВСЕХ 6 попытках без исключения** — включая ту единственную, что успешно законнектилась (строка 1895, ДО `Connecting to...` на 1906). Значит `MatchmakingStop` сам по себе **не является признаком провала** (согласуется с более ранним выводом §17/§20 про "стрэй" `MatchmakingStop`) — реальный индикатор успеха/провала это наличие или отсутствие последующего `Connecting to...`/`Connected to...`.

**Не подтверждено логом**: причина, по которой клиент, успешно законнектившись (1908) и загрузившись в карту (1923), почти сразу вернулся в главное меню (1927-1930) — `'#GenericConfirmText_Label'` не локализуется, реальный текст попапа не виден. `Disconnect`/`kick`/`ban`-строк в этом временном окне нет. Это **отдельная, самостоятельная проблема**, не смешивать с провалом повторного matchmaking — по всем признакам reservation-протокол (0x21/0x25/QueueConnect/connect) для ЭТОЙ конкретной попытки отработал полностью корректно.

### 34.2 Проверка кода: `ReserveServerForOurCookie()` вызывается ровно один раз за жизнь процесса `srcds` — **CONFIRMED** (прямое чтение `gc_server.cpp`, код не менялся с §33)

```cpp
// gc_server.cpp:59-64
switch (messageRead.TypeUnmasked())
{
case k_EMsgGCServerHello:
    SendServerWelcome();
    ReserveServerForOurCookie();   // <-- единственный вызов во всём проекте
    break;
```
Полнотекстовый поиск `ReserveServerForOurCookie` по `csgo_gc/` — **ровно 2 совпадения**: объявление (`gc_server.h`) и это единственное место вызова. Никакого таймера, периодического тика, повторного вызова при новом клиенте/матче/дисконнекте — не существует.

`k_EMsgGCServerHello` — это **однократное** сообщение (GC-логин `server.dll`'s игровой логики к нашему фейковому GC), отправляемое ровно один раз при старте `srcds`-процесса — это не per-match и не per-client событие (для сравнения: `SendServerWelcome()` также имеет собственный **fallback**-путь по требованию, `gc_server.cpp:236-241`, `if (!m_sentWelcome) SendServerWelcome();` — но ЭТОТ путь `ReserveServerForOurCookie()` НЕ вызывает вообще, ещё один потенциальный источник асимметрии, если `ServerHello` почему-то не придёт, а `SendServerWelcome()` сработает только через fallback).

**Вывод по коду**: реализация из §33 архитектурно резервирует сервер **один раз за весь жизненный цикл процесса `srcds.exe`**, не один раз за матч. Это прямое, не гипотетическое следствие текущего кода.

### 34.3 Что происходит с `m_nReservationCookie` после первого коннекта — **NOT PROVEN точный механизм, но исход подтверждён косвенно**

У нас нет доступа к логам `srcds`, поэтому ТОЧНЫЙ механизм очистки/инвалидации (`SetReservationCookie` перезаписывается на 0? `IsReserved()` становится `false` по `m_flReservationExpiryTime` истечению `sv_mmqueue_reservation_timeout`? резервация "расходуется" при фактическом коннекте?) **не подтверждён напрямую** — это потребовало бы либо логов `srcds`, либо RE успешного пути `ReplyReservationCheckRequest`/пост-коннект кода в `engine.dll`, что не делалось в этом раунде (пользователь явно запретил менять/собирать код, RE тоже не запрашивалось в этот раз).

Но **исход** — что после ПЕРВОГО успешного коннекта резервация перестаёт приниматься — подтверждён косвенно, но убедительно: (а) из 6 попыток ровно одна успешно законнектилась, все остальные (и до, и после неё) — нет; (б) §33.5-33.6 подтверждают точку установки cookie исчерпывающе одноразовая; (в) архитектурно источники §27 (`m_flReservationExpiryTime`, `sv_mmqueue_reservation_timeout`) прямо указывают, что резервация в CS:GO **не задумана как перманентное состояние** — она либо истекает по таймауту, либо (по общей логике "очередь на один заход") расходуется по факту использования. Оба варианта одинаково ломаются нашей текущей реализацией, потому что она никогда не переустанавливает cookie повторно.

### 34.4 Client-side lifecycle после дисконнекта — **CONFIRMED** (что есть в логе) / **NOT APPLICABLE** (что не нашлось)

- `k_EMsgGCCStrike15_v2_MatchmakingGC2ClientAbandon` — **0 вхождений** во всём файле. Ожидаемо: это часть Accept-flow (Competitive/Wingman/DangerZone), для Casual не задействуется (§5/§9), не относится к этой проблеме.
- `game/mmqueue`/session-state сообщения — не видны в этом логе (`developer 2`/`con_timestamp 1` не были включены в этом запуске, как и в предыдущем — DevMsg-уровня трассировки нет, только `Msg`-уровня и наши собственные логи). Не могу подтвердить или опровергнуть что-либо про внутреннее состояние `CMatchSessionOfflineCustom`/KV-дерево на клиенте в этом раунде — **честно NOT PROVEN**, не буду придумывать.
- Каждая ИЗ 6 попыток `MatchmakingStart` доходит до нашего `ClientGC::OnMatchmakingStart` и успешно шлёт 9107 + ставит cookie НА КЛИЕНТСКОЙ стороне (что нерелевантно самому себе, см. Live Test #3/§32.1) — то есть клиентская часть **работает идентично и стабильно на каждой попытке**, разницы в клиентском поведении между успешной и провальными попытками **нет** — весь наблюдаемый разброс исходов объясняется исключительно состоянием на стороне `srcds`.

### 34.5 Вердикт по вариантам A-F

- **A (клиент повторно использует старый cookie)** — технически верно, что клиент КАЖДЫЙ раз посылает тот же самый `GameServerCookieId` (293a206f6c6c6548, видно в каждой из 6 попыток в логе) — но это **не баг, а намеренный дизайн** (§32.2: единый fixed cookie, гарантированно общий между `ClientGC`/`ServerGC` через одну сборку DLL). Само по себе это не объясняет провал — сервер ДОЛЖЕН принимать этот же cookie каждый раз, если бы он был переустановлен. **Не является причиной.**
- **B (srcds сохраняет старую резервацию и не принимает новую)** — частично да, но это следствие C, не отдельная независимая причина.
- **C (`ServerGC` устанавливает резервацию только один раз при `ServerHello`)** — **ПОДТВЕРЖДЕНО кодом (§34.2)**, это и есть корень проблемы. `ReserveServerForOurCookie()` физически не может сработать повторно — единственный вызов защищён только однократным `k_EMsgGCServerHello`.
- **D (сервер получает вторую резервацию, но старое состояние не чистится)** — неприменимо: сервер **не получает вторую резервацию вообще** (C — она просто никогда не отправляется повторно), поэтому вопрос "чистится ли старое состояние" не встаёт.
- **E (клиентский `QueueConnect`/session state заблокирован после первого матча)** — нет доказательств в текущем логе (DevMsg недоступен), и не требуется как объяснение — C уже полностью объясняет наблюдаемое (клиент ведёт себя идентично во всех 6 попытках, различается только исход, коррелирующий со стороной сервера, не клиента).
- **F (другое)** — не требуется, C с высокой уверенностью достаточно объясняет весь наблюдаемый паттерн (1 успех из 6, без деградации клиентского поведения).

# Причина: **C** — `ReserveServerForOurCookie()` вызывается ровно один раз, при `k_EMsgGCServerHello`, что происходит один раз за весь жизненный цикл процесса `srcds.exe`. После того как резервация была использована/истекла (точный триггер на стороне `srcds` не подтверждён, §34.3), новой резервации взяться неоткуда — все последующие `A2S_RESERVE_CHECK` не проходят, отсюда `MatchmakingStop` без `Connecting to...` и итоговое "Failed to connect to the match".

### 34.6 Минимальная точка исправления (только план, патч не делать)

Единственная точка кода, которую нужно менять — `gc_server.cpp`, вызов `ReserveServerForOurCookie()`. Сейчас он **привязан к одноразовому событию** (`k_EMsgGCServerHello`). Минимальное исправление — сделать вызов **повторяющимся** вместо одноразового, например (варианты, не решение):
- вызывать `ReserveServerForOurCookie()` периодически (например, раз в несколько секунд/раз за тик) из `Hk_SteamGameServer_RunCallbacks` (`steam_hook.cpp`) вместо/в дополнение к разовому вызову в `k_EMsgGCServerHello` — поддерживать сервер постоянно "разрешённым" для фиксированного cookie, что логично при архитектуре с одним статическим cookie (§32.5/§33.2);
- либо найти более точный триггер "матч закончился/клиент отключился" на серверной стороне и переустанавливать резервацию именно тогда (сложнее, требует RE серверных disconnect-хуков — не минимально).

Первый вариант — минимальный, не требует новых RE-находок, полностью укладывается в уже существующую инфраструктуру (`Hk_SteamGameServer_RunCallbacks` уже main-thread, уже вызывает `ReserveServerForQueuedGame` тем же путём). Не реализовано в этом раунде — только диагностика, по явной инструкции пользователя.

Никаких изменений кода/сборки/Git в этом раунде — только чтение `console.log` и `gc_server.cpp`/`gc_server.h` (уже существующий код с §33, не переоткрывался заново, кроме проверки количества вхождений `ReserveServerForOurCookie`).

---

## 35. Competitive / Wingman / Danger Zone Accept Flow + Fake Participant Research (read-only)

Приоритет смещён с §34 (reservation-баг зафиксирован, НЕ чинится в этом раунде) на исследование Accept-flow для `eGame∈{8,10,13}`. Ничего не менялось/не собиралось/не деплоилось/Git не трогался. Источники: `client_panorama.dll.i64` (та же база, что в §5/§16-21), `.proto`-схемы проекта, читаемые source-деревья, project446-бинарник (§6/§28). `ida_deobfuscated_grok` — **не IDA-decompiled дамп, а второй настоящий leaked/slipped source tree** (~2019-эры CS:GO Panorama lineage), как явно подтверждено пользователем — используется в этом разделе именно как source, не как RE-артефакт.

### 35.1 Главный архитектурный вывод — **CONFIRMED** (уже было найдено в §6, здесь применено к новому вопросу)

**Подсчёт "требуется/принято" НЕ реализован как нативный клиентский или даже GC-DLL-side счётчик, который можно было бы "обмануть" подделкой значения.** Это установлено прямой полной декомпиляцией `project446`'s `sub_100789A0` (§6, 1426 байт, обработчик 9102): при получении Accept-сигнала (не-abandon, accept-required game_type) код делает буквально:
```c
log("Matchmaking: Player ACCEPTED, waiting for all players");
sub_100BFC20(9102, &v23);   // форвардит сигнал НАРУЖУ, на backend
// состояние остаётся "waiting" -- дальше ничего не считается в этом DLL
```
Ни в `project446/csgo_gc.dll`, ни (как показано ниже) в `client_panorama.dll` НЕТ кода, который сравнивает `accepted_count == required_count` и сам принимает решение "все готовы". **Это решение принимается на backend** (Valve-сервере или, в случае project446, на их собственном WebSocket-бэкенде) — вне зоны видимости любого доступного нам бинарника. Клиент — "тонкий" участник: показывает попап, шлёт свой личный Accept, и ждёт ВНЕШНЕГО сигнала "матч готов" (нового GC-сообщения/состояния), не считает сам.

**Следствие для fake players (прямой ответ на §17 задания)**: раз retail-клиент не содержит проверяемого условия "сколько приняло", то fake participant'ам не нужно удовлетворять НИКАКОЙ клиентской логике подсчёта — потому что такой логики просто не существует на клиенте. "Все приняли" — это решение, которое **при отсутствии реального backend'а обязано принимать НАШЕ СОБСТВЕННОЕ GC-side код** (`gc_client.cpp`/`gc_server.cpp`, ещё не реализовано для Accept-flow), причём ровно так, как это делает retail backend: считать входящие Accept/Abandon-сигналы у себя и, когда решили, что "готово", прислать клиенту следующий GC-сигнал, который штатный клиентский код интерпретирует как "все приняли" (см. 35.2/35.3 — конкретно какой сигнал).

### 35.2 Match Found → popup_accept_match — точная цепочка — **CONFIRMED** (переподтверждение + одно новое звено)

Уже задокументированная в §5 цепочка (переподтверждена, не переоткрывалась заново):
```
sub_103DF430 (RVA 0x103DF430, client_panorama.dll) -- KV-listener на "game/mmqueue"
  this+140 == 1 (ready-up state)
    → DevMsg("Server reservation check %p ready-up!")
    → sub_10418AC0("popup_accept_match_found", 0)     -- ПОКАЗЫВАЕТ ACCEPT-ПОПАП
    → (условно) sub_103D8AF0()                         -- получить lobby-event-dispatcher объект
  this+132 (queue-connect state)
    → sub_103D7640(this+92)                            -- Accept-required-by-gametype проверка (35.4)
        true  → sub_10418AC0("popup_accept_match_confirmed", 0)
        false → sub_10418AC0("popup_accept_match_found", 0) + "@"+map (live-join)
    → безусловно строит "QueueConnect" KV-команду (см. §5 таблицу полей)
```
**Новое в этом раунде**: `sub_103D8AF0` (RVA `0x103D8AF0`, 90 байт, полностью декомпилирована) — это **lazy-singleton getter**, не сам dispatcher:
```c
int __thiscall sub_103D8AF0(void *this) {
    if (!byte_1519DDEE) return 0;                 // global "feature enabled" flag
    if (!dword_15278634) {
        v2 = g_pMemAlloc->Alloc(12432);            // 12432-байтный объект -- крупный UI-компонент
        if (v2) { dword_15278634 = sub_1057F440(v2, this); return dword_15278634 ? dword_15278634+8 : 0; }
    }
    return dword_15278634 ? dword_15278634+8 : 0;  // возвращает this+8 закэшированного объекта
}
```
Вызывается из **пяти мест**: 3 раза внутри `sub_103DF430` (ready-up ветка) и **1 раз внутри `sub_103D9900`** (обработчик 9104, RVA `0x103D9900`, см. 35.3). Возвращённый `this+8` затем вызывается как `(**v28)(v28, arg1, arg2, arg3)` — 3-аргументный вызов через vtable slot 0. **Не подтверждено на 100%** (не проверено побайтово), что этот vtable slot 0 буквально указывает на `sub_10581640` ("PanoramaComponent_Lobby_ReadyUpForMatch") — сигнатуры совпадают (3 аргумента), оба используются в одном и том же ready-up контексте, но прямая проверка адреса в vtable не проводилась в этом раунде. **HIGH CONFIDENCE, не CONFIRMED** на уровне идентичности функции.

`PartyMenu.ShowMatchAcceptPopUp` (JS-уровень, `popup_accept_match.js`/`.xml`) — упомянутые пользователем файлы/символы **не найдены заново в этом раунде** (не искались повторно — уже задокументированный путь в client_panorama.dll идёт через нативный `sub_10418AC0("popup_accept_match_found", 0)`, который скорее всего и есть нативная сторона `ShowMatchAcceptPopUp`, но JS-файлы `popup_accept_match.xml/js` физически недоступны для чтения из скомпилированного `panorama.dll`/`code.pbin` без отдельной распаковки Panorama-ресурсов — этого не делалось ни в этой, ни в прошлых сессиях). **TODO**, не критично для основного вывода.

### 35.3 Обработчик 9104 (`MatchmakingGC2ClientUpdate`) — впервые полностью декомпилирован в этом раунде — **CONFIRMED**

`sub_103D9900` (RVA `0x103D9900`, client_panorama.dll, 1385 байт) — ранее (§16/§18) была известна только сигнатура и две DevMsg-строки. Полная декомпиляция в этом раунде даёт:

```c
DevMsg("Matchmaking update: %d\n", v38[39]);           // v38[39] = protobuf поле `matchmaking` (field 1, int32)
if (v1[3])                                               // v1[3] = size() поля waiting_account_id_sessions (field 2)
    DevMsg("Matchmaking waiting for %d accounts (%X, ...)\n", v1[3], *(DWORD*)v1[2]);

if (!v38[39]) {
    // === matchmaking == 0: "поиск снят/остановлен" ветка ===
    // уничтожает закэшированный match-state объект (dword_152786A4/dword_15278690)
    // читает "cfg/qmmconnect.dt" -- QuickMatchMaking connect-файл (см. 35.9)
    // проверяет: sub_108F9840("members/numSlots", 0) == 1  И  KV-путь "game/mmqueue" == "connect"
    //   -> если оба true (и НЕ byte_1519DDEE) -- "не сбрасывать", что-то вызывает через dword_15280E10+80
    //   -> иначе -- сбрасывает через KeyValuesSystem
} else {
    // === matchmaking != 0: "активный статус" ветка ===
    // резолвит/лениво создаёт match-state объект (dword_152786A4, через sub_103DD020)
    // ПОВТОРНО проверяет sub_108F9A90("game/mmqueue", Locale) -- ТОТ ЖЕ GATE, что и в 9107-хендлере (§18)!
    //   -> gate FAIL: строит и шлёт MatchmakingStop обратно в GC (точно как в 9107-хендлере)
    //   -> gate OK: чистит список устаревших "note"-записей (по времени, вероятно penalty/notes поля протобуфа)
    //      if (v38[39] == 3) {                          // <-- matchmaking FIELD == 3
    //          v28 = sub_103D8AF0();                    // <-- ТОТ ЖЕ dispatcher-getter, что в ready-up ветке!
    //          if (v28) (**v28)(v28, 0, 0, 0);           // <-- 3-аргументный вызов, сигнатура = ReadyUpForMatch
    //      }
}
```

**Три новых, ранее не задокументированных факта**:
1. **9104 (`MatchmakingGC2ClientUpdate`) САМ гейтится тем же `game/mmqueue`-непустым условием**, что и 9107-хендлер (§18). Это **опровергает** более раннюю рабочую гипотезу (§18, строка 806/820) о том, что 9104 может быть "недостающим звеном", изначально выставляющим `game/mmqueue` в `"searching"`. Раз 9104 САМ требует, чтобы `game/mmqueue` уже было непустым, чтобы что-либо сделать (иначе тоже просто шлёт `MatchmakingStop`) — начальная установка `"searching"` происходит **где-то ещё, локально на клиенте, до всякого GC-сообщения** (согласуется с Live Test #2's находкой, §19 конец: окно между кликом Play и приходом 9101 — единственное известное место, где это могло произойти; конкретный setter по-прежнему НЕ найден, см. TODO 35.13).
2. **Поле `matchmaking` (protobuf field 1 в `CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate`) — это status-код (int32), не boolean.** Значение **`3`** конкретно триггерит вызов dispatcher-объекта через `sub_103D8AF0()` — тот же getter, что используется в ready-up-ветке `sub_103DF430`. **HIGH CONFIDENCE** (не 100% CONFIRMED — см. 35.2) это и есть механизм доставки `"PanoramaComponent_Lobby_ReadyUpForMatch"`-события ИЗ GC-сообщения 9104 (не только из локального `game/mmqueue`-изменения).
3. **`waiting_account_id_sessions` (protobuf field 2) — это НЕ список участников найденного матча**, это глобальный, queue-wide список (per-account) для ЭТОГО игрока — `DevMsg`-строка `"Matchmaking waiting for %d accounts"` печатает `.size()` этого repeated-поля и первый элемент в hex, но по контексту (общий queue-status update, а не reservation-specific) это, вероятнее всего, список аккаунтов **этого игрока в очереди** (например, при нескольких одновременных запросах/lobby-члены, ожидающие подтверждения), не "сколько человек нашлось в моём матче". **MEDIUM CONFIDENCE** по интерпретации значения, но **CONFIRMED**, что поле НЕ является счётчиком "требуется/принято" конкретного матча — оно из совершенно другого сообщения (9104, статус очереди), не из 9107 (резервация конкретного матча).

### 35.4 Required player count — **NOT FOUND как явное число в GC-протоколе; вывод по косвенным данным**

Прямого, явного числового поля "required players" **не найдено нигде** — ни в одном из полей `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`/`MatchmakingGC2ServerReserve`/`MatchmakingGC2ClientUpdate` (полный список полей — 35.6), ни в декомпилированном коде `sub_103DF430`/`sub_103D9900`/`sub_103DC4B0`. Это **согласуется** с 35.1: если подсчёт делает backend, клиенту незачем знать точное требуемое число — ему достаточно сигнала "готово"/"не готово" по каждому конкретному матчу.

Единственная найденная **числовая проверка count-подобного значения на клиенте** — в `sub_103D9900`'s `matchmaking==0` ветке: `sub_108F9840("members/numSlots", 0) == 1` (35.7/35.9) — но это сравнение с константой `1`, не с динамическим "сколько нужно для этого режима", и относится к KV-пути `"members/..."`, который, судя по структуре (`members/machine{N}/player{M}/xuid`, точно совпадающая с адресацией `ISteamMatchmaking::SetLobbyMemberData` из Hydra source'а `matchmaking/sys_session.cpp`), с высокой вероятностью представляет **PARTY-лобби (группу друзей ДО постановки в очередь)**, а не участников НАЙДЕННОГО матча. **NOT PROVEN**, какое из двух это на самом деле — не удалось декомпилировать writer этой KV-ветки в этом раунде (кандидат-функции найдены, ни одна не декомпилирована — см. 35.11).

`sub_103D7640` (Accept-required-by-gametype, §5) возвращает **bool**, не число — она отвечает только на вопрос "нужен ли Accept вообще для этого gameType", не "сколько человек нужно".

**Вывод**: required count для Competitive/Wingman/Danger Zone **архитектурно не хранится и не проверяется как явное число на клиенте** — единственный источник правды об этом (сколько реальных слотов в команде для конкретного режима) — это то, что backend решает передать через `account_ids`/`rankings` в `MatchmakingGC2ServerReserve` (embedded в 9107, 35.6), и КОЛИЧЕСТВО элементов в этом массиве и есть де-факто "required" — клиент просто ничего не сверяет с этим числом сам.

### 35.5 Accepted player count — **NOT FOUND на клиенте, backend-side по 35.1**

Как и required count — нет native-структуры на клиенте, которая бы считала "сколько уже приняло". `sub_100789A0` (project446, единственный код с полным accept state machine, который у нас вообще есть) явно форвардит каждый accept НАРУЖУ без подсчёта (35.1). На клиентской стороне (`client_panorama.dll`) декомпилированный accept-related код (`LobbyAPI.SetLocalPlayerReady`, §5) тоже **не читает и не пишет никакой числовой счётчик** — он классифицирует reason-строку и пишет булево/enum состояние в `this+12` (**не сеть, не число участников**). Заключение по 35.1 полностью применимо: `accepted_count`/`required_count`/`all_players_ready` как явные, читаемые native-переменные **не существуют** — есть только klиентский bool "я лично готов" и backend-side (недоступное) агрегирование.

### 35.6 Account IDs / участники — полная протобуф-схема, повторно проверено — **CONFIRMED**

`CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (9107) — **не содержит participant-полей напрямую**, но:
```protobuf
message CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve {
    optional uint64 serverid = 1;
    optional uint32 direct_udp_ip = 2;
    optional uint32 direct_udp_port = 3;
    optional uint64 reservationid = 4;
    optional CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve reservation = 5;   // <-- ВЛОЖЕННОЕ сообщение
    optional string map = 6;
    optional string server_address = 7;
}
```
Поле 5, `reservation`, **встраивает целиком** `CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve` — то же сообщение, что уходит НА сервер как 9105:
```protobuf
message CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve {
    repeated uint32 account_ids = 1;               // <-- ПОЛНЫЙ список участников
    optional uint32 game_type = 2;
    optional uint64 match_id = 3;
    optional uint32 server_version = 4;
    optional uint32 flags = 18;
    repeated PlayerRankingInfo rankings = 5;        // <-- per-player ранги
    optional uint64 encryption_key = 6;
    optional uint64 encryption_key_pub = 7;
    repeated uint32 party_ids = 8;                  // <-- группировка по пати (parallel array с account_ids)
    repeated IpAddressMask whitelist = 9;
    optional uint64 tv_master_steamid = 10;
    optional TournamentEvent tournament_event = 11;
    repeated TournamentTeam tournament_teams = 12;
    repeated uint32 tournament_casters_account_ids = 13;   // <-- casters, соответствует {%x} из §26/§28 reservation payload
    optional uint64 tv_relay_steamid = 14;
    optional CPreMatchInfoData pre_match_data = 15;
    optional uint32 rtime32_event_start = 16;
    optional uint32 tv_control = 17;
}
```
Это **уже существующие, реальные поля 2021-протокола** (наша собственная `.proto`-схема, кросс-подтверждённая ранее в проекте как байт-идентичная минимум 3 независимым источникам, §2/§27) — **никаких изменений схемы не требуется** для представления нескольких участников (`account_ids`), их пати-группировки (`party_ids`) и рангов (`rankings`). Это прямой ответ на п.4/п.13 задания: **структура для "real + fake participants" уже существует в протоколе, просто наш текущий `OnMatchmakingStart` не заполняет `reservation` вообще** (уже задокументировано в §18, строка 808-809, как "второстепенная находка" в прошлой сессии — подтверждается здесь снова).

`sub_103DC4B0` (9107-хендлер, §18) читает именно `v9 = *(v31+32)` — это и есть указатель на `reservation` (поле 5) — если не заполнено (как в нашем текущем коде), падает fallback на глобальное клиентское состояние. **Нет доказательств**, что `account_ids`/`party_ids`/`rankings` из этого вложенного сообщения читаются ДАЛЬШЕ (для рендеринга Accept-попапа) — сам 9107-хендлер использует из него только `& 0xF` (game_type-подобный байт) для выбора accept-required ветки. **NOT PROVEN**, что Accept-попап вообще показывает конкретные имена/аватарки участников из этого поля — это отдельный вопрос, не отвечен в этом раунде (не декомпилировался код рендеринга самого попапа, JS/XML недоступны, см. 35.2).

### 35.7 "members/" KV-дерево — новая находка этого раунда — **HIGH CONFIDENCE как party-представление, NOT PROVEN как match-found roster**

Полнотекстовый поиск строк в `client_panorama.dll` нашёл **реально существующую KV-структуру**, отдельную от protobuf/GC-сообщений:
```
members/numSlots            -- читается в sub_10245D10, sub_103DF430(!), sub_104690A0, sub_1058DAA0, sub_105C0040, и обработчике 9104 (sub_103D9900)
members/numPlayers          -- читается в 12+ функциях (sub_103B91C0, sub_1042EA80, sub_10482FF0, sub_104B0490, sub_104C0100/0320, sub_10513AE0, sub_1057F610, sub_1058DAA0, sub_105C0040/5450/5700, sub_10245D10)
members/numSpectators       -- sub_103B91C0
members/numTSlotsFree       -- sub_103B91C0   (T-команда свободные слоты!)
members/numCTSlotsFree      -- sub_103B91C0   (CT-команда свободные слоты!)
members/numMachines         -- sub_103DF430(!), sub_105CA870, sub_105CBA30, sub_105CBB00
members/machine%d/player0/xuid            -- sub_103DDCB0, sub_104B0490   (конкретный участник, per-machine/per-player XUID!)
members/machine0/player0/game/offjoin     -- sub_1057F9F0, sub_10581010
members/machine0/player0                  -- sub_105CBE70
members:numSlots (двоеточие, отдельный вариант пути)  -- sub_10245D10
```
**Критично**: `sub_103DF430` (сама ready-up KV-listener функция для `"game/mmqueue"`, §5!) **читает и `members/numSlots`, и `members/numMachines`** (xrefs @ `0x103dfa5a` и `0x103df6e5`) — то есть ready-up-логика РЕАЛЬНО консультируется с этим деревом, это не изолированная, неиспользуемая структура.

Адресация `machine{N}/player{M}/xuid` **побайтово совпадает по стилю** с реальным, подтверждённым Hydra-source кодом (§4, `matchmaking/sys_session.cpp`, `CSysSessionHost`/`CSysSession`, `ISteamMatchmaking()->SetLobbyMemberData`/`GetLobbyMemberData`, `SetLobbyData`) — Steam Lobby member data традиционно адресуется именно так в старых CS:GO/Source играх. **HIGH CONFIDENCE**: `"members/"` — это локальное зеркало **Steam Lobby участников (PARTY, до постановки в очередь)**, синхронизируемое Steam-колбэками, а не GC-протоколом.

**NOT PROVEN, но важный открытый вопрос**: используется ли ЭТО ЖЕ дерево (или его копия/пересчёт) также для представления участников НАЙДЕННОГО матча после того, как backend объединил несколько партий в одну игру? Ни один из декомпилированных в этом раунде фрагментов не даёт прямого ответа — writer-функции этого дерева (кто реально ПИШЕТ `members/numSlots`, а не читает) не были декомпилированы (см. 35.11, TODO).

### 35.8 Fake Accept — существующий retail-механизм для перевода участника в ready-состояние — **CONFIRMED, частично**

Единственный подтверждённый, полностью прослеженный retail-путь "участник → ready":
```
Panorama JS: LobbyAPI.SetLocalPlayerReady(reason: string)
    ↓ sub_10583500 (регистрация V8 FunctionTemplate)
    ↓ sub_10584F70 (JS-маршалинг аргументов, RVA 0x10584F70)
    ↓ sub_10585B00 (классификация reason через sub_10580C20, RVA 0x10585B00)
    ↓ sub_10580C20: if (reason=="deferred") ... else { sub_103DEA80(0, 2); return true; }
    ↓ пишет в this+12 состояние (НЕ шлёт сеть синхронно здесь — состояние локальное для UI)
```
Это относится к ЛОКАЛЬНОМУ ("моему собственному") ready-state — вызывается когда РЕАЛЬНЫЙ игрок нажимает Accept в UI. **Это единственная native точка, где клиент сообщает "я готов"** — она принимает `reason` (строку), не account id — то есть предназначена ТОЛЬКО для локального игрока, не для указания "какой участник" стал ready. **Нет параметра "for account X"** — `SetLocalPlayerReady` буквально означает "МЕСТНЫЙ игрок", не абстрактный API для управления состоянием произвольных участников.

**Вывод**: retail не предоставляет клиентский API "пометить УЧАСТНИКА B как ready" — потому что клиент вообще не управляет состоянием ДРУГИХ участников, это исключительно backend-domain (35.1). "Fake Accept" для fake-участников поэтому **не может и не должно** имитироваться через `LobbyAPI.SetLocalPlayerReady` (это API для "я" — реального игрока) — вместо этого fake-участники "принимают" исключительно в НАШЕЙ СОБСТВЕННОЙ GC-side бухгалтерии (мы решаем "все готовы" и шлём клиенту соответствующий сигнал), что и есть retail-эквивалентная архитектура (backend решает, клиент не считает).

### 35.9 `cfg/qmmconnect.dt` — новая находка, не задокументированная ранее — **CONFIRMED существование, TODO назначение**

`sub_103D9900` (`matchmaking==0` ветка) читает/пишет файл `"cfg/qmmconnect.dt"` через `"USRLOCAL"`-путь (`(*(vtbl+40))(dword_152680D8+4, "cfg/qmmconnect.dt", "USRLOCAL")`/`(*(vtbl+80))(...)`). Имя файла ("QMM" = Quick Match Making, как и упомянутый пользователем dead-code `cs_bot_manager.cpp:1230-1267`, §3) намекает на кэш/persist-состояние старой bot-fill-slot механики. **Содержимое/формат файла не исследовались** (не пытались открыть/распаковать в этом раунде). **TODO**, потенциально релевантно для "как клиент кэширует информацию об участниках между сессиями", но не подтверждено.

### 35.10 Competitive / Wingman / Danger Zone — потоки по отдельности

Общий вывод для ВСЕХ ТРЁХ режимов: **весь прослеженный retail-код (`sub_103D7640`, `sub_103DF430`, `sub_103D9900`, `sub_10581640`) НЕ различает game_type для целей required/accepted-подсчёта** — единственное место, где `game_type` вообще на что-то влияет в этой цепочке — это (а) `sub_103D7640`'s bool "нужен ли Accept" (35.4, одинаковый результат `true` для 8/9/10/11/13, `false` для остальных), и (б) выбор callback-режима в 9107-хендлере (`sub_103F7980(v34,1,0)` vs `(v34,2,2)`, §18, эффект на что именно — не до конца прослежен). **Нет отдельного, по-game_type-разного, числового required-count в клиентском коде** — что означает: с точки зрения RE, Competitive/Wingman/Danger Zone **структурно идентичны** в части "сколько нужно/сколько приняло", потому что этот подсчёт вообще не на клиенте (35.1/35.4).

**Competitive (eGame=8)**: `MatchmakingStart → (backend matching, не реверсено) → MatchmakingGC2ServerReserve(9105)→сервер → MatchmakingGC2ClientReserve(9107)→клиент, reservation.account_ids заполнен backend'ом → game/mmqueue: reserved/ready-up → popup_accept_match_found → LobbyAPI.SetLocalPlayerReady("...") [клиент] → 9102(accept) → backend решает все ли готовы → (HIGH CONFIDENCE) MatchmakingGC2ClientUpdate(9104).matchmaking==3 → sub_103D8AF0()-dispatcher → ReadyUpForMatch-подобное событие → game/mmqueue: connect → QueueConnect KV-команда → connect`. **CONFIRMED** архитектура пути, **HIGH CONFIDENCE** конкретно звено 9104.matchmaking==3→dispatcher (35.3).

**Wingman (eGame=10)**: **тот же путь**, никакой отдельной, по коду отличающейся структуры не найдено. Единственное известное числовое различие для Wingman — не на клиенте, а в `project446`'s GC-стороне (`sub_10094120`, §28.5): team-cap `2` вместо `5` для account-list bracket-padding при построении `sv_mmqueue_reservation` payload'а — это BACKEND/GC-side (наш будущий код), не retail client-side ограничение.

**Danger Zone (eGame=13)**: **тот же путь** на уровне найденного клиентского кода. Danger Zone — режим с другим числом игроков (battle royale, не 5v5) в реальности, но **этот факт нигде не закодирован в исследованном client_panorama.dll accept-коде** — что согласуется с 35.1 (backend решает состав, клиент не проверяет). ⚠️ project446's `sub_10094120` (native-Q-reserve builder, §28.3) **вообще НЕ обрабатывает** `game_type==13` (условие там `result==8 || result==10`) — то есть даже у project446 нет отдельной реализации для Danger Zone в этом конкретном payload-билдере; неизвестно, обрабатывают ли они DZ каким-то другим путём. **NOT PROVEN** что-либо специфичное для DZ помимо общего вывода "клиент не считает".

### 35.11 Party / участники — что реально существует — **HIGH CONFIDENCE** (см. 35.7), плюс source-tree

`matchmaking/sys_session.cpp` (Hydra source, §4, повторно процитировано) — **реально рабочий** Steam Lobby код: `CreateLobby`, `SetLobbyData`, `JoinLobby` — это и есть, по всей видимости, source-уровневый эквивалент того, что заполняет `"members/..."` KV-дерево (35.7) в реальном бинарнике. Это уже задокументированная в проекте (§4) находка, здесь только явно связана с новым RE-открытием `members/` дерева.

### 35.12 Минимальная test-only архитектура — по факту всего вышеустановленного

Раз retail не хранит required/accepted как проверяемое клиентом число (35.1/35.4/35.5), схема из задания (п.17) технически недостижима буквально ("fake participants становятся ready через retail Accept API") — потому что retail Accept API (`SetLocalPlayerReady`) **предназначен только для локального игрока**, а не для управления состоянием других участников (35.8). Ближайший к идеалу вариант, минимально трогающий retail-логику:

```
Matchmaking test harness (НАШ GC-side код, ещё не написан)
    ↓ real player присылает MatchmakingStart (eGame=8/10/13)
    ↓ мы (GC) РЕШАЕМ состав матча: account_ids = [real, fake_1, ..., fake_N] -- заполняем reservation (35.6),
      это уже существующие protobuf-поля, никакой схемы менять не нужно
    ↓ отправляем 9107 с заполненным reservation -- retail game/mmqueue переходит в ready-up (this+140==1)
    ↓ retail показывает popup_accept_match_found -- ПОЛНОСТЬЮ retail UI, не тронуто
    ↓ real player жмёт Accept -- retail LobbyAPI.SetLocalPlayerReady() -- ПОЛНОСТЬЮ retail API, не тронуто
    ↓ клиент шлёт нам 9102(accept, abandon=0) -- ПОЛНОСТЬЮ retail сеть, не тронуто
    ↓ МЫ (GC) считаем: 1 real accept + предполагаем N fake accepts (наша собственная, не retail, бухгалтерия -- 
      ровно то место, где backend считает в реальности, 35.1) == required -- "матч готов"
    ↓ отправляем клиенту MatchmakingGC2ClientUpdate(9104) с matchmaking==3 (HIGH CONFIDENCE триггер ReadyUpForMatch, 35.3)
      ИЛИ иной сигнал, переводящий game/mmqueue в "connect" (точный эквивалент retail backend-сигнала, TODO уточнить какой именно)
    ↓ retail: ReadyUpForMatch/game-mmqueue-connect → QueueConnect (ПОЛНОСТЬЮ retail, не тронуто) → connect to srcds
```
**Не реализовано в этом раунде.** Ключевое: fake participants существуют ТОЛЬКО как (а) записи в `account_ids`/`party_ids` протобуф-полей, которые retail уже умеет принимать без изменений схемы, и (б) как бухгалтерия в НАШЕМ будущем GC-коде (не retail) — никакого bypass'а retail required/accepted-condition не требуется, потому что такого condition на клиенте не существует.

### 35.13 Нерешённые вопросы (честно, не придумано)

- Кто именно (и когда) СИНХРОННО с кликом "Play" ставит `game/mmqueue` в `"searching"` — по-прежнему **не найдено** (35.3 п.1 только сузил окно, опровергнув 9104-гипотезу; сам setter не идентифицирован).
- Идентичность `dword_15278634+8`'s vtable slot 0 с `sub_10581640` — **не проверена побайтово** (35.2).
- Кто ПИШЕТ (не читает) `members/numSlots`/`members/numPlayers`/`members/machine%d/player0/xuid` — **не найдено**, кандидаты не декомпилировались (список функций-читателей есть, писателя среди них не идентифицировано явно).
- Показывает ли Accept-попап конкретные имена/аватарки из `reservation.account_ids`, или использует ТОЛЬКО `members/`-дерево, или оба — **не определено** (JS/XML Panorama-ресурсы не распакованы).
- `PartyMenu.ShowMatchAcceptPopUp`/`popup_accept_match.xml`/`.js` как отдельные, именованные символы — **не найдены заново** в бинарнике в этом раунде (вероятный кандидат — `sub_10418AC0("popup_accept_match_found", 0)`, но прямого текстового совпадения имени не установлено).
- `cfg/qmmconnect.dt` — назначение/формат не исследованы (35.9).
- Точный протокольный сигнал (9104 с каким именно значением `matchmaking`, либо другое сообщение), которым retail backend доводит до клиента "все приняли, го коннект" — известно только `matchmaking==3` как HIGH CONFIDENCE кандидат; полный список возможных значений этого enum (кроме `0` и `3`) не восстановлен.

### 35.14 Sweep обоих source-деревьев + своего проекта — результат получен, дополняет 35.13 — **CONFIRMED** (прямое чтение source)

Фоновый агент выполнил полнотекстовый поиск по `ida_deobfuscated_grok`/`cstrike15_src-master`/своему проекту. Главный вывод: **ни в одном дереве нет специально построенной "fake test player"/"simulate Accept"-системы для matchmaking.** Значимые находки:

**`IsFakePlayer`/`m_bFakePlayer`/`bFakePlayer`** (`public/igameresources.h:28`, `engine/baseclient.h:119,332`, `engine/baseclient.cpp:654-684`) — это **общий движковый механизм** "клиент — бот, занимающий слот" (`CBaseClient::Connect(..., bool bFakePlayer, ...)`), используемый обычными in-game ботами (`bot_add`), **НЕ** специфичен для matchmaking/Accept-симуляции. Существует идентично в обоих деревьях.

**QMM dead-code блок** (`game/server/cstrike15/bot/cs_bot_manager.cpp:1230-1267`, ida_deobfuscated_grok) — полный текст получен:
```cpp
//
// In Queued Matchmaking mode bots are always taking spots of humans
// find a human that is not yet connected and use that human's stats
// TODO: <vitaliy> this would allow bots to take over humans
//
if ( 0 && CSGameRules() && CSGameRules()->IsQueuedMatchmaking() && CSGameRules()->m_mapQueuedMatchmakingPlayersData.Count() )
{
    CUtlVector< uint32 > arrConnectedHumans;
    for ( int i = 1; i <= gpGlobals->maxClients; ++i ) {
        CCSPlayer *pHuman = dynamic_cast<CCSPlayer *>(UTIL_PlayerByIndex( i ));
        if ( pHuman ) { uint32 uiAccountId = pHuman->GetHumanPlayerAccountID(); if (uiAccountId) arrConnectedHumans.AddToTail(uiAccountId); }
    }
    FOR_EACH_MAP( CSGameRules()->m_mapQueuedMatchmakingPlayersData, idxQueuedPlayer ) {
        CCSGameRules::CQMMPlayerData_t &qmmPlayerData = *CSGameRules()->m_mapQueuedMatchmakingPlayersData.Element(idxQueuedPlayer);
        if ( arrConnectedHumans.Find(qmmPlayerData.m_uiPlayerAccountId) != arrConnectedHumans.InvalidIndex() ) continue; // уже подключён
        bool bThisPlayerIsFirstTeam = ( qmmPlayerData.m_iDraftIndex < 5 );
        ... // team-assignment logic
        bot->SetHumanPlayerAccountID( qmmPlayerData.m_uiPlayerAccountId );
        engine->SetFakeClientConVarValue( bot->edict(), "name", CFmtStr("BOT %u", qmmPlayerData.m_uiPlayerAccountId).Access() );
    }
}
```
**Навсегда мёртвый код** (`if (0 && ...)`, короткое замыкание). **Важная оговорка (честно, не факт)**: этот блок работает **ПОСЛЕ** формирования матча/резервации (на СЕРВЕРНОЙ, не клиентской стороне, в `server.dll`/game DLL) — он про "бот подменяет ЕЩЁ НЕ ПОДКЛЮЧИВШЕГОСЯ зарезервированного человека своим account ID", НЕ про "симулировать Accept-клик в лобби". Другая стадия конвейера, чем то, что нужно для Accept popup.

**`CCSGameRules::CQMMPlayerData_t` / `m_mapQueuedMatchmakingPlayersData`** (`cs_gamerules.h:1226-1235,1326-1334`) — **это НЕ мёртвый код**, активно используется (30+ мест) в `cs_player.cpp` для трекинга per-round статистики по account_id. Это **серверная (game DLL) roster-структура для зарезервированного матча** — по всей архитектуре reservation-протокола (§26-28) она, вероятнее всего, заполняется ИЗ account-list'а `sv_mmqueue_reservation`-payload'а (`[accountid_hex]...` bracket-токены, §28.4-28.5) — **правдоподобная, не 100%-но прослеженная связь**, но она даёт: **если мы хотим "fake participants" на СЕРВЕРНОЙ стороне (не только client UI), кандидат-точка — заполнение ЭТОГО account-списка в payload'е `ReserveServerForQueuedGame`, а не что-либо на клиенте.**

**Независимое кросс-подтверждение 35.7 через source** — `matchmaking/mm_session_online_host.cpp:1663-1669`:
```cpp
int numPlayers = 1;
...
pMembers->SetInt( "numPlayers", numPlayers );
pMembers->SetInt( "numSlots", MAX( numSlotsCreated, numPlayers ) );
```
и `matchmaking/cstrike15/mm_title_gamesettingsmgr.cpp:1536-1567` (`"Filter>=/members:numPlayers"`, `"Near/members:numPlayers"`) — **байт-в-байт совпадает** с найденными в бинарнике KV-путями `members/numPlayers`/`members/numSlots`/`members:numSlots` (35.7)! Это **поднимает уверенность 35.7 с HIGH CONFIDENCE до CONFIRMED**: `members/numPlayers`/`members/numSlots` — реальная, задокументированная в source KeyValues-конвенция для представления состава лобби (`CSysSessionBase`/`SessionMembersFindPlayer(pSettings, xuid)`, `sys_session.cpp:850-851`), используемая как для party (pre-queue), так и, по всей видимости, general session-member accounting в целом — источник НЕ уточняет однозначно party-only это или также match-found-specific (открытый вопрос 35.7 остаётся).

**`required_count`/`accepted_count`/`needed_count`** — **0 совпадений** ни в одном дереве. Единственный близкий эквивалент — `numPlayers`/`numSlots` через KeyValues (выше) — **подтверждает 35.4/35.5's вывод**: явного "required/accepted counter" как отдельной, специально названной переменной не существует нигде в доступном коде.

**`IExternalTestClient`/`IExternalTestClientManager`** (`public/iexternaltest.h`) — существует, это **generic QA/load-testing интерфейс** (массовое создание тестовых game/server клиентов для стресс-тестов), **без видимой связи с matchmaking Accept-симуляцией** — не то, что нужно для нашей задачи.

**Собственный проект (`D:\csgo2021_gc`)** — `test_mm.h/.cpp` (уже задокументированы, §26-33, остаются неиспользуемыми) — единственный "test"-помеченный код, и по собственным комментариям явно НЕ fake-player/Accept-симулятор, а протокол-верификационный responder. `gc_shared.cpp:108-201` уже РЕГИСТРИРУЕТ обработчики party-протокола (`k_EMsgGCInviteToParty`/`PartyInviteResponse`/`KickFromParty`/`LeaveParty`/`Party_Register`/`Unregister`/`Search`/`Invite`/`ClientPartyJoinRelay`/`ClientPartyWarning`) — **реальные, уже объявленные message ID, тела обработчиков не содержат fake-player логики** (не проверялось построчно в этом раунде, только факт регистрации в диспетчере).

---

## 36. Matchmaking State Writers, Accept Propagation and Fake-Player Injection Point (read-only RE)

Код НЕ менялся, сборка/деплой/Git не выполнялись. Использованы IDA-базы `client.dll`, `client_panorama.dll`, `engine.dll`, `project446/csgo_gc.dll` (уже были) и **две новые**, снятые в этом раунде с реальных retail-файлов игры: `matchmaking.dll` (`csgo/bin/`, 624 KB) и `server.dll` (`csgo/bin/`, 12 MB), плюс loose-файл `csgo/gamemodes.txt`. Source-деревья использованы как source (не как IDA-дамп). Скрипты: `q36_*.py` в scratchpad. **Ключевой вывод раунда меняет постановку**: у Accept НЕТ "внутриклиентского счётчика, который надо обманывать" — счёт делает **игровой сервер (`CBaseServer`)** по per-player `reservationStage`, а клиент лишь показывает результат (см. 36.F).

### 36.A `game/mmqueue` — точный writer и последовательность записей — **CONFIRMED** (client.dll, прямая декомпиляция + raw disasm)

Раздел 35.13 п.1 ("кто ставит `searching`") **закрыт**, и уточнена формулировка §35.3: `searching`/`connect` пишет обработчик 9104, а **первое** значение пишет отправитель 9101 — `registering`.

| Значение `game/mmqueue` | Кто пишет | Функция (client.dll / client_panorama.dll) | Условие |
|---|---|---|---|
| *(нет ключа)* | сессия при создании | `CMatchSession*::InitializeGameSettings` (matchmaking.dll) | до Play |
| **`registering`** | **отправитель `MatchmakingStart`** | **`sub_103F6240`** (client.dll, RVA `0x103F6240`) / `sub_103DDCB0` (client_panorama, `0x103DDCB0`) | `game/mmqueue` был пуст (`v73==0`) — шаблон `"Update { game { mmqueue registering } }"` @ `0x10bcc5c0`, raw disasm `0x103f6ab0-0x103f6ac0`: `test al,al; cmovz edx, ecx` |
| **`heartbeating`** | тот же `sub_103F6240` | то же место | `game/mmqueue` уже был непуст — шаблон `0x10bcc574` |
| **`searching`** | обработчик 9104 | **`sub_103F1C00`** (client.dll) / `sub_103D9900` (client_panorama) | gate `game/mmqueue` непуст **И** поле `matchmaking != 0` **И `!= 4`**: raw `0x103f1ffd: cmp [ecx+0A8h],4; ...; cmovnz edx,eax` → `"Update { system { lock #empty# } game { mmqueue searching } }"` @ `0x10bcaee8` |
| **`connect`** | 9104 при `matchmaking == 4`; также `sub_103F7C00` (net-callback, status 4) | те же | `"Update { system { lock mmqueue } game { mmqueue connect } }"` @ `0x10bcaf28`, xrefs `0x103f2009`, `0x103f7dac` |
| **`reserved`** | обработчик 9107 (`sub_103F49F0`/`sub_103DC4B0`), `sub_103F6240` (reconnect-ветка), `sub_103F7C00` | — | `"Update { system { lock mmqueue } game { mmqueue reserved } }"` @ `0x10bcc020`, xrefs `0x103f4a98`,`0x103f633c`,`0x103f7d07` |
| *(удаление)* | `sub_103F6DD0` (cancel), 9104 при `matchmaking==0` | — | `"Delete { system { lock #empty# } game { mmqueue #empty# } }"`, `"Delete/game/mmqueuestop"` |

**`sub_103F6240` — отправитель `MatchmakingStart` (RVA `0x103F6240`, client.dll, 2956 байт)** — **CONFIRMED**: содержит vftable `CProtoBufMsg<CMsgGCCStrike15_v2_MatchmakingStart>` (xref `0x103f6d88`); это **виртуальный метод `CLobbyMenuSingleton`** (client.dll vtable `0x10bccbf0`: slot2=`sub_103F6240`, slot3=`sub_103F6DD0` cancel; client_panorama vtable `0x10b6a410`: slot2=`sub_103DDCB0`, slot3=`sub_103DE630`), вызываемый только через vtable/data. Поля 9101 берутся так (прямо из декомпиляции):
- `account_ids` ← **цикл по KV `members/machine%d/player0/xuid`** (сначала локальный Steam ID, затем остальные machine-записи ≠ себя);
- `game_type` ← `game/mapgroupname` (+ `"reconnect"`-ветка) через `sub_103F1240`, затем `<<8`-композиция mapgroup;
- `client_version` ← `INETSUPPORT_003`→vtbl+36 (=13805, совпадает с нашим логом);
- `prime_only` ← `game/prime`; поле flags ← `game/gamemodeflags`; турнирные `tm_event_id/tm_event_stage_id/tm_team_id_ct/tm_team_id_t` в KV `Game::EnteringQueue`.
Метод **пере-вызывается периодически** (при непустом mmqueue пишет `heartbeating`, обновляет `this[1]=Plat_MSTime()`, снова шлёт 9101) — **HIGH**: частота и вызывающий таймер не найдены (вызов только через vtable).

**Реальная последовательность (client-side, подтверждено кодом):** `Play` → `CLobbyMenuSingleton::vfn2` (`sub_103F6240`): пустой mmqueue ⇒ `registering` + 9101 → GC шлёт 9104 (`matchmaking`≠0,≠4) ⇒ `searching` → (повторные 9101 ⇒ `heartbeating`) → 9107 ⇒ `reserved` → status 4 ⇒ `connect`. **Почему наш Casual-тест проходил gate 9107 без 9104**: `registering` уже был выставлен `sub_103F6240` на клике Play, gate (`mmqueue` непуст) выполнялся сам собой.

### 36.B `sub_103D8AF0` — **CONFIRMED** (полная цепочка)

`sub_103D8AF0` (client_panorama `0x103D8AF0`, 90 байт; client.dll-аналог `sub_103F0C70`) — **ленивый singleton-getter** глобального `dword_15278634` (объект 12432 байт, ctor `sub_1057F440` = **`CUiComponent_Lobby`**, vtables `0x10bbdf44/0x10bbdf60/0x10bbdf74`); возвращает `obj+8` **только если `byte_1519DDEE`** (Panorama-лобби включено). `obj+8` — встроенный **`CLobbyMenuSingleton`** (ctor `sub_103DDA40`). Вызывается из 5 мест: 9104-хендлер ×1 (`0x103d9d5b`) и `sub_103DF430` ×4. Дальше `(**v)(v, a, b, c)` = виртуальный слот 0 подобъекта (в `CUiComponent_Lobby` vtable `0x10bbdf60`) = **`sub_1057FF90`**.

### 36.C `ReadyUpForMatch` — **CONFIRMED** (native→Panorama путь)

- **Определение события**: `sub_10581640` (client_panorama `0x10581640`) регистрирует тип `"PanoramaComponent_Lobby_ReadyUpForMatch"`, id **`word_10D54BF8`**, 3 аргумента. Вызывается только через таблицу трамплинов (`sub_100502C0`, data `0x10a98d30`) — это **регистрация схемы события, не его вызов**.
- **Создание аргументов**: `sub_1057F030` = конструктор **`panorama::CUIEvent3<bool,int,int>`** для того же `word_10D54BF8`.
- **Raiser**: **`sub_1057FF90(this, bool bShow, int a3, int a4)`** (`0x1057FF90`, 100 байт): `this+16 = bShow ? Plat_MSTime() : 0`; если у Panorama есть слушатель события `word_10D54BF8` (`dword_15280F00` vtbl+216) — диспатчит `CUIEvent3<bool,a3,a4>` (vtbl+208). Совпадение id `word_10D54BF8` в регистрации и в raiser'е — подтверждение идентичности (ответ на "CONFIRMED / NOT CONFIRMED" из задания: **CONFIRMED**).
- **Точки вызова raiser'а** (все = `RaiseReadyUp(bool, int, int)`):
  1. `sub_103DF430` (net-callback `CServerConfirmedReservationCheckCallback`, `0x103DF430`): `stage==2` и статус "ещё ждём" → **`(true, accepted, total)`** (обновление счётчика попапа); `stage==1` и status==4 → **`(true, 0, total)`** (показ ready-up после `popup_accept_match_found`); при `!byte_1519DDEE` — ещё `(false,0,total)`.
  2. 9104-хендлер (`sub_103D9900`) при **`matchmaking == 3`** → `(false, 0, 0)` (`0x103d9d5b`) — это **сброс/закрытие ready-up**, а НЕ показ попапа. **Это уточняет §35.3 п.2** (там `matchmaking==3` трактовалось как триггер показа): показ идёт из 0x21/0x25-цикла (36.F).
- `ServerReserved`: `sub_103E1390` — регистрация события, `sub_103D7750` строит `CUIEvent2<char const*,double>` для **`word_10D501B4`**, raiser — `sub_103DEA80(kv, mode)` (`0x103DEA80`): при `mode==2` и `dword_15278694` (отложенный QueueConnect KV) берёт `map` и диспатчит `word_10D501B4` ⇒ **`ServerReserved(map,time)`** — **HIGH** (id совпадают, raiser найден, JS-слушатель не распакован).
- Значения поля `matchmaking` (proto field 1) по коду 9104-хендлера: `0`→очистка (`Delete`, `cfg/qmmconnect.dt`), `3`→`RaiseReadyUp(false,0,0)`+`searching`, `4`→`connect`, прочие ненулевые→`searching`. Смысловые имена enum — **не восстановлены**.

### 36.D `members/numSlots` — writer — **CONFIRMED** (matchmaking.dll binary + source)

`members/*` — **локальное KV party/session-настроек**, пишется **клиентским `matchmaking.dll`**, не GC:
- **`sub_10024F70`** (matchmaking.dll `0x10024F70`) = `CMatchSessionOnlineHost::InitializeGameSettings` (строка `"CMatchSessionOnlineHost::InitializeGameSettings adjusted settings:"`, в начале читает `members/numSlots` = `numSlotsCreated`); caller `sub_10022720`.
- **`sub_1001BEA0`** (`0x1001BEA0`) = `CMatchSessionOfflineCustom::InitializeGameSettings` (тот же паттерн); caller `sub_1001B0A0`.
- Пишут: `members{ numMachines=1, numPlayers=1, numSlots=MAX(numSlotsCreated,numPlayers), machine0{ id, flags, numPlayers, dlcmask, tuver, ping, player0{ xuid, name } } }` из **локального player manager** — 1:1 с `mm_session_online_host.cpp:1658-1700` (source) и `mm_session_offline_custom.cpp:430`.
- Дальнейшие модификации (source): `mm_title_gamesettingsmgr.cpp:841-860` — `numSlots` (по умолчанию 10) принудительно = `GetMaxPlayersForTypeAndMode` **только** для official-search (`options/createreason=="searchempty"` && `!game/hosted`); `:321` — "all CS:GO online lobbies are 10-slots"; `sys_session.cpp:956` — при входе машины `numSlots = machine.numPlayers`; чужие члены приходят через `SysSession` (Steam Lobby), не GC.
- `sub_105807B0` (client_panorama, vtable slot1 `CLobbyMenuSingleton`) = "я хост/соло-лидер?": `Members/numSlots == 1` ⇒ true, иначе сравнение `xuidHost` с локальным.
- **Мост `members/` ↔ `account_ids`**: **CONFIRMED только в одну сторону** — `members/machine%d/player0/xuid` ⇒ `MatchmakingStart.account_ids` (`sub_103F6240`, 36.A). **Обратной связи `reservation.account_ids` (9107) ⇒ `members/` в декомпилированном 9107-хендлере нет** (используется только `& 0xF` game_type; полевой копировщик `sub_103DFBE0` не декомпилировался — **TODO, MEDIUM**). Значит `members/` = **party до очереди**, не состав найденного матча.

### 36.E Реальный Accept — путь от Panorama до сервера — **CONFIRMED (код) / MEDIUM (JS-reason)**

```
Panorama Accept-кнопка → JS LobbyAPI.SetLocalPlayerReady(reason)         [JS popup_accept_match.js не распакован]
  → sub_10583500 (V8 template) → sub_10584F70 (маршалинг) → sub_10585B00 (client_panorama)
  → sub_10580C20(reason):
       reason == "deferred": sub_103DF3A0(dword_15278690)               <== ЕДИНСТВЕННОЕ место, меняющее stage
            if (callback.stage(this+140) < 2) { callback.stage=2;
               cancel prev request: callback.this+128 handle → vfunc+20;
               INETSUPPORT_003 vtbl+60 (тот же вызов, что в ctor) с reservationStage=2 }
       иначе:  sub_103DEA80(0, 2)                                         (отложенный QueueConnect KV → ServerReserved event)
  → engine: новый A2S_RESERVE_CHECK (0x21) с reservationStage = 2, SteamID клиента
```
`CServerConfirmedReservationCheckCallback` ctor (`sub_103DF1C0(this, kv, a3=stage, a4=mode)`, client_panorama `0x103DF1C0`) кладёт `this+140 = a3` (stage), `this+132 = a4` (mode), печатает `"Server reservation check %p heartbeating"` и регистрирует запрос в `INETSUPPORT_003` vtbl+60 с `a3` как reservationStage. 9107-хендлер выбирает: nested `reservation.game_type & 0xF` ∈ {8,9,10,11,13} ⇒ `sub_103DF1C0(kv, **1**, 0)` (stage 1, mode 0 — Accept-ветка), иначе ⇒ `sub_103DF1C0(kv, **2**, 2)` (stage 2, mode 2 — прямой connect). При **отсутствии nested `reservation`** берётся глобальный search-state (`dword_10D9A180+32`) — то есть наш нынешний 9107 без `reservation` уже пойдёт по Accept-ветке для `eGame∈{8,10,13}`. **Не подтверждено**, какой `reason` реально передаёт retail-JS Accept-кнопки (единственная нативная ветка, меняющая stage, — `"deferred"`; **HIGH**, что это и есть Accept).

### 36.F Где реально считается accepted/required — **CONFIRMED** (source engine + binary engine.dll + server.dll)

**Источник истины — игровой сервер, а не клиент и не (в retail-модели) backend.** `CBaseServer::ReplyReservationCheckRequest` — engine.dll **`sub_101BC1D0`** (RVA `0x101BC1D0`, 2189 байт; строки `"Reservation from client %u: %u"` @ xref `0x101bc533`, `"Match start status: %u/%u"` @ `0x101bc69e`, дефолт `awaiting=127`), source `engine/baseserver.cpp:2165-2308`:
- **Roster** = `m_arrReservationPlayers` (`QueueMatchPlayer_t{account, adr, token, stage}`), строится в `SetReservationCookie` (`baseserver.cpp:4217-4296`) **только когда cookie ≠ текущего** и `sv_mmqueue_reservation` начинается с `Q<cookie>,`: каждый токен `[%x]` с **ненулевым** account → игрок (stage 0); `m_numGameSlots = Count()`. Для `G` roster очищается, `m_numGameSlots=0`. (Серверный game-rules строит свой `m_mapQueuedMatchmakingPlayersData` из тех же токенов, **`m_iDraftIndex` = позиция токена, включая `[0]`**; команда = `draftIndex < kNumPlayersPerSide` — `cs_gamerules.cpp:4636-4653,18383`; отсюда padding `[0]` у project446, §28.5.)
- **Обработка 0x21** (33 байта, §26): нужны `cookie == m_nReservationCookie`, `stage != 0`, `steamid != 0`, `sv_mmqueue_reservation[0]=='Q'`; игрок ищется по `CSteamID(steamid).GetAccountID()`; если найден: сохраняются `adr`,`token`, `qmp.stage = stage`; **`awaiting = #(игроков со stage < запрошенного stage)`, `total = Count()`**. Если аккаунта **нет в roster** — `awaiting` остаётся `127`, `total=0` (клиент никогда не получит status 4).
- Ответ `S2A_RESERVE_CHECK_RESPONSE` (0x25) `... stage, awaiting(byte), total(byte)`; **при `awaiting==0` сервер сам рассылает 0x25 ВСЕМ игрокам roster'а** (на сохранённые `adr`/`token`).
- `serverGameDLL->ReportGCQueuedMatchStart(minStage, confirmedAccounts[], count)` вызывается при каждом "just confirmed" (engine.dll: `(*(dword_139097E8 vtbl + 188))`, offset **188 = slot 47**). Есть встроенный **spoof auto-accept**: `bSpoofForcefulConnect = (awaiting && bReady && stage==2)` → сервер принудительно ставит stage всем остальным. **В retail `server.dll` 2021 этот метод — заглушка: `CServerGameDLL` vtable `0x1085cb10`, slot 47 = `sub_10145510` (RVA `0x10145510`, 5 байт) = `char __stdcall (int,int,int){return 0;}` → `bReady == false` всегда → spoof НЕ срабатывает, GC о подтверждениях не уведомляется** — **CONFIRMED** (прямая декомпиляция). project446 не случайно хукает именно этот слот (§6, `sub_100BADE0`).
- Клиент (36.C): аргументы `RaiseReadyUp(true, accepted, total)` вычисляются из ответа 0x25: `sub_103DF430` берёт 64-битный результат `a2->vfunc+8()` (два байта `awaiting`/`total`), `accepted = clamp(byte1 − byte0, 0, 64)`, `total = byte1` — **HIGH** (порядок байт не сверен побайтно; согласуется с project446 `"[ReadyUp] popup counter set to %d/%d"`, `sub_100EA660`).
- **Клиент поднимает Accept-попап**, когда для **stage 1** пришёл status 4 (`awaiting==0` ⇒ **все** игроки roster'а прислали stage≥1) — `sub_103DF430`, `this+140==1` → `DevMsg("Server reservation check %p ready-up!")`, `popup_accept_match_found`, `RaiseReadyUp(true,0,total)`. После Accept (stage 2) прогресс `accepted/total` идёт из очередных 0x25; при `awaiting==0` на stage 2 — ветка `QueueConnect` (KV с `adronline`,`reservationid`,`map`,`gametype/gamemode` по `this+92`), при `this+132 != 0`.

**project446-модель (для сравнения, §6/§28, повторно проверена)**: `sub_100789A0` форвардит Accept наружу без подсчёта (`sub_100BFC20`, 20+ call sites), подсчёт "все приняли" делает **их backend**; после этого GC получает **второй 9107 с `game_type==0`** — `"[GC] Connect reserve received (all players accepted)"` (`sub_1006EAD0`, xref `0x1006ede1`) → state 3 → proceed (`sub_100774E0`); счётчик попапа они выставляют нативно (`sub_100EA660`, RVA-параметры `GC_CLIENT_READYUP_ACTIVE_RVA`/`GC_CLIENT_READYUP_COMP_RVA`). Это согласуется с клиентским кодом 9107-хендлера: 9107 с не-accept `game_type` (например 0) создаёт callback `(stage 2, mode 2)` ⇒ direct-connect. Два способа довести до connect.

### 36.G Fake players — минимальная точка внедрения — вывод по подтверждённым фактам

**Ответ на п.9 (нужен ли отдельный fake GC client): НЕТ.** Участники в retail-модели — не GC-клиенты, а **записи roster'а game-сервера + UDP-пакеты `A2S_RESERVE_CHECK`**:
1. **Принадлежность к матчу** = fake-аккаунты как **`[%x]`-токены `Q`-payload'а** (`SetReservationCookie`). Тот же payload обязан содержать **реальный account игрока** (иначе его 0x21 получит `awaiting=127,total=0`, status 4 не придёт никогда).
2. **"Accept" fake-участника** = per-player `m_uiReservationStage`, который меняет **только входящий 0x21** с подходящим `SteamID.account` и совпавшим cookie (проверка личности — только nonzero steamid из пакета; Steam-auth/GC-сессия для этого пути **не нужны**). Нужно **два прохода**: stage 1 (для показа попапа — иначе клиент ждёт `awaiting==0` на stage 1) и stage 2 (после реального Accept). Встроенный spoof (36.F) покрывает **только stage 2** и в retail-`server.dll` выключен (`return 0`); stage 1 он не покрывает вообще.
3. Точка внедрения без изменения retail-условий: **`ServerGC` внутри `srcds`** (уже владеет вызовом `ReserveServerForQueuedGame`, §33): (а) строит `Q`-payload из `[real, fake…]` (сейчас строит `G`), (б) шлёт 33-байтные 0x21 от имени fake-account'ов на собственный UDP-порт сервера (формат — §26, byte-exact). Никаких изменений клиента, `Accept`-условия, required-count, QueueConnect, Panorama, protobuf.
4. **Что сервер должен знать заранее**: account реального игрока. `ServerGC` его не знает (клиент/сервер — разные процессы; account приходит в 9101 к `ClientGC`). Варианты (не реализовано, только фиксация ограничения): test-only параметр в `config.txt` сервера либо передача аккаунта существующим каналом. **Это реальное архитектурное ограничение.**
5. Со стороны `ClientGC`, отправляющего 9107: для Accept-ветки достаточно `reservation.game_type ∈ {8,9,10,11,13}` (или отсутствия nested `reservation` + search game_type из 9101); nested `reservation.account_ids/party_ids/rankings` в клиентском хендлере для accept-логики **не используются** (только `& 0xF`; **NOT PROVEN**, что попап рисует по ним имена).

### 36.H Режимы — сравнение — **CONFIRMED (client/server код) / NOT PROVEN (DZ-roster)**

| | Competitive (8) | Wingman (10) | Danger Zone (13) |
|---|---|---|---|
| Accept-ветка клиента (stage1→popup→stage2) | да | да | да — **идентичная** машина состояний |
| Server accounting | `CBaseServer` Q-roster, идентично | идентично | идентично (тот же `CBaseServer`) |
| `gamemodes.txt` `maxplayers` (retail, `csgo/gamemodes.txt`) | **10** (стр.96) | **4** `scrimcomp2v2` (стр.183) | **16** `freeforall/survival` (стр.781) |
| Team split токенов (`kNumPlayersPerSide`) | 5+5 (project446 pad 5) | 2+2 (project446 pad 2) | **не подтверждено** — project446 `sub_10094120` вообще не строит Q для 13; DZ — freeforall |
| Required count | `Count()` ненулевых токенов Q — **не зашит в клиенте/движке**, определяется составом payload'а | то же | то же |

`members/numSlots` НЕ различает режимы для найденного матча (party-настройка, 36.D). **Ошибочно** было бы заключить "три режима требуют одинакового числа fake players": `maxplayers` разные (10/4/16), но движок **не требует**, чтобы roster был равен `maxplayers` — он считает то, что в payload. "Похожее на retail" наполнение — 9 / 3 / до 15 fake (при 1 реальном), но это **выбор теста**, а не подтверждённое условие retail. Party-флаг "full" (`UpdateAggregateMembersSettings`, `mm_title_gamesettingsmgr.cpp:1197-1200`): `numSlots = 5`, для `cooperative` = 2 — только party, не матч.

### 36.I Итоговая подтверждённая цепочка (реальные функции)

```
Real Player (Play)
  → CLobbyMenuSingleton::vfn2 sub_103F6240 [client]: mmqueue=registering; MatchmakingStart(account_ids ← members/machine*/player0/xuid)
  → GC (наш ClientGC): 9101; [опц.] 9104 matchmaking≠0,≠4 ⇒ client пишет mmqueue=searching
  → GC/backend → сервер: ReserveServerForQueuedGame("Q<cookie>,<matchid>,<n>:[real][fake…]")   [CBaseServer::SetReservationCookie → m_arrReservationPlayers]
  → GC: 9107 (game_type ∈ {8,10,13}) ⇒ client sub_103DC4B0: mmqueue=reserved; CServerConfirmedReservationCheckCallback(stage=1, mode=0)
  → client → server: 0x21(stage=1, steamid=real)        ;  fake accounts → server: 0x21(stage=1, steamid=fake_i)   [UDP, harness]
  → server sub_101BC1D0: awaiting=0 ⇒ 0x25(awaiting=0,total=N) всем roster
  → client sub_103DF430: status 4 & stage 1 ⇒ popup_accept_match_found; RaiseReadyUp(true,0,total) [sub_1057FF90, id word_10D54BF8]
  → Real Accept: LobbyAPI.SetLocalPlayerReady → sub_10580C20 → sub_103DF3A0: stage=2, 0x21(stage=2, steamid=real)
  → server: awaiting = #fake со stage<2 ⇒ 0x25(awaiting=k,total=N) ⇒ client RaiseReadyUp(true, N−k, N)   (счётчик попапа — retail)
  → fake accounts → server: 0x21(stage=2, steamid=fake_i)                                            [UDP, harness]
  → server: awaiting=0 ⇒ 0x25(awaiting=0) всем ⇒ client sub_103DF430: stage 2 & status 4 ⇒ (см. оговорку) QueueConnect / ServerReserved
```
**Оговорка по последнему звену (NOT PROVEN)**: для Accept-ветки ctor получает `mode=0` (`this+132==0`), а `sub_103DF430` при `mode==0` делает `return` до формирования `QueueConnect` — то есть **финальный connect для accept-режимов, вероятно, запускается ВТОРЫМ 9107 (не-accept `game_type`, ctor `(stage 2, mode 2)`) либо через `sub_103DEA80(kv,2)`**, а не самим stage-1 callback'ом. project446 подтверждает второй вариант (второй 9107 с `game_type=0` после "all players accepted"). Точная retail-последовательность финального звена не доказана — **главный открытый вопрос перед дизайном harness** (36.J).

### 36.J Нерешённое (честно)

- **Финальный триггер QueueConnect в accept-режимах**: второй 9107 (`game_type=0`, ctor `(2,2)`) vs `sub_103DEA80(kv,2)` после stage 2 — по коду `mode=0` не даёт QueueConnect из `sub_103DF430`; нужен либо RE `sub_103DFBE0`/`sub_103DEA80` mode-семантики до конца, либо живой тест.
- Какой `reason` шлёт retail-JS Accept-кнопки (нужна распаковка Panorama `code.pbin`).
- Кто и с каким периодом периодически вызывает `sub_103F6240` (heartbeat).
- Как retail-`server.dll` принимает 9105 (`GC2ServerReserve`): найден только `ClientJob_EMsgGCCStrike15_v2_GC2ServerReservationUpdate` (9142 — счётчики зрителей, к Accept не относится) и RTTI `CProtoBufMsg<MatchmakingServerReservationResponse>` (server→GC 9106); сам обработчик 9105 по RTTI не найден.
- Danger Zone: как строится Q-roster (`freeforall`), team-split.
- Смена cookie: `SetReservationCookie` пересобирает roster только при `cookie != текущий`, а `ReserveServerForQueuedGame` для `Q` отвергает reservation с другим cookie, пока сервер `IsReserved()` (source `baseserver.cpp:~4199-4215`) — важно для многоматчевого сценария (связано с §34, не чинится).

### Итог раунда (одним абзацем)

Источник истины participant/accept-state в retail-модели — **roster `CBaseServer::m_arrReservationPlayers` на game-сервере**, наполняемый `[%x]`-токенами `Q`-payload'а (`SetReservationCookie`), и **per-player `reservationStage`**, который меняют только входящие `A2S_RESERVE_CHECK`; клиент лишь показывает `awaiting/total` из ответов 0x25 (`RaiseReadyUp(bool,int,int)` = `sub_1057FF90`, id `word_10D54BF8`) и переводит **свой** stage 1→2 через `LobbyAPI.SetLocalPlayerReady` (`sub_103DF3A0`). Retail `server.dll::ReportGCQueuedMatchStart` — заглушка `return 0`. Минимально безопасная точка для fake participants — **`ServerGC` в `srcds`**: `Q`-payload с [real+fake] аккаунтами и UDP-0x21 от fake-account'ов (stage 1 и 2), без fake GC-клиентов и без изменения Accept/required/QueueConnect/Panorama/protobuf. Оговорка: финальный connect в accept-режимах требует уточнения (36.J).

---

## 37. CHECKPOINT — полная консолидация §35 + §36 (для продолжения без потери контекста)

Этот раздел — **самодостаточный чекпоинт**: собирает ВСЕ подтверждённые результаты §35 и §36 (включая промежуточные находки), состояние проекта и открытые вопросы. Новых исследований при его записи не проводилось, код/сборка/деплой/Git не менялись. Детали и RVA — в §35/§36 выше; здесь — то, что нельзя потерять, плюс пункты, которые в §35/§36 были разбросаны или сформулированы кратко. Уровни: **CONFIRMED** (прямая декомпиляция/raw disasm/source), **HIGH**, **MEDIUM**, **TODO/NOT PROVEN**.

### 37.0 Состояние проекта на момент чекпоинта

- Ветка `experimental-native-mm`, **0 коммитов** сверх `2455dcf`; всё — рабочее дерево. Изменены: `csgo_gc/{CMakeLists.txt,config.cpp,config.h,gc_client.cpp,gc_client.h,gc_server.cpp,gc_server.h,gc_shared.h,platform.h,platform_unix.cpp,platform_windows.cpp,steam_hook.cpp}`, `examples/config.txt`, `RESEARCH_FINDINGS.md`; новые `test_mm.cpp/.h` (в DLL компилируются, **не вызываются**).
- Реализовано и **подтверждено live-тестом (Casual, клиент + отдельный `srcds`, 192.168.1.150:27016)**: клиентский reservation bridge (§31: `HostEvent::ReserveServerForQueuedGame` → `Hk_SteamAPI_RunCallbacks` → `DispatchReserveServerForQueuedGame` → `IVEngineServer` `VEngineServer023` vtable slot 149), серверный аналог (§33: `ServerGC::ReserveServerForOurCookie()` после `k_EMsgGCServerHello`, payload `G<cookie>,<cookie>,1:`, диспатч через `Hk_SteamGameServer_RunCallbacks`), fallback `SteamGameServer014→010` (§33.8, флаг `s_steamGameServerIsV010`, `BLoggedOn` slot 8 в 011-014 vs slot 2 в 010). Первый матч Casual доходил до `srcds` и подключал клиента.
- Известная неисправленная проблема (**§34, НЕ чинится**): `ReserveServerForOurCookie()` вызывается **один раз** за жизнь `srcds` → повторный матч даёт "Failed to connect to the match". Минимальная точка исправления (не реализована): повторный/периодический вызов из `Hk_SteamGameServer_RunCallbacks`.
- Сборка: `build_local.bat` (ninja, MSVC x86, `x86-windows-static`); `srcds` — отдельно `cmake --build Build\build_ninja --target srcds`. Артефакты в `Build/release/` (`csgo_gc/csgo_gc.dll`, `srcds.exe`, `csgo.exe`). Runtime-игра: `C:\Program Files (x86)\Steam\steamapps\common\csgo legacy` (DLL в `csgo_gc\csgo_gc.dll`, config `csgo_gc\config.txt` — секция `matchmaking{test_server_address,test_server_port}`); лаунчер грузит `<exe_dir>\csgo_gc\csgo_gc.dll` и зовёт `InstallGC(dedicated)`.
- RE-инфраструктура (scratchpad `...\scratchpad\re\`): `rh.py` (helper), скрипты `q36_a..x.py`, базы `.i64`: `client.dll`, `client_panorama.dll`, `engine.dll`, `p446_csgo_gc.dll` (были) + **новые** `matchmaking.dll.i64`, `server.dll.i64` (снято в §36).

---

### 37.1 Accept-требование по режимам — матрица (§5/§9/§35)

Клиентская проверка `sub_103D7640` (client_panorama RVA `0x103D7640`, `bool(gameType)`), результат **true = Accept нужен**:

| eGame | режим | Accept |
|---|---|---|
| 4 | ArmsRace | НЕ нужен |
| 5 | Demolition | НЕ нужен |
| 6 | Deathmatch | НЕ нужен |
| **7** | **Casual** | **НЕ нужен** |
| **8** | **Competitive** | **нужен** |
| 9 | Cooperative | нужен |
| **10** | **Wingman** (`scrimcomp2v2`) | **нужен** |
| 11 | ScrimComp5v5 | нужен |
| 12 | Skirmish | НЕ нужен |
| **13** | **Danger Zone** (`freeforall/survival`) | **нужен** |

- Тот же набор `{8,9,10,11,13}` используется в **9107-хендлере** (`sub_103DC4B0`/`sub_103F49F0`): `v10 = reservation.game_type & 0xF; v10==9||13||11||(v10-8)==0||(v10-8)==2` → accept-ветка. **Согласованность**: `sub_103D7640` и 9107-хендлер совпадают; расхождение с project446 GC-стороной (`sub_100694A0`: `{8,10,13}`, §9) — это **их** (более узкий) выбор, не расхождение самого retail-клиента (для 9/11 project446 не реализует accept-цикл; **MEDIUM**, не доказано намерение).
- `game_type` в 9101/9107 = `(eGame & 0xF) | (eMapGroup << 8)`; live-подтверждено: `game_type=519` → `eGame=7`, `eMapGroup=2`=`mg_dust247`.
- Gametype/gamemode строки в `QueueConnect` KV по `this+92` (`sub_103DF430`): 4→gungame/gungameprogressive, 5→gungame/gungametrbomb, 6→gungame/deathmatch, 7→classic/casual, 8→classic/competitive, 9→cooperative/cooperative, 10→classic/**scrimcomp2v2**, 11→classic/scrimcomp5v5, 12→skirmish/skirmish, 13→freeforall/**survival**, иначе unknown/unknown.
- `maxplayers` (retail `csgo/gamemodes.txt`): casual 20, competitive **10** (стр.96), scrimcomp2v2 **4** (183), scrimcomp5v5 10, deathmatch 16, skirmish 12, survival **16** (781). **Не** являются "required count" (см. 37.4).

### 37.2 Accept UI / native-мост (client_panorama.dll) — §5/§35/§36

- **Попап**: `sub_10418AC0("popup_accept_match_found", 0)` и `("popup_accept_match_confirmed", 0)` вызываются из `sub_103DF430` (RVA `0x103DF430`); для live-join (accept не нужен) — `popup_accept_match_found` + строка `"@<map>"`. JS/XML (`popup_accept_match.xml/.js`, `PartyMenu.ShowMatchAcceptPopUp`) **не распакованы** (Panorama `code.pbin`) — как отдельные символы **не найдены**; нативная сторона попапа — `sub_10418AC0`. **TODO/NOT PROVEN**: что именно рисует попап (имена/аватары участников или только счётчик).
- **`LobbyAPI.SetLocalPlayerReady(reason: string)`** (описание `"Tell matchmaking this player is ready to play a queued match (which leave peanalties)."` @ `0x10bbd940`): цепочка `sub_10583500` (`0x10583500`, V8 FunctionTemplate, параметр `reason`) → **`sub_10584F70`** (`0x10584F70`, JS-маршалинг + `sub_10512130` проверка объекта) → **`sub_10585B00`** (`0x10585B00`, пишет `this+12` ← `dword_15280F00` vtbl+516 +76/+80 в зависимости от результата) → **`sub_10580C20`** (`0x10580C20`, reason-классификатор): `reason=="deferred"` → `sub_103DF3A0(dword_15278690)`; иначе → `sub_103DEA80(0,2)`.
- **Вывод (CONFIRMED)**: `SetLocalPlayerReady` работает **только с локальным игроком** (нет параметра account/участника). API "пометить участника B ready" в клиенте **не существует**.
- `LobbyAPI` регистрируется `sub_1057F2D0` (`0x1057F2D0`), класс `CUiComponent_Lobby`, таблица методов `sub_1057F1E0` (21 запись); `CUiComponent_Lobby` ctor `sub_1057F440`, содержит `IMatchEventsSink` (+4; OnEvent = `sub_1057F9F0`, обрабатывает `OnMatchSessionUpdate/OnPlayerRemoved/OnPlayerUpdated/OnPlayerLeaderChanged/ScaleformComponent_GC_Hello`, шлёт Panorama-события PlayerJoined/Removed/Updated/LeaderChanged с xuid — **party-события**) и `CLobbyMenuSingleton` (+8).
- Отсутствие client-side `required_count` / `accepted_count` — **CONFIRMED** (нет такой переменной ни в client.dll, ни client_panorama.dll, ни в обоих source-деревьях: grep `required_count|accepted_count|needed_count` — 0 совпадений; единственный близкий эквивалент — KV `members/numPlayers|numSlots`, это party).

### 37.3 Reservation callback — stage/mode и Accept → 0x21 stage=2 (§36.E)

- `CServerConfirmedReservationCheckCallback` ctor **`sub_103DF1C0(this, kv, a3=stage, a4=mode)`** (client_panorama `0x103DF1C0`; client.dll-аналог `sub_103F7980`): `this+140 = a3` (**stage**), `this+132 = a4` (**mode**), `this+92` = game_type, `this+128` = handle сетевого запроса (out-параметр), лог `"Server reservation check %p heartbeating"`, регистрация в **`INETSUPPORT_003` vtbl+60** с `a3` как **reservationStage** (5-й аргумент).
- **Выбор stage/mode в 9107-хендлере** (`sub_103DC4B0` client_panorama / `sub_103F49F0` client.dll): `v10 = (*(nested reservation +32)) & 0xF`; **nested `reservation` отсутствует** (`*(v31+32)==0`) → fallback на глобальный search-state `dword_10D9A180+32` (client_panorama) — т.е. наш 9107 без `reservation` использует `game_type` исходного поиска. accept-режим (`{8,9,10,11,13}`) → `sub_103DF1C0(kv, **1**, **0**)` (stage 1, mode 0); иначе → `sub_103DF1C0(kv, **2**, **2**)` (stage 2, mode 2, direct connect; Casual). Reconnect-ветка `sub_103DDCB0`/`sub_103F6240` → `sub_103DF1C0(kv, 2, 1)`.
- **`sub_103DF3A0`** (client_panorama `0x103DF3A0`, 132 байта) — вызывается из `sub_10580C20` при `reason=="deferred"` — **единственное место, меняющее stage**: `if (cb.stage(+140) >= 2) return 0; cb.+35dw(stage)=2; if (cb.+128 handle) handle->vfunc+20 (отменить прежний запрос); INETSUPPORT_003 vtbl+60(this+24, +40, +44, stage=2, cb, cb+128)` — **перерегистрация сетевого запроса резервации со stage=2**. Следствие: **Accept → новые `A2S_RESERVE_CHECK (0x21)` с `reservationStage=2` и SteamID клиента** (**CONFIRMED** код; **HIGH**, что именно эта ветка = кнопка Accept; **MEDIUM**: какой `reason` передаёт retail-JS — не распаковано; ветка `else` → `sub_103DEA80(0,2)`).
- **`sub_103DF430`** (net-callback, `a2` = `CServerMsg_CheckReservation*`, условие `a2 == this+128`; `vfunc+4(a2)` = status, `4` = "все подтвердили"; `vfunc+8(a2)` = 64-бит, из которых `byte0`=awaiting, `byte1`=total — **HIGH**, порядок байт не сверен побайтно):
  - status "ещё нет" и `stage==2` → `RaiseReadyUp(true, accepted, total)`, `accepted = clamp(byte1 − byte0, 0, 64)`, `total = byte1` (**счётчик попапа "x/N принято"**);
  - status 4 и `stage==1` → `DevMsg("Server reservation check %p ready-up!")`, `popup_accept_match_found`, `RaiseReadyUp(false,0,total)` (если `!byte_1519DDEE`) и `RaiseReadyUp(true,0,total)`;
  - status 4 и `stage!=1`: `if (!this+132) return;` (mode 0 — выход **без QueueConnect**), иначе `"queue connect"`: `popup_accept_match_confirmed` (accept-режим) либо `popup_accept_match_found`+`@map`, затем **KV `QueueConnect`** (`adronline`, `reservationid` (64-бит `this+40/44`), `helper_pSession`, `helper_time` (+2000 мс при live-join), `auto_close_session` (0 если `members/numMachines>1`), `map`, `gametype`, `gamemode`) → `sub_103DEA80(kv, mode)`;
  - иначе `"will not queue connect"` (fallback-ветка, §20/§21).

### 37.4 ReadyUpForMatch / ServerReserved — native→Panorama (§36.B/C)

- **`sub_103D8AF0`** (client_panorama `0x103D8AF0`, 90 байт; client.dll `sub_103F0C70`): **ленивый singleton-getter `CLobbyMenuSingleton`**; глобал `dword_15278634` (объект `CUiComponent_Lobby`, 12432 байт, аллокация через `g_pMemAlloc`, ctor `sub_1057F440`); возвращает `obj+8`, **только если `byte_1519DDEE`** (Panorama-лобби включено). 5 caller'ов: `sub_103D9900` (9104, `0x103d9d5b`) ×1 и `sub_103DF430` ×4 (`0x103df4a0`, `0x103df581`, `0x103dfb9e`, `0x103dfba7`). Слот 0 подобъекта (vtable `CUiComponent_Lobby` `0x10bbdf60`) = **`sub_1057FF90`**.
- **`sub_1057FF90(this, bool a2, int a3, int a4)`** (`0x1057FF90`, 100 байт) = **`RaiseReadyUp(bool,int,int)`**: `this+16 = a2 ? Plat_MSTime() : 0`; при наличии Panorama-слушателя события `word_10D54BF8` (`dword_15280F00` vtbl+216) диспатчит `panorama::CUIEvent3<bool,int,int>` (конструктор `sub_1057F030`, vtbl+208).
- **`sub_10581640`** (`0x10581640`) = **регистрация (определение схемы)** события `"PanoramaComponent_Lobby_ReadyUpForMatch"`, id `word_10D54BF8`, дескриптор `v5=3`; вызывается только через таблицу трамплинов (`sub_100502C0`, data `0x10a98d30`). **Совпадение `word_10D54BF8` в регистрации и raiser'е ⇒ CONFIRMED**: `sub_10581640` — определение именно того события, которое поднимает `sub_1057FF90`.
- **Вызовы raiser'а**: `sub_103DF430` (см. 37.3) и 9104-хендлер при `matchmaking == 3` → `RaiseReadyUp(false,0,0)` (**сброс**, не показ — **исправляет §35.3 п.2**).
- **`ServerReserved`**: `sub_103E1390` (`0x103E1390`) регистрирует `"ServerReserved"`; **`sub_103D7750`** (`0x103D7750`) строит **`panorama::CUIEvent2<char const*,double>`** (`ServerReserved(const char* map, double time)`) для id **`word_10D501B4`**; raiser — **`sub_103DEA80(kv, mode)`** (`0x103DEA80`): при `mode==2` и наличии отложенного QueueConnect-KV (`dword_15278694`) берёт `map`, `sub_10496C10(map, skirmishmode)` и диспатчит `word_10D501B4`. **HIGH** (id совпадают, raiser найден; JS-слушатель не распакован). Связь с reservation state: событие поднимается из `sub_103DEA80`, вызываемой (а) `sub_103DF430` в конце ветки QueueConnect (`sub_103DEA80(kv, v29)`), (б) `sub_10580C20` (Accept, `(0,2)`), (в) `sub_103DEC40`/`dword_15278694`-механика отложенного connect — т.е. `ServerReserved` = "сервер зарезервирован, можно коннектиться", завязано на `dword_15278694` (KV `QueueConnect`) и `dword_15278690` (активный reservation-check callback).
- Значения `matchmaking` (proto field 1, 9104): `0`→очистка (Delete mmqueue, `cfg/qmmconnect.dt`), `3`→`RaiseReadyUp(false,0,0)`+`searching`, `4`→`connect`, прочие≠0→`searching`. Смысловые имена enum — **не восстановлены**.

### 37.5 `game/mmqueue` — writer и последовательность (§36.A)

- **`sub_103F6240`** (client.dll `0x103F6240`, 2956 байт; client_panorama-аналог `sub_103DDCB0`) = **`CLobbyMenuSingleton::vfn2`** (vtable client.dll `0x10bccbf0` slot2; client_panorama `0x10b6a410` slot2; slot3 = `sub_103F6DD0`/`sub_103DE630` — cancel/`MatchmakingStop`). Вызывается только через vtable.
  - **отправляет `MatchmakingStart`** (vftable `CProtoBufMsg<CMsgGCCStrike15_v2_MatchmakingStart>` xref `0x103f6d88`);
  - **пишет `registering`**, если `game/mmqueue` был пуст (`test al,al; cmovz` @ `0x103f6ab6-0x103f6ac0`, шаблон `"Update { game { mmqueue registering } }"` @ `0x10bcc5c0`), иначе **`heartbeating`** (`0x10bcc574`); обновляет `this[1]=Plat_MSTime()` (метод перевызывается периодически — **HIGH**, вызывающий таймер/период не найдены);
  - **`account_ids`** ← цикл по KV **`members/machine%d/player0/xuid`** (`sub_1094F4C0` GetUint64): сначала локальный SteamID (`dword_14DEDADC` vtbl+8), затем остальные machine-записи ≠ себя ⇒ **`members/` (party) → `MatchmakingStart.account_ids`**;
  - `game_type` ← `game/mapgroupname` (`"reconnect"`-ветка → `sub_103F1240`), `<<8`-композиция mapgroup; `client_version` ← `INETSUPPORT_003` vtbl+36 (13805); `prime_only` ← `game/prime`; flags ← `game/gamemodeflags`; турнирные `tm_event_id/tm_event_stage_id/tm_team_id_ct/tm_team_id_t` в KV `Game::EnteringQueue`; также создаёт `dword_152EACBC` (384-байт объект статус-UI) и `game/map` (`Update/game/map`).
- **Полная последовательность**: `#empty#` → **`registering`** (Play, `sub_103F6240`) → **`heartbeating`** (повторные 9101) → **`searching`** (9104 при `matchmaking∉{0,4}`, `sub_103F1C00`/`sub_103D9900`, raw `0x103f1ffd cmp [ecx+0A8h],4; cmovnz`) → **`reserved`** (9107, `sub_103F49F0`/`sub_103DC4B0`, также `sub_103F7C00`/`sub_103F6240`) → **`connect`** (9104 при `matchmaking==4`, и `sub_103F7C00` при status 4). Шаблоны `Update { system { lock … } game { mmqueue … } }` — только в **client.dll** (literals @ `0x10bcae78`(Delete), `0x10bcaee8`(searching), `0x10bcaf28`(connect), `0x10bcc020`(reserved), `0x10bcc574`(heartbeating), `0x10bcc5c0`(registering), `0x10bcc678`(Delete/game/mmqueuestop)).
- **Gate** 9104- и 9107-хендлеров: `game/mmqueue` **уже непуст**, иначе клиент сам шлёт `MatchmakingStop`. Поэтому наш Casual-тест без 9104 проходил: `registering` выставлен `sub_103F6240` на клике Play. **Связь 9104 → `searching`**: 9104 — writer `searching`/`connect` (не initial); §35.3 п.1 корректно: 9104 сам не может быть initial-setter'ом (gate), initial = `registering`.

### 37.6 `members/*` — party-KV (§35.7 + §36.D)

- KV-пути (client_panorama/client.dll — только **readers**): `members/numSlots`, `members/numPlayers`, `members/numMachines`, `members/numSpectators`, `numTSlotsFree/numCTSlotsFree`, `members/timeout`, `members/machine%d/player0/xuid`, `members/machine0/player0/game/offjoin`, `members:numSlots`.
- **Writers (CONFIRMED, matchmaking.dll)**: `sub_10024F70` (`0x10024F70`, `CMatchSessionOnlineHost::InitializeGameSettings`, caller `sub_10022720`) и `sub_1001BEA0` (`0x1001BEA0`, `CMatchSessionOfflineCustom::InitializeGameSettings`, caller `sub_1001B0A0`): `members{numMachines=1,numPlayers=1,numSlots=MAX(numSlotsCreated,numPlayers),machine0{id,flags,numPlayers,dlcmask,tuver,ping,player0{xuid,name}}}` из **локального player manager**; 1:1 с source `mm_session_online_host.cpp:1658-1700`, `mm_session_offline_custom.cpp:430`. Дальше — `mm_title_gamesettingsmgr.cpp:841-860` (`numSlots` по умолчанию 10; = `GetMaxPlayersForTypeAndMode` только для official-search `searchempty`&&`!game/hosted`), `:321` (online lobby 10 слотов), `sys_session.cpp:956` (join машины), чужие члены — Steam Lobby (`SysSession`).
- `sub_105807B0` (client_panorama vtable slot1 `CLobbyMenuSingleton`): `Members/numSlots==1` ⇒ "я хост/соло".
- **Вывод**: `members/` = **локальное party/session KV (до очереди)**, не состав найденного матча и не приходит из GC. Мост `reservation.account_ids (9107)` → `members/` — **не найден** (9107-хендлер использует только `&0xF`; полевой копировщик `sub_103DFBE0` не декомпилирован — **TODO/MEDIUM**).

### 37.7 GC-протокол — участники (§35.6, схема существующая, менять не нужно)

`CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (9107): `serverid(1) direct_udp_ip(2) direct_udp_port(3) reservationid(4) reservation(5, nested MatchmakingGC2ServerReserve) map(6) server_address(7)`. Вложенное **`CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve`** (то же, что 9105 на сервер): `account_ids(1, repeated uint32)`, `game_type(2)`, `match_id(3)`, `server_version(4)`, `flags(18)`, `rankings(5, repeated PlayerRankingInfo)`, `encryption_key(6)`, `encryption_key_pub(7)`, `party_ids(8, repeated uint32)`, `whitelist(9)`, `tv_master_steamid(10)`, `tournament_event(11)`, `tournament_teams(12)`, **`tournament_casters_account_ids(13)`** (↔ `{%x}` в Q-payload), `tv_relay_steamid(14)`, `pre_match_data(15)`, `rtime32_event_start(16)`, `tv_control(17)`. `CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate` (9104): `matchmaking(1,int32)`, `waiting_account_id_sessions(2)`, `error(3)`, `ongoingmatch_account_id_sessions(6)`, `global_stats(7)`, `failping/penalty/failready/vacbanned/…(8..18)` — **очередь-статус per-player**, не состав матча; `waiting_account_id_sessions` печатается как `"Matchmaking waiting for %d accounts (%X, ...)"` (**MEDIUM** трактовка).
- В **клиентском** 9107-хендлере `account_ids/party_ids/rankings` для Accept-логики **не используются** (только `game_type & 0xF`); что попап рисует по ним — **NOT PROVEN**.

### 37.8 Где реально считается accepted/required — game-сервер (§36.F) — **CONFIRMED**

- **`CBaseServer::ReplyReservationCheckRequest`** = engine.dll **`sub_101BC1D0`** (`0x101BC1D0`, 2189 байт; `"Reservation from client %u: %u"` @ `0x101bc533`, `"Match start status: %u/%u"` @ `0x101bc69e`, awaiting-дефолт `127`), source `engine/baseserver.cpp:2165-2308`.
- Roster **`m_arrReservationPlayers`** (`QueueMatchPlayer_t{m_uiAccountID, m_adr, m_uiToken, m_uiReservationStage}`) строит **`SetReservationCookie`** (`baseserver.cpp:4217-4296`) **только если cookie ≠ текущего** и `sv_mmqueue_reservation` начинается `Q<cookie_hex>,`: по каждому токену `[%x]` с **ненулевым** account → игрок (stage 0, token 0); `m_numGameSlots = Count()`; для `G` roster очищается, slots=0.
- **`CSGameRules::m_mapQueuedMatchmakingPlayersData`** (`cs_gamerules.cpp:4631-4653`, `cs_gamerules.h:1226-1235,1326-1334`; включён при `sv_mmqueue_reservation[0]=='Q'`, `cs_gamerules.cpp:18604`): игровая логика (game DLL) строит **свой** roster `CQMMPlayerData_t{m_uiPlayerAccountId, m_iDraftIndex}` из тех же `[%x]`; **`m_iDraftIndex` = позиция токена, считая `[0]`**; команда = `draftIndex < kNumPlayersPerSide` (`cs_gamerules.cpp:18383`), отсюда padding `[0]` у project446 (§28.5: cap 5 Competitive / 2 Wingman). Используется 30+ мест в `cs_player.cpp` (per-round стата по account_id). Dead-code блок `if(0 && ...)` `cs_bot_manager.cpp:1230-1267` — "бот подменяет ещё не подключившегося зарезервированного игрока" (`SetHumanPlayerAccountID`, имя `"BOT %u"`) — **навсегда отключён**, работает ПОСЛЕ формирования матча, не про Accept.
- **Обработка 0x21**: условия `cookie==m_nReservationCookie`, `stage!=0`, `steamid!=0`, `sv_mmqueue_reservation[0]=='Q'`; игрок ищется по `CSteamID(steamid).GetAccountID()`; если найден → сохраняются `adr`,`token`, `stage=stage`; **`awaiting = #(stage < запрошенного)`, `total = Count()`**; если аккаунта нет в roster → `awaiting=127,total=0` (status 4 не придёт). Для `G`: `awaiting=0` всегда (**поэтому Casual/`G` работает**). Ответ `0x25`: `header,0x25,hostVersion,token,stage,awaiting(byte),total(byte)`; **при `awaiting==0` сервер рассылает 0x25 ВСЕМ игрокам roster'а** на сохранённые `adr/token`.
- **`serverGameDLL->ReportGCQueuedMatchStart(minStage, confirmedAccounts[], count)`** — engine.dll вызывает `(*(dword_139097E8 vtbl + 188))` (slot **47**). Встроенный **spoof auto-accept** `bSpoofForcefulConnect = (awaiting && bReady && stage==2)`. **В retail `server.dll` 2021**: `CServerGameDLL` vtable `0x1085cb10`, slot 47 = **`sub_10145510` (`0x10145510`, 5 байт): `char __stdcall(int,int,int){return 0;}`** ⇒ `bReady==false`, spoof **не срабатывает**, GC о подтверждениях не уведомляется (**CONFIRMED**). Spoof покрывает **только stage 2**; stage 1 (показ попапа) он не покрывает.
- Клиентская интерпретация ответов — 37.3 (`RaiseReadyUp(true, total−awaiting, total)`).
- Retail `server.dll` содержит job `ClientJob_EMsgGCCStrike15_v2_GC2ServerReservationUpdate` (9142, **только счётчики зрителей**, не Accept) и `CProtoBufMsg<MatchmakingServerReservationResponse>` (9106); обработчик 9105 по RTTI **не найден** (TODO).
- **Важное ограничение для многоматчевости (связано с §34)**: `ReserveServerForQueuedGame` для нового `Q` с другим cookie отвергается, пока сервер `IsReserved()` (source ~`baseserver.cpp:4199-4215`); roster пересобирается только при смене cookie; повторный вызов с тем же cookie ничего не пересобирает и не логирует `-> Reservation cookie` (виден в client console.log: лог движка был только у первой попытки).

### 37.9 project446 — модель backend-side Accept (§6/§28/§36.F, повторно проверено)

- **`sub_100789A0`** (`0x100789A0`, 1426 байт, 9102-handler / Accept state machine по `this+1568`): для accept-режимов `"Matchmaking: Player ACCEPTED, waiting for all players"` → **форвардит сигнал наружу** `sub_100BFC20(9102,…)` (20+ call sites) и остаётся в waiting; **подсчёта "все приняли" в DLL нет — на их backend** (**CONFIRMED**). Accept-инжектор `sub_100787F0`: `"Accept hook: injecting MatchmakingStop(abandon=0) — legacy client omits 9102 on Accept"` ⇒ retail-клиент **не шлёт 9102 при Accept** (Accept идёт как 0x21 stage 2 на сервер, 37.3). `sub_100694A0` (`NeedsAccept`): `{8,10,13}`.
- **`sub_1006EAD0`** (`0x1006EAD0`, reserve-handler): `"[GC] Match found! server=… game_type=…"` (ready-up reserve) → state 2; **второй 9107 с `game_type==0`** → `"[GC] Connect reserve received (all players accepted)"` → state 3 → `sub_100774E0(…)` (proceed). Также `"Duplicate reserve resId=%llu ignored (ready-up active)"`, `"Delayed ready-up reserve ignored after local search cancel"`, `"Ignoring spurious matchmaking=0 during active ready-up/connect"`.
- **`sub_100EA660`** — нативно выставляет счётчик попапа `"[ReadyUp] popup counter set to %d/%d"` (`min(x,total)/total`), через RVA-параметры в `client.dll` (`GC_CLIENT_READYUP_ACTIVE_RVA`, `GC_CLIENT_READYUP_COMP_RVA`), т.е. вызывает эквивалент `RaiseReadyUp`; `sub_100F5D00` — обработка ReadyUp counter/cancel событий backend'а (`"[ReadyUp] ignored malformed counter event (%zu bytes)"` — событие 12 байт, `"ignored stale status for reservation %llu"`, `"cancelled native reservation check %p (kind=%u stage=%u)"`, `"native connect armed (check=%p kind=%u stage=%u)"` — `kind`≙`this+132`(mode), `stage`≙`this+140`). Хук `sub_100BADE0` ставит vtable-хук на `ReportGCQueuedMatchStart` (slot 47 default, env `GC_VTBL_REPORT_MATCH_START`), т.к. в retail это `return 0`-заглушка.
- Согласуется с клиентским 9107-хендлером: 9107 с не-accept `game_type` (например 0) создаёт callback `(stage 2, mode 2)` = direct connect.

### 37.10 Режимы Competitive / Wingman / Danger Zone (§35.10/§36.H) — сходства и различия

- **Сходство (CONFIRMED)**: клиентская Accept-машина состояний одна (stage 1 → popup → Accept → stage 2 → connect); серверный accounting одинаков (`CBaseServer`, Q-roster); `sub_103D7640`/9107-хендлер не различают 8/10/13 по счёту.
- **Различия**: `maxplayers` 10/4/16; team-cap padding у project446 5/2 (`sub_10094120`, только 8 и 10; для 13 Q **не строится** — DZ NOT PROVEN); `gamemode` строки в QueueConnect (`competitive`/`scrimcomp2v2`/`survival`); DZ — `freeforall`, team-split не подтверждён. **Required count не зашит** ни в клиенте, ни в движке — это `Count()` ненулевых токенов Q. **Ошибочный вывод "все три режима требуют одинакового числа fake players" не подтверждён и не делается**; "похожие на retail" 9/3/до 15 fake (при 1 real) — выбор теста, не условие retail.

### 37.11 Fake GC clients — гипотеза и опровержение; test-only fake participants (§35.12/§36.G)

- **Исходная гипотеза (задание §36 п.9)**: fake players требуют отдельных виртуальных GC clients/соединений. **Опровергнуто (CONFIRMED по коду/source)**: участие в матче определяется **(а) токеном `[account]` в Q-payload'е сервера** и **(б) UDP-пакетами `A2S_RESERVE_CHECK` с этим account в SteamID** (сервер проверяет лишь ненулевой steamid, cookie, stage; Steam-auth/GC-сессия в этом пути не нужны); GC-уровень (наш `ClientGC`) — один, он сам является "backend'ом". **Отдельные GC connections для fake players не нужны.**
- **Test-only fake participant concept (архитектура, НЕ реализовано)**: `ServerGC` (в `srcds`, уже владеет `ReserveServerForQueuedGame`, §33): (1) строит **`Q<cookie>,<matchid>,<n>:[real][fake…]`** (сейчас `G`), (2) от имени fake account'ов шлёт на свой UDP-порт 33-байтные 0x21 (формат §26): **stage 1** (чтобы `awaiting` на stage 1 стал 0 → клиент покажет попап) и **stage 2** (после реального Accept). Не трогаем: Accept-матрицу, required-count, Panorama, protobuf, QueueConnect, `SetLocalPlayerReady`. Ограничение: **`ServerGC` должен знать account реального игрока** (иначе его 0x21 → `awaiting=127`); `ClientGC`/`ServerGC` — разные процессы; вариант — test-only параметр в server `config.txt`. Со стороны `ClientGC` для accept-ветки достаточно `reservation.game_type∈{8,9,10,11,13}` (или отсутствие nested `reservation` + search game_type).
- **Не** годятся: `required_count=1`, ручная подстановка `accepted_count`, вызов `OnAccept`/`QueueConnect` напрямую, отключение попапа, ручной stage-write в `m_arrReservationPlayers` (memory patch).

### 37.12 Итоговая подтверждённая цепочка (реальные функции)

```
Play → CLobbyMenuSingleton::vfn2 sub_103F6240: mmqueue=registering; 9101(account_ids ← members/machine*/player0/xuid)
 → GC(ClientGC): [9104 matchmaking≠0,4 ⇒ mmqueue=searching (sub_103F1C00)]
 → сервер: ReserveServerForQueuedGame("Q<cookie>,<id>,<n>:[real][fake…]") ⇒ SetReservationCookie ⇒ m_arrReservationPlayers
 → GC: 9107 (game_type∈{8,9,10,11,13}) ⇒ sub_103DC4B0: mmqueue=reserved; callback sub_103DF1C0(stage=1,mode=0)
 → client 0x21(stage1) + fakes 0x21(stage1) ⇒ server sub_101BC1D0: awaiting=0 ⇒ 0x25 всем
 → sub_103DF430: status4&stage1 ⇒ popup_accept_match_found; RaiseReadyUp(true,0,total) [sub_1057FF90, word_10D54BF8]
 → Accept: LobbyAPI.SetLocalPlayerReady → sub_10580C20 → sub_103DF3A0: stage=2; INETSUPPORT_003 vtbl+60 ⇒ 0x21(stage2, real)
 → server: awaiting=#fake stage<2 ⇒ 0x25(awaiting,total) ⇒ RaiseReadyUp(true,total−awaiting,total)   [счётчик попапа]
 → fakes 0x21(stage2) ⇒ awaiting=0 ⇒ 0x25 всем ⇒ sub_103DF430 stage2&status4 ⇒ QueueConnect KV / ServerReserved (sub_103DEA80, word_10D501B4)
```
**Оговорка (NOT PROVEN, главный открытый вопрос перед дизайном harness)**: в accept-ветке ctor даёт `mode=0`, а `sub_103DF430` при `this+132==0` возвращается до формирования QueueConnect. Финальный connect, вероятно, запускается **вторым 9107** (не-accept `game_type`, ctor `(2,2)`) — так делает project446 ("Connect reserve … all players accepted") — либо `sub_103DEA80(kv,2)`; retail-последовательность точно не доказана (нужен RE `sub_103DFBE0`/`sub_103DEA80` или live-тест).

### 37.13 Unresolved (сводно §35 + §36)

1. Финальный триггер QueueConnect в accept-режимах (второй 9107 vs `sub_103DEA80(kv,2)`), см. 37.12.
2. Какой `reason` шлёт retail-JS Accept-кнопки (`"deferred"`?), `popup_accept_match.js/.xml`, `PartyMenu.ShowMatchAcceptPopUp` — не распакованы.
3. Что попап показывает (имена из `reservation.account_ids`? только счётчик?).
4. Кто и с каким периодом вызывает `sub_103F6240` (heartbeat 9101).
5. Полное enum-значение `matchmaking` (кроме 0,3,4).
6. Идентичность порядка байт `awaiting/total` в `a2->vfunc+8()` (HIGH, не побайтно).
7. `sub_103DFBE0` (полевой копировщик 9107) — копирует ли `reservation.account_ids` куда-либо.
8. Кто ПИШЕТ `members/` при входе чужих участников (Steam Lobby / `SysSession`) — на уровне binary не разбирался.
9. Danger Zone: как строится Q-roster, team-split; project446 для 13 Q не строит.
10. Обработчик 9105 в retail `server.dll` (RTTI не найден); что делает retail `server.dll` при `ReserveServerForQueuedGame` помимо engine-части.
11. Назначение `cfg/qmmconnect.dt` (пишется/читается 9104-хендлером при `matchmaking==0`).
12. Смена cookie между матчами / `Unreserve` (§34) — не чинится в этом цикле.
13. `sub_103D7640` vs project446 `{8,10,13}` — смысл расхождения (MEDIUM: их выбор).

### 37.14 Ограничения, действовавшие в §35/§36

READ-ONLY; без изменения кода/сборки/деплоя/Git; без protobuf/Panorama/QueueConnect-изменений; без обхода Accept/required-count; без исправления reservation (§34). Резервный следующий шаг (только после явного решения пользователя): закрыть п.1 из 37.13 (RE или live-тест), затем — дизайн test-only harness в `ServerGC` (Q-payload + UDP 0x21).

### 37.15 Исправления к §35 (явный список; старый текст §35 не удалялся)

| # | Что утверждалось в §35 | Исправление (источник — §36 / 37.x) | Уровень |
|---|---|---|---|
| 1 | §35.3 п.2: 9104 при `matchmaking==3` **показывает ReadyUp** | **Ошибочно.** 9104-хендлер при `matchmaking==3` вызывает `RaiseReadyUp(false,0,0)` (`sub_1057FF90`, id `word_10D54BF8`) — т.е. **закрывает/сбрасывает** ready-up и переводит состояние дальше (`searching`). Показ попапа делает `sub_103DF430` (status 4 + stage 1). См. 37.4. | CONFIRMED |
| 2 | §35.1 (уточнение): accepted/required учёт в backend/клиенте | **Учёт — на game-сервере** (`CBaseServer::ReplyReservationCheckRequest`, engine `sub_101BC1D0`, `m_arrReservationPlayers`, per-player `m_uiReservationStage`); клиент лишь отображает `awaiting/total` из 0x25. Client-side `required_count`/`accepted_count` **не существует** (CONFIRMED, 37.2). project446 форвардит Accept на свой backend (37.9), retail — нет. | CONFIRMED |
| 3 | Гипотеза: fake players = отдельные GC clients | **Опровергнута** (37.11): fake participant = токен `[account]` в Q-roster + UDP `0x21` stage 1/2 с этим account в SteamID. Отдельные GC-соединения не нужны. | CONFIRMED |
| 4 | Возможное смешение `members/numSlots` с составом матча | **Не смешивать.** `members/*` (writers `matchmaking.dll` `sub_10024F70`/`sub_1001BEA0`, `InitializeGameSettings`) = локальный party/player-manager состав **до** очереди. Состав найденного матча = Q-roster на сервере. Writer, который переносил бы `reservation.account_ids` в `members/`, **не найден** (`sub_103DFBE0` не декомпилирован). См. 37.6. | CONFIRMED (writers) / TODO (мост) |
| 5 | `maxplayers` (10/4/16) как ориентир roster size | `maxplayers` **не** равен размеру reservation roster; required count = `Count()` ненулевых токенов Q. Для Danger Zone Q-roster в project446 не подтверждён (37.10). | CONFIRMED / UNRESOLVED (DZ) |

### 37.16 Быстрый чеклист «что сохранено» (для сверки будущим Claude)

- §35: матрица 8/9/10/11/13 vs 4/5/6/7/12 → 37.1; `popup_accept_match*`, `PartyMenu.ShowMatchAcceptPopUp`, `LobbyAPI.SetLocalPlayerReady`, `sub_10583500→sub_10584F70→sub_10585B00→sub_10580C20`, local-player-only, no client counters → 37.2; `MatchmakingGC2ServerReserve` (`account_ids/party_ids/rankings/tournament_casters_account_ids`) → 37.7; `m_mapQueuedMatchmakingPlayersData` → 37.8; project446 `sub_100789A0` + backend-side forwarding → 37.9; Comp/Wingman/DZ → 37.10; fake GC clients гипотеза/опровержение, test-only fake participant → 37.11; unresolved → 37.13.
- §36: `game/mmqueue` + `sub_103F6240` (`registering`/`heartbeating`/`account_ids` ← `members/machine*/player0/xuid`/последовательность/9104→`searching`) → 37.5; `sub_103D8AF0` + slot0 `sub_1057FF90`=`RaiseReadyUp(bool,int,int)` → 37.4; `sub_10581640` + `word_10D54BF8` CONFIRMED → 37.4; `sub_103DF3A0` stage=2, `INETSUPPORT_003`, Accept→0x21 stage=2 → 37.3; аргументы ReadyUp ← reservation-check / 0x25 / `awaiting,total` → 37.3, 37.8; `sub_103D7750` `ServerReserved(const char*,double)` → 37.4; 9107 `game_type` → выбор stage/mode → 37.3; `CBaseServer::ReplyReservationCheckRequest` `0x101BC1D0`, `m_arrReservationPlayers`, stage, `[%x]`-токены, zero skip, awaiting/total → 37.8; `ReportGCQueuedMatchStart` server.dll slot 47 `0x10145510` = `return 0` → 37.8; fake players как roster+0x21 → 37.11/37.12; `members/numSlots` writers → 37.6; исправление §35 → 37.15.

### 37.17 CURRENT NEXT STEPS

Только подтверждённые открытые вопросы (детали — 37.13):

1. **Финальный Accept → QueueConnect path.** В accept-режимах stage-1 callback создаётся с `mode=0`, `sub_103DF430` при `this+132==0` выходит до QueueConnect. Что именно запускает connect после stage 2: второй 9107 с не-accept `game_type` (как у project446) или `sub_103DEA80(kv,2)` — **не доказано**. Требуется RE `sub_103DFBE0`/`sub_103DEA80` или live-тест.
2. **Как сервер идентифицирует real player при `0x21`.** Известно (CONFIRMED): сервер берёт `CSteamID(steamid).GetAccountID()` из пакета и ищет в `m_arrReservationPlayers`; если аккаунта нет в roster — `awaiting=127,total=0`. Неизвестно: откуда `ServerGC` (отдельный процесс `srcds`) получит account реального игрока для Q-payload (config-параметр / другой канал).
3. **Как именно реализовать fake-player stage=2.** Концепт есть (37.11): UDP `0x21` stage 2 от fake-аккаунтов после реального Accept; но как `ServerGC` узнаёт момент реального Accept (stage 2 от real виден серверу через `m_uiReservationStage`), в каком потоке и как формировать пакеты (формат §26) — не спроектировано.
4. **Danger Zone Q-roster.** Как строится Q для eGame 13 (размер, team-split, `[0]`-padding) — не подтверждено; project446 для 13 Q не строит.
5. **Cookie lifecycle / повторная reservation** (§34) — **отложено до server/backend этапа**; сейчас не чинится.

**НЕ начинать реализацию fake players до закрытия пунктов 1–4** (п.5 отложен отдельно). Никаких изменений кода/сборки/деплоя/Git в рамках чекпоинта не производилось; Git ведёт пользователь.

---

## 38. Accept → QueueConnect — закрытие участка `???` между `0x25 awaiting=0` и `connect` (read-only статический RE)

**Режим**: ТОЛЬКО чтение. Код/`.cpp`/`.h`/CMake/config/backend/Panorama НЕ менялись, сборка/деплой/Git не выполнялись, runtime-проверка не понадобилась (всё доказано статикой + читаемым JS из `csgo\panorama\code.pbin`). Источники: IDA-базы `client.dll`, `client_panorama.dll`, `engine.dll`, `matchmaking.dll`, `p446_csgo_gc.dll`; JS-ресурсы retail `code.pbin`; source-деревья `ida_deobfuscated_grok` (для `matchmaking/*`, `engine/cdll_engine_int.cpp`). Скрипты/дампы: scratchpad `...\re\q38_a..au.py`, `q38_*_out.txt`, `pb_accept_js*.txt`. Уровни: **CONFIRMED** (прямая декомпиляция/raw disasm/RTTI/читаемый JS), **HIGH**, **MEDIUM/HYPOTHESIS**, **UNRESOLVED**.

### 38.0 Какой клиентский DLL реально живой — важная оговорка к §5/§35–§37

- `engine.dll` жёстко грузит **`bin\client.dll`** (строка `'bin\\client.dll'` @ `0x10491790`, xref `0x101a8392`; строки `client_panorama` в engine **нет**). Значит **живой клиент = `client.dll`** (в нём Panorama-код `CUiComponent_Lobby` присутствует — `SetLocalPlayerReady`, `GetConfirmedMatchPlayerCount`, `ReadyUpForMatch` и т.д.). `client_panorama.dll` — соседний билд с **тем же кодом на других RVA** (подтверждено сопоставлением: `sub_103F7C00`≡`sub_103DF430`, `sub_103F49F0`≡`sub_103DC4B0`, `sub_105C3D90`≡`sub_10580C20`, `sub_103F7B60`≡`sub_103DF3A0`, `sub_103F7240`≡`sub_103DEA80`, `sub_103F7350`≡`sub_103DEB90`; структура decompile идентична). В §5/§35–§37 RVA даны в основном по `client_panorama.dll` — они **логически верны, но для хуков/патчей живого клиента нужны RVA `client.dll` (таблица 38.14)**. Совпадает с project446 (патчит именно `client.dll`).
- **Panorama JS доступен как текст**: `csgo\panorama\code.pbin` (заголовок `PAN\x02`, внутри zip-подобные записи `PK…panorama\scripts\…`) содержит несжатые `.js` (проверено: `popup_accept_match.js` @ ≈`2532500-2546100`, `PartyMenu`/`ServerReserved` @ `1826716`, `LoadingScreen`/`QueueConnectToServer` @ `1267646`). Это закрывает прежнее «JS не распакован» (§35.2, §37.2, §37.13 п.2-3): достаточно читать байты `code.pbin` (скрипт `re\pb.py`, `pb2.py`).

### 38.1 Короткий ответ — что находится в `???`

```
0x25 (awaiting=0) → engine sub_1008A610: state=4, callback(slot0)  [CServerConfirmedReservationCheckCallback::sub_103DF430]
  ├─ callback stage=1 (ready-up)  → mmqueue=reserved; sound; ServerReserved(map) → JS popup; RaiseReadyUp(true,0,total)
  ├─ callback stage=2 mode=0      → sub_103DF430: `if (!this+132) return;`  — НИЧЕГО (accept-режимы после полного Accept!)
  └─ callback stage=2 mode!=0     → KV QueueConnect → sub_103DEA80 (store) → per-frame poller sub_103DEB90 → session Command("QueueConnect")
                                    → matchmaking.dll → UpdateClientReservation + StartLoadingScreenForCommand("connect <adr>") → engine `connect`
```

**Главный вывод (CONFIRMED статикой):** в accept-режимах (`game_type&0xF ∈ {8,9,10,11,13}`) callback создаётся с `stage=1, mode=0` и после Accept (stage→2) **не способен запустить QueueConnect сам** — `mode` (`cb+132`) пишет только конструктор, а при `mode==0` `sub_103DF430` возвращается до формирования KV. Финальный переход обязан прийти **новым 9107**, у которого `reservation.game_type & 0xF ∉ {8,9,10,11,13}` (или `reservation` отсутствует → default instance → `game_type=0`): 9107-хендлер уничтожает старый callback и создаёт `(stage=2, mode=2)`, который сразу шлёт 0x21 stage 2 → 0x25 status 4 → QueueConnect по тому же пути, что и уже live-проверенный Casual. **Второй 9107 — единственный клиентски-допустимый триггер** (доказано data-flow'ом клиента, CONFIRMED); что именно retail-GC шлёт его в этот момент, подтверждено только косвенно — project446-бэкенд (§37.9) делает ровно так (HIGH). Ответ на «есть ли другой путь»: единственные другие создатели callback с `mode!=0` — reconnect-ветки `mode=1` (`sub_103DDCB0`/`sub_103DEC40`/хвост `sub_103DF430`), они про «переподключиться к идущему матчу» (`dword_1519C820`), не про Accept.

### 38.2 Исправления к предыдущим разделам (старые тексты не удалялись; приоритет у §38)

| # | Прежнее утверждение | Исправление | Уровень |
|---|---|---|---|
| C1 | §35.8/§36.E/§37.2-37.3: `sub_10580C20`: «`reason=="deferred"` → `sub_103DF3A0`; иначе → `sub_103DEA80(0,2)`» | **Инвертировано.** `sub_108F51B0` (client_panorama) / `sub_1094AD30` (client.dll) — **регистронезависимый `stricmp` (0 при равенстве)**; raw: `call sub_108F51B0; test eax,eax; jnz loc_10580C49` → **`reason != "deferred"` (в т.ч. `'accept'`) → `if (cb) sub_103DF3A0()` (stage→2)**; **`reason == "deferred"` → `sub_103DEA80(0,2)`**. Retail JS: кнопка Accept → `SetLocalPlayerReady('accept')`; NQMM-авто-ready → `SetLocalPlayerReady('deferred')` (38.7). Итог «Accept ⇒ 0x21 stage 2» остаётся верным, но по другой ветке. | CONFIRMED (raw disasm `0x10580c2e-0x10580c53` + JS) |
| C2 | §36.C/§37.4: `sub_103D7750` строит `ServerReserved(const char*,double)`, id `word_10D501B4`, raiser `sub_103DEA80` | **Перепутано.** `sub_103E1390` регистрирует **`ServerReserved`** → id **`word_10D501B0`**, arity 1 (`CUIEvent1<const char*>`, builder `sub_103D76D0`, raiser **`sub_103E2DF0`**). `sub_103E05A0` регистрирует **`QueueConnectToServer`** → id **`word_10D501B4`**, arity 2 (`CUIEvent2<const char*,double>`, builder **`sub_103D7750`**, raiser **`sub_103DEA80`**); double = константа `qword_10C776A8 = 2.1` (сек, совпадает с порогом `>2100` мс в poller'е). Также `sub_103E04E0` = `CancelConnectToServer` (`word_10D501B8`), `sub_103E0660` = `MatchAssistedAccept` (`word_10D501AC`). | CONFIRMED (регистрационный код + константа) |
| C3 | §5/§35.2/§37.2: `sub_10418AC0("popup_accept_match_found"/"…confirmed")` «показывает Accept-попап» | **Это звук.** `sub_10418AC0` → `sub_10415B40` строит `CUIPanelEvent2<const char*,const char*>` с id `word_10D5E09C`, зарегистрированным в `sub_107FE1D0` как **`PlaySoundEffect`** (JS использует те же имена: `$.DispatchEvent('PlaySoundEffect','popup_accept_match_confirmed','MOUSE')`). **Попап создаёт JS**: событие `ServerReserved` → `PartyMenu.ShowMatchAcceptPopUp(map)` → `UiToolkitAPI.ShowGlobalCustomLayoutPopupParameters('', popup_accept_match.xml, 'map_and_isreconnect='+map+',false')` + `ShowAcceptPopup`. | CONFIRMED (JS+регистрации) |
| C4 | §37.3: 9107 без nested `reservation` → fallback «на глобальный search-state `dword_10D9A180+32`» | `dword_10D9A180` (client_panorama) / `dword_10E0AEA0` (client.dll) — **дефолтный экземпляр protobuf** (запись в статическом инициализаторе `sub_100E8C10`); fallback = `default_instance.reservation` ⇒ **`game_type = 0`**. Следствие: **9107 без nested `reservation` = «не accept-режим» ⇒ callback `(2,2)` (direct connect)** — это и есть наш Casual (для Casual результат тот же). Для accept-режимов первый 9107 **обязан** нести `reservation.game_type` из {8,9,10,11,13}. | HIGH (default-instance паттерн; не сверено байтами инициализации) |
| C5 | §37.5: шаблоны `Update { … mmqueue … }` «только в client.dll» | Есть **в обоих** DLL (client_panorama: `0x10b6aeec/af2c/af74/bf20/c514/c540`, xrefs в `sub_103D9900`,`sub_103DC4B0`,`sub_103DDCB0`,`sub_103DF430`,`sub_103DE630`). | CONFIRMED |
| C6 | §37.12: «stage 2 & status 4 ⇒ QueueConnect / ServerReserved» (accept-режим) | **Неверно для `mode=0`** (см. 38.1, 38.4). QueueConnect случается только у callback с `mode!=0` (после второго 9107 либо reconnect). | CONFIRMED |
| C7 | §37.3/§37.13 п.6: порядок байт `awaiting/total` «HIGH, не сверен» | **CONFIRMED**: engine `sub_1008A610` читает из пакета `awaiting` (8 бит) затем `total` (8 бит) и пишет `*((_QWORD*)this+10) = awaiting | (total<<8)` (частичный ответ: `this+20 = awaiting | total<<8`); `sub_103DF430`: `v4=byte0=awaiting`, `v30=byte1=total`, `accepted = clamp(total−awaiting,0,64)`. | CONFIRMED |
| C8 | §37.13 п.4: «кто/когда вызывает heartbeat `sub_103F6240`» | **Найдено**: per-frame `CUiComponent_Lobby::Update` → `sub_103DE9B0(this+2)`: если сессия есть, `game/mmqueue` непуст, `system/lock` пуст и mmqueue ∈ {`heartbeating`,`registering`} и `Plat_MSTime()-this[1] > 0xAFC8 (45000 мс)` → вызывает `vfn2` (`sub_103DDCB0`) → повторный `MatchmakingStart` (`heartbeating`). Для `searching` — по таймеру `sub_103DD8B0()` при `dword_152786A4`. В состояниях `reserved`/`connect` действует `lock mmqueue` ⇒ heartbeat НЕ шлётся. | CONFIRMED (кроме `sub_103DD8B0`: MEDIUM) |
| C9 | §35.3/§37.4: `matchmaking` enum «не восстановлен» | Из 9104-хендлера: `0`→очистка; `4`→mmqueue=**connect** (raw `cmp [ecx+9Ch],4; cmovnz`); любое **другое ≠0** →**searching**; `3` дополнительно `RaiseReadyUp(false,0,0)`. Полные имена enum не нужны для connect. | CONFIRMED |
| C10 | §37.13 п.3/п.7 (что рисует попап; копирует ли `sub_103DFBE0` `account_ids`) | `sub_103DFBE0` копирует **только** `reservationid`(поле 4, msg+24), `serverid`(msg+8), адрес (`direct_udp_ip/port`, `server_address`), `map`, флаг+`game_type&0xF`. `account_ids` **не копируются**. Список аватаров попапа — `LobbyAPI.GetConfirmedMatchPlayerCount/ByIdx` (client.dll `sub_105C70F0`/`sub_105C7220` → `sub_105C4080/40A0`) читает `RepeatedField` из глобального указателя `dword_10DBD140` (единственная запись в статике — обнуление в инициализаторе `0x10045df0`; в `client_panorama.dll` этих API **нет вообще**). Отрисовка списка идёт только при `numPlayers > 2`. Источник этого указателя в 9107-пути **не найден**. | CONFIRMED (что 9107-копировщик их не берёт) / UNRESOLVED (источник `dword_10DBD140`) |

Дополнительное предупреждение (**`stricmp`-семантика**): все сравнения вида `if (sub_108F51B0(x,"literal"))` в client_panorama означают «**НЕ равно**». Это затрагивает трактовки `"connect"` в §35.3/§37.3 (9104 `matchmaking==0`-ветка: `v14 = (mmqueue != "connect")`; `sub_103DF430` fail-ветка: `mmqueue != "connect"` ⇒ CloseSession+ошибка `#SFUI_QMM_ERROR_NoOngoingMatch`). На connect-цепочку не влияет.

### 38.3 Engine: как приходит `0x25` и как вызывается callback (engine.dll, live-проверено ранее §22-27)

- Диспетчер connectionless `sub_1008E7C0`: `case 37` (0x25) → **`sub_1008A610`** (`CServerMsg_CheckReservation::OnResponse`). Проверки: `protocol const` (`dword_138EF1C4`=13805), `state==0` (pending), `reservationid` (`this+44`), `token` (`this+88`, `arg`), затем читает **`awaiting` (u8), `total` (u8)**:
  - `awaiting != 0`: `DevMsg("Server reservation%u is awaiting %d/%d")`; если `awaiting==127` → `state=3` (failed), callback; иначе `this+80 = awaiting | total<<8`, **state остаётся 0**, callback;
  - `awaiting == 0`: `DevMsg("Server confirmed all players reservation%u/%d")`; **если `state==0` → `state=4`**, `this+80 = total<<8`, callback. (Повторный 0x25 при `state!=0` игнорируется.)
- Callback = `(**(cb_ptr@this+12))(cb, this)` — vtable slot 0 = **`CServerConfirmedReservationCheckCallback::vftable[0]`**: RTTI `??_7CServerConfirmedReservationCheckCallback@@6B@` @ `0x10b6a408` (client_panorama) → slot 0 = **`sub_103DF430`** (CONFIRMED по RTTI-имени); client.dll — vtable data xref `0x10bccbe8` → `sub_103F7C00`.
- **Что значит `vfunc+4(a2)==4` в `sub_103DF430`**: `(*(*a2+4))(a2)` = vtable slot 1 `sub_100635B0` = `state` (`this+8`); `==4` ⇔ engine напечатал `"Server confirmed all players reservation%u/%d"` (ответ 0x25 с `awaiting==0` при `state==0`); `vfunc+4 == 0` ⇔ pending/partial. `vfunc+8(a2)` = slot 2 `sub_100A44C0` = qword `this+80`.
- Состояния `CServerMsg_CheckReservation` (`this+8`): `0` pending; `2` отменён пользователем (slot 4 `sub_100A44B0` пишет 2; текст `"User canceled matchmaking"`); `3` неудача/таймаут (`"Matchmaking failed; Waiting on %d/%d clients"` / `"…We never heard from gameserver"`, `sub_1008A3E0`); `4` все подтвердили. slot0 `sub_100A5EE0` = `state>1` (done), slot1 `sub_100635B0` = `state`, slot2 `sub_100A44C0` = qword `this+80` (`byte0=awaiting`, `byte1=total`).
- Создание запроса: `INETSUPPORT_003` vtable slot 15 (`+60`, `CNetSupportImpl` `sub_1024BC90`) → `sub_10098300(adr, reservationid_lo, reservationid_hi, stage, callback, &handle)` → `sub_1008A350` (объект 112 байт, CServerMsg_CheckReservation) регистрируется в списке pending; handle пишется в `cb+128`. Отправка `0x21` (33 байта, §23) повторяется движком (~1 c) до `state != 0`.
- Отмена/уничтожение: `handle->vfunc+20` (slot 5, `sub_1008A3E0`).

### 38.4 `CServerConfirmedReservationCheckCallback` и полная таблица решений `sub_103DF430` (client_panorama `0x103DF430`, 1966 байт; client.dll `0x103F7C00`)

Поля callback (144 байта; ctor `sub_103DF1C0` / client.dll `sub_103F7980`): `+0` vftable; `+8` встроенная struct (88 байт: копия 9107-полей через `sub_103DF2C0`); **`+40/+44` = `reservationid`** (msg+24, поле 4; идёт в 0x21, в KV `reservationid`, в `UpdateClientReservation`); `+48/+52` = `serverid` (msg+8; потребитель не найден, MEDIUM); `+68` = `map` (std::string); **`+92` = `reservation.game_type & 0xF`** (если у msg есть `reservation`, иначе 0); **`+96..+127` = 32-байтный address (`sub_103DFED0`, строится из статического адреса, который `sub_103DFBE0` заполняет из `direct_udp_ip/port`/`server_address` — HIGH)**, он же цель 0x21 и `adronline`; **`+128` = handle engine-запроса**; **`+132` = mode** (`a4`: 0 accept-ready-up, 1 reconnect, 2 direct); `+136` = байт «был непустой ответ» (`=1` при первом status≠pending); **`+140` = stage** (`a3`: 1 ready-up, 2 accepted/direct). Единственные писатели: ctor (`+132`,`+140`) и `sub_103DF3A0` (`+140`←2) — **проверено сканом всех 12 функций, трогающих `dword_15278690`**.

Решающая таблица `sub_103DF430(this, a2)` (условие входа `a2 == this+128`; `S=a2->state`, `T=stage`, `M=mode`, `G`= сессия есть ∧ `game/mmqueue` непуст):

| Условие | Действие |
|---|---|
| `S==0` (pending/partial) | `if T==2`: `RaiseReadyUp(true, clamp(total−awaiting,0,64), total)`; return (T==1: ничего) |
| `S!=0`, `G`, `S==4`, **`T==1`** | KV `Update{system{lock mmqueue} game{mmqueue reserved}}` (`0x103df537`) → сессия `vtbl+8`; звук `popup_accept_match_found`; если panorama-флаг (`byte_1519DDEE`): **`ServerReserved(map)`** (`sub_103E2DF0`, `map = cb+68` по raw disasm `0x103df56a-0x103df573`); `if !panorama: RaiseReadyUp(false,0,total)`; **`RaiseReadyUp(true,0,total)`**; return (**callback остаётся жить** для stage 2) |
| `S!=0`, `G`, `S==4`, `T!=1`, **`M==0`** | **`return` — ничего не происходит** (accept-режимы после полного Accept; callback остаётся жить до следующего 9107/сброса) |
| `S!=0`, `G`, `S==4`, `T!=1`, `M!=0` | **QueueConnect-ветка** (ниже) |
| остальное (`S∈{3,2}` или `!G`) | «will not queue connect»: `M==1`/`M==2` → при условиях `CloseSession` + `sub_103D98D0("#SFUI_LobbyPrompt_MMFailedTitle","#SFUI_QMM_ERROR_NoOngoingMatch")`; затем `if (M!=0 \|\| !(dword_1519C894&2))` → cleanup (`RaiseReadyUp(false,0,total)` если `M!=0`&panorama&`v25!=1`; destroy cb; `dword_15278690=0`), иначе (M==0 и есть reconnect-данные) — `"failed to receive confirmation, but will reconnect to ongoing match!"` → новый cb `(2,1)` из `dword_1519C820` |

**QueueConnect-ветка** (`T==2`, `M!=0`, `S==4`): KV `Update{system{lock mmqueue} game{mmqueue connect}}` (`0x103df5dc`); `if sub_103D7640(game_type)` (accept-required): звук `popup_accept_match_confirmed`, `v29=0`; **иначе** `v29=1`, звук `popup_accept_match_found`, `ServerReserved("@"+map)` (**NQMM-«announcement» попап**, 38.7). Затем строится KV **`QueueConnect`**: `adronline`(cb+96 как строка), `reservationid`(cb+40/44), `helper_pSession`(текущая сессия, ptr), `helper_time = Plat_MSTime() + (v29==1 ? 2000 : 0)`, `auto_close_session` (=1; для `M!=2` и `members/numMachines>1` → 0), `map`, `gametype`/`gamemode` (таблица §37.1 по `game_type`), затем **`sub_103DEA80(KV, v29)`** и cleanup (закрыть ready-up если `M!=0`&panorama&`v29!=1`; destroy cb; `dword_15278690=0`).

`sub_103DEA80(kv, a2)` (client.dll `sub_103F7240`, `0x103DEA80`, 267 байт): **`a2==1`** → заменить сохранённый KV (`dword_15278694`=kv), событий нет; **`a2==2`** → если сохранённого KV нет → `return 0`, иначе только поднять событие; **`a2==0`/прочее** → заменить KV **и** поднять событие. Событие (при panorama): `map = KV["map"]`, `sub_10496C10(loadingScreenMgr, map, skirmishmode)` (подготовка loading-screen), затем **`QueueConnectToServer(map, 2.1)`** (`word_10D501B4`). Вызывающие: `sub_103DF430` (a2=v29), `sub_10580C20` (JS `deferred`, a2=2), `sub_10580430` (`StartListenServer`, a2=0).

### 38.5 9107 (`MatchmakingGC2ClientReserve`) — полный lifecycle клиентского обработчика (`sub_103DC4B0` / client.dll `sub_103F49F0`)

Обработчик **не имеет дедупликации и рассчитан на N вызовов**. На КАЖДОЕ 9107: (1) gate: сессия есть и `game/mmqueue` непуст, иначе клиент шлёт `MatchmakingStop`; (2) при `has(map)` — сессионный KV `Update/game/map = msg.map`; (3) **`Update{system{lock mmqueue} game{mmqueue reserved}}`** (`0x103dc534`) — `mmqueue=reserved` **на каждом** 9107; (4) `sub_103DFBE0` копирует поля msg→struct; `DevMsg("Matchmaking reservation confirmed: %llx/%s")` (`%llx`=`reservationid`, `%s`=адрес); (5) **старый callback уничтожается** (`sub_103DF330`: отмена его engine-запроса + free); (6) `v10 = (msg.reservation ?: default).game_type & 0xF`: **`{8,9,10,11,13}` → `cb=(stage 1, mode 0)`**, иначе **`cb=(stage 2, mode 2)`**; ctor сразу регистрирует запрос 0x21 с этим `stage`.

Что реально используется из сообщения (data-flow): `reservationid` (поле 4) — 0x21 + KV + cookie для connect; адрес (`direct_udp_ip`, `direct_udp_port`, `server_address`) — цель 0x21 и `adronline`; `map` — сессионный KV и попап; **`reservation.game_type&0xF` — единственный селектор ветки**; `serverid` копируется, потребителя не найдено. **Не используются**: `account_ids`, `party_ids`, `rankings`, остальные поля `reservation`. Следствие для второго 9107: он должен нести **валидные `reservationid` и адрес** (они перечитываются из НОВОГО сообщения) и **не-accept `game_type`/отсутствие `reservation`**.

Кросс-проверка (HIGH): project446 `sub_1006EAD0` читает `game_type` из разобранного 9107 (`*(a2+68)`, вероятно `reservation.game_type`): ненулевой accept-тип → «Match found!» → ready-up-состояние 2; **`game_type==0` при ready-up** → `"[GC] Connect reserve received (all players accepted)"` → состояние 3 → connect; `game_type==0` без ready-up → `"Live join: server sent game_type=0"`. Т.е. их backend шлёт «connect-reserve» именно как 9107 с `game_type=0`.

### 38.6 9104 (`MatchmakingGC2ClientUpdate`, `sub_103D9900` / client.dll `sub_103F1C00`) и переходы `game/mmqueue`

Обработка целиком **пропускается**, если активный callback имеет `mode==2` (`*(dword_15278690+132)==2`). При `matchmaking!=0` и непустом mmqueue: `matchmaking==4` → **`connect`**, иначе → **`searching`** (`0x103d9d28/9d2d`, `cmp [ecx+9Ch],4; cmovnz`); `matchmaking==3` дополнительно `RaiseReadyUp(false,0,0)` (закрыть попап). **9104 не запускает connect**, только пишет маркер. При `matchmaking==0` — очистка (`Delete{…mmqueue #empty#}`, `cfg/qmmconnect.dt`).

Все писатели `game/mmqueue` (CONFIRMED по строкам/xrefs):

| Значение | Писатель (client_panorama / client.dll) | Условие |
|---|---|---|
| `registering` | `sub_103DDCB0` / `sub_103F6240` (Play, `vfn2`) | mmqueue пуст |
| `heartbeating` | тот же `vfn2` | mmqueue непуст; период 45 с (`sub_103DE9B0`) |
| `searching` | 9104 (`sub_103D9900`/`sub_103F1C00`) | `matchmaking ∉ {0,4}` |
| `reserved` | 9107-хендлер; `sub_103DF430` (ready-up ветка, stage 1, status 4); reconnect `sub_103DDCB0` | — |
| `connect` | 9104 при `matchmaking==4`; `sub_103DF430` QueueConnect-ветка (перед KV `QueueConnect`) | — |
| очистка | 9104 `matchmaking==0`; `sub_103DE630` (`MatchmakingStop`/cancel, **не выполняется** если `dword_15278694!=0` или `cb.mode==2`) | — |

### 38.7 Accept-попап и native-мост (retail JS, `popup_accept_match.js` из `code.pbin`)

- **Показ**: `PartyMenu` слушает `ServerReserved` → `ShowMatchAcceptPopUp(map)` → `popup_accept_match.xml` с `map_and_isreconnect=<map>,false`. `map` начинается с `@` ⇒ **NQMM-«announcement only»** (`m_isNqmmAnnouncementOnly=true; m_hasPressedAccept=true`): без кнопки Accept/таймера, `$.Schedule(1.9, _OnNqmmAutoReadyUp)`.
- **Кнопка Accept** (`_OnAcceptMatchPressed`): `LobbyAPI.SetLocalPlayerReady('accept')` → `sub_10584F70`→`sub_10585B00`→`sub_10580C20('accept')` → **`sub_103DF3A0`** (`stage>=2 → return 0`; иначе `cb+140=2`, отмена старого запроса, **новый 0x21 stage 2** через `INETSUPPORT_003 vtbl+60`). Событие `MatchAssistedAccept` (`word_10D501AC`) вызывает тот же `OnAcceptMatchPressed`.
- **NQMM auto** (`_OnNqmmAutoReadyUp`): звук `popup_accept_match_confirmed`, `SetLocalPlayerReady('deferred')` → `sub_10580C20('deferred')` → **`sub_103DEA80(0,2)`** → (если есть сохранённый KV) `QueueConnectToServer(map, 2.1)`; затем `CloseAcceptPopup`.
- **Счётчик**: `PanoramaComponent_Lobby_ReadyUpForMatch(shouldShow, playersReadyCount, numTotalClientsInReservation)` → `_ReadyForMatch`: `!shouldShow` → закрыть попап; иначе слоты `AcceptMatchSlot0..total-1`, `accepted = playersReadyCount` (`"#match_ready_players_accepted"`); спец-случай `(1,1)` при ранее известном `total>1` — использует прежний total. Таймер: `LobbyAPI.GetReadyTimeRemainingSeconds()` (от `this+16 = Plat_MSTime()`, ставится `RaiseReadyUp(true,…)`).
- **Потребитель `QueueConnectToServer`** — только `LoadingScreen.Init` (UI загрузочного экрана), **сам connect не выполняет**.

### 38.8 Отложенное исполнение QueueConnect — per-frame poller

`CUiComponent_Lobby` (vftable `0x10bbdf44` slot 0 = `sub_1057F760`; client.dll vtable data `0x10c25770` slot 0 = `sub_105C2330`) — **per-frame Update**. В конце: `sub_103DE9B0` (heartbeat-watchdog) и **`sub_103DEB90`** (client.dll `sub_103F7350`): при `dword_15278694 != 0` **и** `Plat_MSTime() − KV["helper_time"] > 2100` **и** `!IVEngineClient slot 28` (флаг «loading plaque active», `byte_10625F16` — тот же байт проверяет slot 29 `HideLoadingPlaque`) **и** `текущая сессия == KV["helper_pSession"]` → **`session->vtbl+12 (Command)(QueueConnect KV)`**; в любом случае KV затем освобождается (`dword_15278694=0`). Предусловия самого Update, пока mmqueue занят (`sub_103DE8E0`: mmqueue непуст ∨ `system/lock` непуст): `byte_10CFB1F8` (client-secure флаг; начальное значение **1**; сбрасывается событием `OnClientInsecureBlocked`) должен быть ≠0, иначе `sub_103D9190` (ошибка `X_InsecureBlocked`, CloseSession); и `dword_15280DBC vtbl+512 != 2` (pure-file state). Временная сводка «не-accept» пути: t0 = `0x25` status 4 → KV + `helper_time=t0+2000` + NQMM-попап; t0+1.9 c `deferred` → `QueueConnectToServer` (loading screen); **t0+≈4.1 c poller → Command(QueueConnect)** → connect.

### 38.9 Session `Command("QueueConnect")` → matchmaking.dll (retail-бинарь + source)

`CMatchSessionOfflineCustom::OnRunCommand` = matchmaking.dll **`sub_1001B290`** (vtable data `0x1006adf8`; source `mm_session_offline_custom.cpp:155`): для `QueueConnect` при `auto_close_session!=0`: `m_eState=2 (RUNNING)`; **`sub_1002CE60` = `MatchSession_PrepareClientForConnect(reservationid)`** (форматирует `"$%llx"` и передаёт в `dword_10085114 vtbl+56`; затем `(dword_100A336C vtbl+48)()->vtbl+40`; при `reservationid==0` берёт `xuidReserve` из настроек); **`CloseSession`** (`dword_100A336C vtbl+80`); **`INetSupport::UpdateClientReservation(reservationid, 0)`** (`dword_100842C8 vtbl+52`; engine `CNetSupportImpl` slot 13 `sub_1024BBF0` → пишет cookie в `cl+256/260`); **`IVEngineClient::StartLoadingScreenForCommand("connect %s", adronline)`** (`dword_100842D0 vtbl+848`, slot 212). Аналоги для сессий OnlineClient/OnlineHost: `sub_1001D990`, `sub_10023990` (`state=3/7`, тот же вызов). Если `auto_close_session==0` (мульти-машинная party) — команда уходит title-handler'у.

### 38.10 Engine connect chain (насколько нужно, без полного RE)

`CEngineClient` vtable `0x104657e8` slot 212 `sub_100B3FB0` → `EngineVGui (dword_138D0320) vtbl+136` = `CEngineVGui::StartLoadingScreenForCommand` (`sub_1029BCE0`, slot 34) → `staticGameUIFuncs (dword_13909C04) vtbl+48` = **`CGameUI::StartLoadingScreenForCommand`** (IGameUI slot 12; client_panorama `sub_10417E50`, vtable `0x10b79018`: `if (this[510]) IVEngineClient(vtbl+456 = slot 114 ClientCmd_Unrestricted)(cmd,0)`) → engine `sub_100B1C80` → **`Cbuf_AddText`** (`sub_101CE460`) → команда исполняется на следующем кадре → консольная команда **`connect`** (ConCommand `0x105888f8`, help «Connect to specified server.», callback **`sub_100D5220`**) → `CBaseClientState::Connect` (vtbl+180 `sub_1008CE50` → **`sub_1008CC80`**: собирает `public`/`private`/`direct` адреса, печатает `Connecting to public(ip:port)`) → сетевой connect с cookie из `UpdateClientReservation`; сервер проверяет cookie (`sub_101B8F60`: `"Rejecting connection request … client's reservation cookie %llx does not match servers's cookie"`) → `Connected to %s` (`sub_100EB620`) → загрузка уровня. (Строка `"Connecting to %s\n"` @ `0x10474060` в `sub_100DF2D0` — это Steam `GameServerChangeRequested_t`, **не** наш путь.)

### 38.11 Итоговая цепочка (детально)

**A. Не-accept режим (Casual `game_type=7`, live-проверено)**: клик Play (`sub_103DDCB0`: `registering`, 9101) → `ClientGC` шлёт 9107 без `reservation` → 9107-хендлер: `mmqueue=reserved`, `cb=(2,2)`, 0x21 stage 2 → `srcds` (`G`-cookie ⇒ `awaiting=0`) → 0x25 → `sub_1008A610` state 4 → `sub_103DF430`: QueueConnect-ветка (`v29=1`): `mmqueue=connect`, звук, `ServerReserved("@map")`, KV `QueueConnect`, `sub_103DEA80(KV,1)` (store) → NQMM-попап 1.9 c → `deferred` → `QueueConnectToServer(map,2.1)` → poller (≥ `helper_time+2100`) → `Command(QueueConnect)` → matchmaking.dll → `UpdateClientReservation` + `StartLoadingScreenForCommand("connect adr")` → `Cbuf_AddText` → `connect` → `CBaseClientState::Connect`.

**B. Accept-режим (8/9/10/11/13)**:
1. 9107 #1 с `reservation.game_type∈accept` → `mmqueue=reserved`, `cb=(1,0)`, 0x21 stage 1 (реальный + fake-0x21 stage 1).
2. Сервер: все stage≥1 → `awaiting=0` → 0x25 (всем) → status 4, `T==1` → `mmqueue=reserved`, звук, `ServerReserved(map)` → JS открывает попап, `RaiseReadyUp(true,0,total)`.
3. Игрок жмёт Accept → `SetLocalPlayerReady('accept')` → `sub_103DF3A0`: stage=2, **новый 0x21 stage 2**; 0x25 partial → `RaiseReadyUp(true, total−awaiting, total)` (счётчик).
4. Все stage 2 → сервер `awaiting=0` → 0x25 → status 4, `T==2`, **`M==0` → return (пусто)**.
5. **9107 #2** (не-accept `game_type`/без `reservation`; тот же `reservationid`+адрес) → 9107-хендлер: `mmqueue=reserved`, **destroy старый cb**, `cb=(2,2)`, новый 0x21 stage 2 → 0x25 status 4 → QueueConnect-ветка → далее как в A (п. с `v29=1`).

### 38.12 Роли ключевых функций (по списку задания)

- **`0x25 awaiting=0`**: engine `sub_1008A610` (`state=4`) → `CServerConfirmedReservationCheckCallback[0]` = `sub_103DF430` (RVA см. 38.14).
- **9107**: единственный источник callback `(1,0)`/`(2,2)`; может приходить многократно; второй 9107 с не-accept `game_type` = триггер connect в accept-режимах; уничтожает предыдущий callback; пишет `mmqueue=reserved`.
- **9104**: только маркеры (`searching`/`connect`/очистка), `RaiseReadyUp(false,0,0)` при `matchmaking==3`; connect не запускает; игнорируется при `cb.mode==2`.
- **`sub_103F7C00` (client.dll)** ≡ `sub_103DF430`: единственный формирователь KV `QueueConnect` (единственный xref строки `"QueueConnect"`) и ready-up-ветки; **это и есть «reservation-check callback»**.
- **`sub_103DF3A0`** ≡ client.dll `sub_103F7B60`: stage 1→2 и перерегистрация запроса (Accept); единственный вызов — `sub_10580C20` при `reason != "deferred"`.
- **`sub_103D7750`**: builder `QueueConnectToServer(const char*, double 2.1)` (НЕ `ServerReserved`, C2); `ServerReserved` = `sub_103D76D0`/`sub_103E2DF0`.
- **`sub_103DEA80`**: хранилище отложенного QueueConnect-KV + raiser `QueueConnectToServer`; **сам connect не выполняет**.
- **`sub_103DEB90`** (per-frame): реальный «спусковой крючок» — исполняет `Command(QueueConnect)` в текущей сессии.
- **`sub_103D8AF0`**: singleton-getter lobby-объекта (только адаптер к `RaiseReadyUp`).
- **`sub_103F6240`/`sub_103DDCB0`**: Play/`MatchmakingStart` (registering/heartbeating), reconnect-ветка `(2,1)`.

### 38.13 Что это значит для нашего `ClientGC` (НЕ реализуется в §38; только вывод)

- Наш нынешний 9107 (без `reservation`) ⇒ `cb=(2,2)` ⇒ **всегда direct-connect** (для Casual правильно). Для 8/9/10/11/13 это **обойдёт Accept**: нужен 9107 #1 с `reservation.game_type` ∈ accept-set.
- Retail-клиент **не сообщает GC о своём Accept** (project446: `"legacy client omits 9102 on Accept"`, инжектор `sub_100787F0`); «все приняли» знает игровой сервер (`m_arrReservationPlayers`, `ReportGCQueuedMatchStart` — в retail `server.dll` заглушка `return 0`, §37.8) и, по retail-архитектуре, сообщает GC, который шлёт 9107 #2. У нас `ServerGC` и `ClientGC` — разные процессы/машины и **канала между ними нет**: источник сигнала для 9107 #2 — открытая проблема (NEXT STEP).
- `UpdateClientReservation(reservationid)` берёт cookie из 9107 #2 — он должен совпасть с cookie сервера (общий `GameServerCookieId`, как сейчас).

### 38.14 Таблица RVA (живой `client.dll` ⇄ `client_panorama.dll`)

| Роль | `client.dll` (живой) | `client_panorama.dll` |
|---|---|---|
| reservation-check callback (`vftable[0]`) | `sub_103F7C00` (`0x103F7C00`, 1966 б) | `sub_103DF430` |
| callback ctor | `sub_103F7980` | `sub_103DF1C0` |
| stage→2 (Accept) | `sub_103F7B60` | `sub_103DF3A0` |
| 9107-хендлер | `sub_103F49F0` | `sub_103DC4B0` |
| 9104-хендлер | `sub_103F1C00` | `sub_103D9900` |
| Play/`vfn2` | `sub_103F6240` | `sub_103DDCB0` |
| cancel/`vfn3` | `sub_103F6DD0` | `sub_103DE630` |
| `SetLocalPlayerReady` classifier | `sub_105C3D90` (вызов `0x105C932C`) | `sub_10580C20` |
| store KV + raise `QueueConnectToServer` | `sub_103F7240` | `sub_103DEA80` |
| per-frame poller QueueConnect | `sub_103F7350` (из `sub_105C2330`) | `sub_103DEB90` (из `sub_1057F760`) |
| `ServerReserved` raiser | `sub_103FB5C0` | `sub_103E2DF0` |
| `ServerReserved`/`QueueConnectToServer`/`Cancel…`/`MatchAssistedAccept` регистрация | `0x103F9B60`/`0x103F8D70`/`0x103F8CB0`/`0x103F8E30` | `0x103E1390`/`0x103E05A0`/`0x103E04E0`/`0x103E0660` |
| `ReadyUpForMatch` регистрация / raiser | `0x105C4900` / (`RaiseReadyUp`, slot 0 lobby-подобъекта) | `sub_10581640` / `sub_1057FF90` |
| хранимый KV / активный callback (глобалы) | `dword_152EAD18` / `dword_152EAD0C` | `dword_15278694` / `dword_15278690` |
| accept-required(gametype) | `sub_103EF770` | `sub_103D7640` |
| звук `PlaySoundEffect` raiser | `sub_10432490` | `sub_10418AC0` |
| `CGameUI::StartLoadingScreenForCommand` | (тот же класс в `client.dll`; `GameUI011` @ `0x10253A50`) | `sub_10417E50` (vtbl `0x10b79018`) |

Engine (`engine.dll`): `0x25`-обработчик `sub_1008A610`; диспетчер `sub_1008E7C0`; создание запроса `sub_10098300`/`sub_1008A350`; `connect` ConCommand `sub_100D5220`; `CBaseClientState::Connect` `sub_1008CE50→sub_1008CC80`; `Cbuf_AddText` `sub_101CE460`; `CEngineClient` vtbl `0x104657e8` (slot 114 `ClientCmd_Unrestricted`, slot 212 `StartLoadingScreenForCommand`); `CEngineVGui` vtbl `0x104b94e4` (slot 34 `sub_1029BCE0`). matchmaking.dll: `sub_1001B290` (offline custom OnRunCommand), `sub_1001D990`/`sub_10023990` (online client/host QueueConnect), `sub_1002CE60` (PrepareClientForConnect).

### 38.15 Что НЕ доказано (сжато)

Точный мгновенный «формат» retail-9107 #2 (наблюдался только через project446: `game_type=0`); кто в retail-инфраструктуре шлёт его и на каком триггере (GC ← `ReportGCQueuedMatchStart`, HIGH по архитектуре); значение `serverid` (потребителя нет); источник глобального `dword_10DBD140` для списка аватаров попапа (`GetConfirmedMatchPlayer*`); клиентская сторона `PartyMenu` для `map` без `@` при `mode==0` подтверждена по JS+native, но визуально/live не проверялась; `sub_103DD8B0` (таймер `searching`-heartbeat); какой `IUiComponent` менеджер вызывает `Update` каждый кадр (по vtable slot 0 — очевидно, но вызывающий не трассировался).

### §38 STATUS

CONFIRMED:
- `0x25` обрабатывает engine `sub_1008A610`: `awaiting`(u8), `total`(u8); `awaiting==0` ∧ `state==0` ⇒ `state=4`; callback = `CServerConfirmedReservationCheckCallback::vftable[0]` = `sub_103DF430` (client.dll `sub_103F7C00`).
- `cb+132` (mode) и `cb+140` (stage) пишутся только ctor'ом (mode) и ctor/`sub_103DF3A0` (stage). Accept-режим ⇒ `(stage 1, mode 0)`; в `sub_103DF430` при `stage==2 ∧ mode==0` и status 4 — **немедленный `return`** без QueueConnect.
- Единственный формирователь KV `QueueConnect` — `sub_103DF430`/`sub_103F7C00` (единственный xref строки); он требует `mode!=0`, `stage!=1`, `status==4`; сохраняет KV через `sub_103DEA80`; исполняет **per-frame poller `sub_103DEB90`** (`helper_time+2100`, не loading plaque, та же сессия) вызовом `session->Command`.
- 9107-хендлер: многократный вызов, `mmqueue=reserved` на каждом, уничтожение прежнего callback, выбор `(1,0)`/`(2,2)` по `reservation.game_type&0xF ∈ {8,9,10,11,13}`; из msg используются `reservationid`, адрес, `map`, `game_type`; `account_ids`/`party_ids`/`rankings` не используются.
- Отсутствует другой путь создания `mode!=0` callback в accept-потоке, кроме нового 9107 (и reconnect `mode=1`).
- 9104: `matchmaking==4`→`mmqueue=connect`, ≠0,≠4→`searching`, `3`→`RaiseReadyUp(false,0,0)`; connect не запускает; игнорируется при `cb.mode==2`.
- Accept-кнопка JS ⇒ `SetLocalPlayerReady('accept')` ⇒ `sub_10580C20` (≠`deferred`) ⇒ `sub_103DF3A0` (stage 2 + новый 0x21); `'deferred'` (NQMM авто) ⇒ `sub_103DEA80(0,2)`. `sub_108F51B0`/`sub_1094AD30` = `stricmp` (0 при равенстве).
- События: `ServerReserved`=`word_10D501B0` (arity 1, raiser `sub_103E2DF0`, JS `PartyMenu.ShowMatchAcceptPopUp`); `QueueConnectToServer`=`word_10D501B4` (arity 2, builder `sub_103D7750`, double=2.1, JS `LoadingScreen.Init`); `PlaySoundEffect`=`word_10D5E09C` (`sub_10418AC0`); `MatchAssistedAccept`=`word_10D501AC`; `ReadyUpForMatch` JS `PopupAcceptMatch.ReadyForMatch`.
- Цепочка после QueueConnect: matchmaking.dll `sub_1001B290` → `PrepareClientForConnect`+`CloseSession`+`UpdateClientReservation`+`StartLoadingScreenForCommand("connect %s")` → engine `Cbuf_AddText` → `connect` ConCommand `sub_100D5220` → `CBaseClientState::Connect` `sub_1008CE50/1008CC80`.
- Живой клиент = `client.dll` (engine хардкодит `bin\client.dll`).
- Heartbeat 9101: 45 с (`0xAFC8`), только при `mmqueue ∈ {registering,heartbeating}` без `system/lock`.

HIGH CONFIDENCE:
- Второй 9107 с не-accept `game_type` — реальный retail-триггер connect после полного Accept (статически необходим; cross-check: project446 `game_type==0` «Connect reserve»).
- Дефолтный экземпляр protobuf (`dword_10D9A180`/`dword_10E0AEA0`) ⇒ отсутствие `reservation` ≡ `game_type 0`.
- Адрес `cb+96` строится из `direct_udp_ip/port`/`server_address` через статический staging-объект (`sub_103DFBE0`→`sub_103DFED0`).
- `sub_10418AC0` = звук (`PlaySoundEffect`).
- Retail GC получает «все приняли» от игрового сервера (`ReportGCQueuedMatchStart`), клиент Accept на GC не шлёт.

HYPOTHESIS:
- Retail-9107 #2 содержит те же `reservationid`/адрес, что и #1 (нужно клиенту по data-flow, но байты retail-GC не наблюдались).
- Reconnect-ветка `(2,1)` (`dword_1519C820`) теоретически пригодна как альтернативный триггер без 9107 #2 (для теста), не исследована на пригодность.

UNRESOLVED:
- Как у нас (два процесса) получить сигнал «все приняли» для отправки 9107 #2; способ канала ServerGC↔ClientGC.
- Как сервер идентифицирует real player в `0x21` при нашей архитектуре (account реального игрока должен быть в Q-roster — источник account на стороне `srcds`).
- Реализация fake-player stage=2 (порядок/тайминг 0x21 fake после реального Accept).
- Danger Zone: строение Q-roster и team-split.
- Источник `dword_10DBD140` (`GetConfirmedMatchPlayer*`), `serverid` consumer, `sub_103DD8B0`.
- Cookie lifecycle / повторная reservation (§34) — отложено.

### NEXT STEP AFTER §38

1. **Не начинать fake players/реализацию** до решения по сигналу «все приняли»: клиентский протокол теперь известен полностью (две фазы 9107), неизвестно только, **откуда `ClientGC` узнаёт момент отправки 9107 #2** при отсутствии канала с `srcds`. Нужно явное решение пользователя между вариантами: (a) test-only канал `ServerGC`→`ClientGC` (например, простое сообщение по сети; `ServerGC` узнаёт «все stage≥2» через vtable-хук `ReportGCQueuedMatchStart` slot 47 как у project446 `sub_100BADE0`, либо через собственный учёт 0x21); (b) test-only локальная эвристика на клиенте (таймер после Accept) — не retail-эквивалент; (c) reconnect-ветка `(2,1)` как альтернативный test-триггер (требует отдельного RE `dword_1519C820`/`GC_Hello`).
2. Затем (по отдельной команде) — дизайн harness: `ServerGC`: Q-roster `[real][fake…]` (account реального игрока нужно передать серверу), fake-0x21 stage 1 (для попапа) и stage 2 (после реального Accept); `ClientGC`: 9107 #1 `reservation{game_type∈accept, account_ids…}` и 9107 #2 (без `reservation`, тот же `reservationid`+адрес).
3. Малые уточняющие RE-проверки перед кодом (по желанию): `ReportGCQueuedMatchStart` аргументы (`minStage`, `confirmedAccounts[]`) для сигнала; поведение попапа при `total` из Q-roster (проверка `RaiseReadyUp(true,…)`) — можно live-логами существующего прототипа без правок кода.
4. Отложено: cookie lifecycle/повторная reservation (§34), Danger Zone Q-roster.

---

## 39. Source of Post-Accept GC Trigger — что запускает connect после полного Accept (read-only статический RE)

**Режим**: только чтение. Код/`.cpp`/`.h`/CMake/config/backend/Panorama не менялись, сборка/деплой/Git не выполнялись, runtime не понадобился. Источники: IDA-базы `client.dll` (живой клиент, §38.0), `client_panorama.dll`, `engine.dll`, `server.dll`, `p446_csgo_gc.dll`; `protobufs/cstrike15_gcmessages.proto`. Скрипты/дампы: scratchpad `...\re\q39_a..ah.py`, `q39_*_out.txt`. RVA даны для `client.dll` (живой), эквиваленты `client_panorama.dll` — в таблице §38.14. Уровни: **CONFIRMED**, **HIGH**, **HYPOTHESIS**, **UNRESOLVED**.

### 39.1 Короткий ответ

**Да: единственный внутрисессионный GC-триггер connect после полного Accept, который допускает retail-клиент, — это `MatchmakingGC2ClientReserve` (9107) с `reservation.game_type & 0xF ∉ {8,9,10,11,13}` (или без `reservation` ⇒ default ⇒ `game_type=0`)** — «второй/следующий 9107». Это доказано **замыканием**: у retail `client.dll` ровно 5 GC→client matchmaking-job'ов (9103, 9104, 9107, 9110, 9112), и только 9107-хендлер способен в сессии создать reservation-callback с `mode!=0` (39.2-39.3). Клиент **ничего не отправляет** в GC при Accept (39.2). Кто и по какому событию шлёт этот 9107 **в retail-инфраструктуре** — **в доступных бинарниках отсутствует**: `server.dll` — публичная сборка «Not Valve DS» без GC-репортинга, engine лишь вызывает `ReportGCQueuedMatchStart` (39.6). Поэтому «источник» на стороне GC остаётся UNRESOLVED, а на стороне клиента — CONFIRMED.

**Что доказано и что нет**: 9104 не является триггером (§38.6, повторно подтверждено замыканием); 9110 (Hello, `ongoingmatch`) не может дать connect в сессии без флага, записываемого только при старте (39.4); project446 **не** доказывает retail-поведение, но независимо согласуется (39.7).

### 39.2 Замкнутый инвентарь matchmaking-сообщений клиента (`client.dll`)

**GC → client** (реестр job'ов; запись `{name, msgid, flags=0x1f, factory}` в `.data`, найдена сканом):

| msgid | job (vtable) | factory | тело-обработчик | влияние на reservation-state |
|---|---|---|---|---|
| **9107** `GC2ClientReserve` | `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (`0x10bcccb4`) | `sub_103F4D30` | **`sub_103F49F0`** | **создаёт callback `(1,0)`/`(2,2)`, `mmqueue=reserved`** (§38.5) |
| 9104 `GC2ClientUpdate` | `…GC2ClientUpdate` (`0x10bccd84`) | `sub_103F4060` | `sub_103F1C00` | только `searching`/`connect`-маркеры, `RaiseReadyUp(false,0,0)`; connect не запускает; пропускается при `cb.mode==2` |
| 9110 `GC2ClientHello` | `…GC2ClientHello` (`0x10bccda0`) | `sub_103F1A60` | `sub_103F19D0`→`sub_103F0D20` | пишет `cfg/qmmconnect.dt` при `ongoingmatch`, событие `ScaleformComponent_GC_Hello` (39.4) |
| 9112 `GC2ClientAbandon` | `…GC2ClientAbandon` (`0x10bccc48`) | `sub_103F5100` | `sub_103F4D90` | только уведомление-попап (`"Matchmaking abandon notification"`) |
| 9103 `Client2ServerPing` | `…Client2ServerPing` (`0x10bccd50`) | `sub_103F41C0` | `sub_103F40C0` | пинг-список; **не** трогает reservation-глобалы (проверено) |

Других matchmaking-job'ов в `client.dll` нет (RTTI-перечень `q39_a`). Класс `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` встречается в `ProtoBufMsg`-обёртке только в `sub_103F49F0` (+ dtor/ctor-хелпер `sub_103F9340`); во вложенном виде — в `GC2ClientHello.ongoingmatch`.

**client → GC**: `Start` (9101) — только `sub_103F6240` (Play/heartbeat); `Stop` (9102, `abandon`) — `sub_103F6DD0` (cancel), `sub_103EFB20` (leave; вызывается из `sub_1054A540`), гейты 9104/9107 (`sub_103F1C00`/`sub_103F49F0`, когда `mmqueue` пуст); ответ на 9103. **Ни в `sub_103F7C00` (callback), ни в `sub_105C3D90` (JS `SetLocalPlayerReady`), ни в `sub_103F7B60` (stage→2) GC-сообщений нет** — **Accept уходит только UDP `0x21 stage=2` на game-сервер** (CONFIRMED). Соответствует комментарию p446 `"legacy client omits 9102 on Accept"`.

**Замыкание по состоянию**: глобал активного callback `dword_152EAD0C` (client_panorama `dword_15278690`) трогают только: `sub_103F1C00` (9104), `sub_103F49F0` (9107), `sub_103F6240` (Play), `sub_103F6DD0` (cancel), `sub_103F7400` (reconnect-из-GC-Hello), `sub_103F7B60` (stage→2), `sub_103F7C00` (callback), `sub_105C3D90` (JS `SetLocalPlayerReady`). Глобал сохранённого QueueConnect-KV `dword_152EAD18` — только `sub_103F6DD0`, `sub_103F7240`, `sub_103F7350` (poller). **CONFIRMED (xref-скан)**.

### 39.3 Кто создаёт callback с `mode != 0` (единственные пути к QueueConnect)

Единственные вызовы ctor `sub_103F7980(kv, stage, mode)`: `sub_103F49F0` (9107: `(1,0)` accept-типы, `(2,2)` прочие), `sub_103F6240` (Play, ветка `"reconnect"` → `(2,1)`), `sub_103F7400` (`(2,1)` по флагам GC-Hello), хвост `sub_103F7C00` (`(2,1)` «will reconnect to ongoing match»). Из GC-сообщений в сессии — **только 9107** (два остальных — локальный клик «reconnect» и Hello-путь, см. 39.4). Следовательно после `0x25 awaiting=0` в accept-режиме (callback `(1,0)→stage 2`, `mode 0`, где `sub_103F7C00` возвращается без действия, §38.4) дальше возможен **только новый 9107 с не-accept `game_type`**: 9107-хендлер уничтожает старый callback (`sub_103F7AF0`), создаёт `(2,2)`, ctor немедленно регистрирует 0x21 stage 2, сервер отвечает 0x25 status 4, `sub_103F7C00` (mode≠0) строит KV `QueueConnect` → `sub_103F7240` (store) → per-frame poller `sub_103F7350` → `Command("QueueConnect")` (§38.8-38.10).

### 39.4 Альтернативы «не 9107»: 9104 и 9110 (Hello)

- **9104**: пишет только `game/mmqueue` (`searching`/`connect`) и закрывает ready-up при `matchmaking==3`; **не** создаёт/меняет callback (он лишь читает `cb.mode`); `mmqueue=connect` не является триггером (читатели строки «connect» — только сравнения в `sub_103F7C00`/9104-очистке/`sub_103DE960`-аналоге). **CONFIRMED** (xref-замыкание 39.2).
- **9110 `GC2ClientHello` (`ongoingmatch` = вложенный `ClientReserve`)**: `sub_103F0D20` печатает `"Client hello received: … ongoingmatch/ok"`, при `has(ongoingmatch)` копирует поля (`sub_103F83B0`) и **пишет `cfg/qmmconnect.dt`** (`sub_103F8500`); при отсутствии — удаляет файл. Reconnect-callback `(2,1)` создаётся `sub_103F7400` (вызывается из обработчиков события `ScaleformComponent_GC_Hello`, `sub_105C2810`/`sub_105C2E20`) **только если `dword_1520ED8C & 2`**; этот бит выставляется **единственным** местом `sub_103F0AD0` (`or dword_1520ED8C,2` @ `0x103f0af8`), которое **читает `cfg/qmmconnect.dt` при старте компонента** (вызов из `sub_105C4230`). Значит, Hello внутри сессии connect **не** запускает; это путь «reconnect после перезапуска клиента» (файл, кстати, пишет и ctor callback `sub_103F7980` через `sub_103F8500` @ `0x103f7a32` — т.е. любая reservation персистится). **CONFIRMED (файл/флаг), HIGH (что во время Accept-сессии бит не выставлен)**.
- **Локальный клик «reconnect»** (`game/mapgroupname=="reconnect"` в `sub_103F6240`) — пользовательское действие, не post-accept.

### 39.5 Первый 9107 vs второй 9107 — что клиент требует (data-flow, `client.dll`)

Общий обработчик `sub_103F49F0` (§38.5); отличие только в значении `reservation.game_type & 0xF` и в наличии полей:

| Поле (`CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`) | FIRST 9107 (ready-up) | SECOND 9107 (connect) | Использование клиентом |
|---|---|---|---|
| `reservation` (5) / `reservation.game_type` | **обязателен**, `game_type&0xF ∈ {8,9,10,11,13}` | **отсутствует** ⇒ default instance ⇒ `game_type=0`, либо `&0xF ∉ accept-set` | единственный селектор ветки; `this+92` (`cb`) |
| `reservationid` (4) | обязателен (cookie сервера) | **обязателен заново** (перечитывается из нового msg) | 0x21 (cb+40/44), KV `reservationid`, `UpdateClientReservation` → cookie connect-пакета |
| `direct_udp_ip` (2) + `direct_udp_port` (3) / `server_address` (7) | обязательны | **обязательны заново** | статический staging-адрес (`sub_103F83B0`, аналог адресного построителя `sub_103DFED0`) ⇒ `cb+96` ⇒ цель 0x21; `adronline` = `sub_103EF6C0(cb+96)` (HIGH) |
| `map` (6) | желательно | желательно | `Update/game/map`, `cb+68`, KV `map`, `ServerReserved("@map")` |
| `serverid` (1) | не используется найденными потребителями | то же | (MEDIUM) |
| `reservation.account_ids`/`party_ids`/`rankings` | не читаются | не читаются | — |

**Нет fallback на ранее сохранённый адрес**: `sub_103F83B0` (≡`sub_103DFBE0`) безусловно копирует поля нового сообщения; отсутствующие поля = нули/пустые строки ⇒ 0x21 и `connect` ушли бы в никуда. **CONFIRMED (код)**.

**Почему `game_type=0`**: 9107-хендлер проверяет `v10 = game_type & 0xF` по набору `{9,13,11,8,10}`; всё остальное (в т.ч. `0`, `7`) даёт `(stage 2, mode 2)`. Затем в QueueConnect-ветке `sub_103F7C00`: `sub_103EF770(game_type)` (accept-required?) ложь ⇒ `v29=1`, звук `popup_accept_match_found`, `ServerReserved("@"+map)` (NQMM-попап), `helper_time=now+2000`, `sub_103F7240(KV,1)` (store без события); KV `gametype/gamemode`=`unknown/unknown` при `game_type==0` (на connect не влияет; наш live-проверенный Casual идёт именно так — 9107 без `reservation`). Дальнейшая цепочка `sub_103D7750`→`QueueConnectToServer(map, 2.1)` (raiser `sub_103DEA80`≡`sub_103F7240`, при `a2==2` из JS `SetLocalPlayerReady('deferred')` после 1.9 c NQMM-попапа) и poller — §38.7-38.8. **CONFIRMED**.

### 39.6 Server-side flow: что есть в доступных бинарниках

**`server.dll` (retail 2021, build 1352) — публичная «Not Valve DS» сборка**:
- `CServerGameDLL` vtable `0x1085cb10`: slot 41 `sub_1029C640` = `DevMsg("Not Valve ds: Direct-connect is allowed\n"); return 1;` (p446 патчит именно его, называя `IsValveDS`), slot 47 `sub_10145510` = `return 0` (`ReportGCQueuedMatchStart`, §37.8).
- **Нет job'а для 9105 (`GC2ServerReserve`)**: в RTTI `server.dll` из GC-сообщений есть лишь `CProtoBufMsg<ServerReservationResponse>` (9106, отправка), `ClientJob…GC2ServerReservationUpdate` (9142, только счётчики зрителей), `Server2GCClientValidate` (9153), `ServerNotificationForUserPenalty`, `MatchEndRunRewardDrops`/`MatchEndRewardDropsNotification`, `GiftsLeaderboard*`, `ServerVarValueNotificationInfo`, `GiftedItems`/`ItemAcknowledged`, `IncrementKillCountAttribute` и служебные `ServerWelcome`/`ServerUpdateVersion` (`CGCClientJobServerWelcome`/`CGCClientJobServerUpdateVersion`). Классы `GC2ServerReserve`/`RoundStats` присутствуют лишь как сгенерированный protobuf-код; **`CProtoBufMsg<RoundStats>` нет ⇒ `server.dll` не шлёт `MatchmakingServerRoundStats`** (несмотря на поле `reservation_stage`).
- **Единственный вызов `IVEngineServer::ReserveServerForQueuedGame` (slot 149) в `server.dll`** — `CGCClientJobServerWelcome` (`sub_1059BEB0`, vtable data `0x1092ee70`): при `ServerWelcome` с полем-reservationid вызывает с строкой **`"R%p"`** (`qword_10B77B90`). Строки `G…`/`Q…` server.dll не формирует ⇒ **retail-путь GC→server reservation `Q` отсутствует в этих бинарниках** (закрывает §37.13 п.10).
- `MatchmakingServerReservationResponse` (9106) шлёт `sub_103E7AE0` (метод `CServerGameDLL`, vtable slot 46, data `0x1085cbc8`) — **периодический статус** (перепосылка каждые `RandomInt(480,720)` c либо при смене карты/флагов/версии/адреса): поля из `sub_103E7890` — `server_version` (`INETSUPPORT_003`+36), карта, `tv_info`/`tv_advertise_watchable`, публичный адрес (engine slot 150 `sub_101B5130`), списки аккаунтов клиентов по их состоянию (`reward`/`idle`). **Стадии Accept не содержит** ⇒ **не является уведомлением о завершении Accept** (HIGH; `sub_103E7890` декомпилирован целиком).

**`engine.dll` — единственный «completion»-хук**: `CBaseServer::ReplyReservationCheckRequest` (`sub_101BC1D0`) для `Q`-резервации (`*sv_mmqueue_reservation=='Q'`) при **каждом** `0x21` от участника roster'а пересчитывает `minStage` (минимум по roster'у) и список аккаунтов со `stage>=2 ∨ stage>minStage`, печатает `"Match start status: %u/%u"` и вызывает **`serverGameDLL->ReportGCQueuedMatchStart(minStage, accounts[], count)`** (vtable+188, slot 47); при ненулевом результате и `stage==2` принудительно ставит всем stage 2 (встроенный spoof) и печатает `"Reservation time extended +%d sec"`. То есть **retail-архитектура «сервер → GC»: репорт `minStage`+подтверждённые аккаунты при каждом продвижении Accept**; реализация — в непубличном `server.dll` Valve DS (в нашем — заглушка). Дальнейшая реакция GC — **вне доступных бинарников (UNRESOLVED)**.

Вывод: последовательность `client Accept → srcds 0x21 → 0x25 awaiting=0 → server→GC report → GC → client 9107(non-accept)` **согласована с клиентом на 100% на клиентской стороне (39.3) и с engine на серверной (репорт-хук), но GC-звено недоступно**. Другого server→GC механизма завершения в `server.dll`/`engine.dll` нет (проверены: все GC-обёртки `server.dll`, единственный call slot 149, единственный call slot 47).

### 39.7 project446 — строгая проверка (не доказательство retail)

- **`sub_100789A0` — НЕ отправитель 9107**: это обработчик **`case 9102` (client→GC `MatchmakingStop`)** в switch `ClientGC` (jumptable `0x100712A3`, вызов `0x10071358`; там же `case 9109`→`sub_10079660`, `9194`, `9171`). Парсит `CMsgGCCStrike15_v2_MatchmakingStop`, при состоянии `2` (ready-up) и `abandon==0`, accept-тип (`sub_100694A0`: `{8,10,13}`) логирует `"Matchmaking: Player ACCEPTED, waiting for all players"` и **пересылает `9102` на их backend** (`sub_100BFC20(9102,…)`); `abandon!=0` → штраф-путь. Прежняя рабочая формулировка «`sub_100789A0` и возможный второй 9107 с `game_type=0`» (§35/§37.9/задание §39) — **уточнена**: `game_type=0` в `sub_100789A0` не задаётся.
- **`sub_100787F0`** (лямбда `_lambda_17_` в `ClientGC::ClientGC`, `sub_10081A80`): «Accept hook» — нативное Accept-событие → синтетический `MatchmakingStop(abandon=0)` (`"legacy client omits 9102 on Accept"`) → `sub_10073AD0` → далее как выше.
- **`sub_1006EAD0`** (лямбда `_lambda_16_` `void(const MatchFoundInfo&)`, `sub_10081AC0`): приём «reserve» от backend. `game_type = *(a2+68)`: ≠0 (accept-тип) → `"[GC] Match found!"`, состояние 2, **9104 (`matchmaking=2`) + `sub_100774E0(game_type,0,0,1)` (9107 с `reservation.game_type=game_type`)** в игру; `==0` при состоянии 2 или флаге `+1736` → `"[GC] Connect reserve received (all players accepted)"`, состояние 3, `sub_100772B0()` (**9104, `matchmaking=4` (HIGH), `waiting=[local account]`**, msgid `0x80002390`) + `sub_100B9B80(4,…)` (host-событие типа 4; назначение не расшифровано); `==0` без ready-up → `"Live join: server sent game_type=0"`.
- **`sub_100774E0(a3,a4,a5,a6)`** (`0x100774E0`) — строит и отправляет игре **9107** (`msgid 0x80002393`): `reservationid` (`this+1672`), адрес (`ip/port` либо SDR `serverid`+`server_address="=[A:…]:0"`), `map`, вложенный `reservation{account_ids=[local], game_type=a3 (has-bit), match_id=(accept-тип ∧ !a4 ? resId : 0)}`; при `a4&&a6` предварительно шлёт 9104(4). Connect-вариант `sub_100774E0(0,1,1,x)` ⇒ **`reservation.game_type=0` присутствует явно**.
- **Нативный путь p446 (главный)**: `sub_100F5D00` (main-thread queue) `case 3` — 12-байтный «ReadyUp counter»-событие backend'а (`resId u64 + count u32`): отменяет активный **нативный `(mode 0)` check** (`"[ReadyUp] cancelled native reservation check %p (kind=%u stage=%u)"`, освобождает engine-запрос `cb+128`); затем **`sub_100EA7B0`** (`"native connect armed (check=%p kind=%u stage=%u)"`) находит в `client.dll` по сигнатуре (`sub_100EA580`, `GC_CLIENT_READYUP_ACTIVE_RVA`/`_COMP_RVA`; `activeCheck` = глобал по смещению `+36` в найденной функции) функцию, вызывает её и требует итог **`cb.mode==1 ∧ cb.stage==2`** — т.е. **p446 «перевооружает» accept-callback в reconnect-kind (2,1)** (MEDIUM: функция — по побочным эффектам, вероятно эквивалент `sub_103F7400`; сигнатура байтами не сверена); при неудаче — сообщение **`"falling back to the game_type=0 reserve"`** и отправка 9107 `game_type=0` (`sub_100774E0(0,1,1,0)`).
- **Server-side p446**: `sub_100BADE0` патчит `server.dll` vtable (slot 47 `ReportGCQueuedMatchStart` → `sub_100BAD90`, slot 41 → `IsGCSendAvailable`, slot 4 `GameFrame`); хук вызывает оригинал и при `accounts!=null ∧ 1≤count≤64` вызывает `sub_100992A0` — упаковывает `[minStage u8][count u8][accounts u32…]` и шлёт на backend (`dword_1074AB84`). Строки: `"ready-up accepts now come from the engine"` vs `"…stay a guess from MatchmakingStop"`.
- **Вывод (HIGH, не CONFIRMED-retail)**: независимая реализация p446 подтверждает три факта §38-§39 — (1) нативный `mode 0` check после Accept сам connect не запускает (их код его отменяет/перевооружает), (2) рабочий конечный сигнал — 9107 `game_type=0` (fallback), (3) «все приняли» приходит от `ReportGCQueuedMatchStart`. Retail-звено GC→клиент p446 **не** доказывает (backend их собственный).

### 39.8 Цепочка data-flow до QueueConnect (максимально ранний подтверждённый триггер)

```
[SERVER→GC (retail, недоступно)] engine sub_101BC1D0 → serverGameDLL slot 47 ReportGCQueuedMatchStart(minStage=2, accounts)     — UNRESOLVED дальше
[GC→client] MatchmakingGC2ClientReserve (9107): reservationid + адрес + map, БЕЗ reservation / game_type&0xF ∉ {8,9,10,11,13}    — CONFIRMED требования клиента
   ↓ job factory sub_103F4D30 → handler sub_103F49F0 (client.dll)
[STATE] mmqueue=reserved; старый cb (1,0) уничтожен (sub_103F7AF0); новый cb (2,2) = sub_103F7980; 0x21 stage 2 (INETSUPPORT_003 vtbl+60)
   ↓ srcds 0x25 awaiting=0 → engine sub_1008A610 state=4 → cb.vftable[0] = sub_103F7C00
[STATE] mmqueue=connect; KV QueueConnect{adronline, reservationid, helper_pSession, helper_time=+2000, …}; NQMM ServerReserved("@map")
   ↓ sub_103F7240(KV,1) store (dword_152EAD18)          (client_panorama: sub_103DEA80; QueueConnectToServer builder sub_103D7750 — при a2==2, из JS 'deferred')
   ↓ JS 1.9 c: SetLocalPlayerReady('deferred') → sub_105C3D90 → sub_103F7240(0,2) → QueueConnectToServer(map, 2.1) → LoadingScreen.Init
   ↓ per-frame CUiComponent_Lobby::Update (client.dll sub_105C2330) → poller sub_103F7350 (helper_time+2100, нет loading plaque, та же сессия)
   ↓ session->Command("QueueConnect") → matchmaking.dll sub_1001B290 → UpdateClientReservation + StartLoadingScreenForCommand("connect adr") → Cbuf_AddText → `connect`
```

### 39.9 Поправки к §37/§38 (старое не удалялось)

| # | Было | Стало |
|---|---|---|
| C1 | §38.1/38.13: «второй 9107 — единственный клиентски-допустимый триггер» | Уточнено: **единственный внутрисессионный GC-триггер**; существуют локальные reconnect-пути `(2,1)` (Play-`reconnect`, GC-Hello при флаге `&2` из `cfg/qmmconnect.dt`), которые p446 использует как «native connect» (39.7). Post-accept GC-триггер = 9107; иного GC-сообщения нет (замыкание 39.2). |
| C2 | §37.9/§35: `sub_100789A0` ассоциировался с вторым 9107 `game_type=0` | `sub_100789A0` = `case 9102` (Stop/Accept-приём); `game_type=0` 9107 формирует `sub_100774E0` из `sub_1006EAD0` (MatchFoundInfo с `game_type==0`); перед ним шлётся 9104 `matchmaking=4`. |
| C3 | §37.13 п.10: «обработчик 9105 в retail server.dll не найден» | **Отсутствует по сути**: retail `server.dll` (Not-Valve-DS) не имеет job'а 9105 и не вызывает slot 149 со строками `Q/G` (только `"R%p"` из ServerWelcome). |
| C4 | §37.8: `ReportGCQueuedMatchStart` — «заглушка, spoof неактивен» | Дополнено: вызывается engine'ом при каждом `0x21` `Q`-roster'а с `minStage` и списком `stage>=2 ∨ >min`; в Valve-DS server.dll это, очевидно, канал server→GC (в p446 — их backend). |
| C5 | §38.4 `sub_103DF1C0` не упоминал персист | Ctor `sub_103F7980` (client.dll) вызывает `sub_103F8500` ⇒ каждая reservation пишется в `cfg/qmmconnect.dt` (reconnect-данные); читается при старте (`sub_103F0AD0`). |
| C6 | `MatchmakingServerReservationResponse` (9106) рассматривался как возможный «completion»-канал | Это периодический status/advert сервера (без stage); не completion (39.6). |

### 39.10 Не доказано (сжато)

Как именно retail-GC решает отправить 9107 #2 и какие точно поля кладёт (нет GC/Valve-DS бинарников); тайминг между `ReportGCQueuedMatchStart(minStage=2)` и 9107 #2; наличие/отсутствие `reservation` во втором 9107 (клиенту безразлично, если `game_type&0xF` не accept); точное условие `*(this+4)` в `sub_103F7400` (для reconnect-пути) и способ, которым p446 выставляет `dword_1520ED8C&2`; когда retail-server шлёт `Server2GCClientValidate`(9153) (не Accept); `serverid` (потребитель не найден).

### §39 STATUS

CONFIRMED:
- `client.dll` имеет ровно 5 GC→client matchmaking-job'ов: 9103, 9104, 9107, 9110, 9112 (msgid→factory→handler в 39.2); 9107 — единственный, создающий reservation-callback в сессии.
- Клиент **не отправляет GC-сообщений при Accept и при `0x25`-обработке**; Accept = UDP `0x21 stage 2`.
- Callback после Accept в accept-режиме `(stage 2, mode 0)` ничего не делает при status 4; connect возможен только через callback `mode!=0`, создаваемый (в сессии) лишь 9107 с не-accept `game_type` (либо локальными reconnect-путями `(2,1)`).
- Второй 9107 должен нести **`reservationid` и адрес заново** (нет fallback на сохранённые); `reservation` необязателен (default ⇒ `game_type=0`); `account_ids/party_ids/rankings` клиентом не используются.
- 9104 не запускает connect; 9110 не запускает connect внутри сессии (флаг `&2` из файла при старте).
- `server.dll` (Not-Valve-DS): нет job'а 9105, нет `Q/G`-вызовов slot 149 (только `"R%p"` из `ServerWelcome`), нет `RoundStats`, `9106` — периодический status без stage, slot 41 = «Not Valve ds…», slot 47 = `return 0`.
- engine `sub_101BC1D0` вызывает `ReportGCQueuedMatchStart(minStage, accounts[], count)` на каждый `0x21` `Q`-roster'а.
- project446: `sub_100789A0` = обработчик `case 9102` (не отправитель 9107); 9107 со значением `reservation.game_type=a3` (для connect — 0) строит `sub_100774E0`; server-hook slot 47 (`sub_100BAD90`) → `sub_100992A0` → backend; строки p446 `"native connect armed…"` / `"falling back to the game_type=0 reserve"`.

HIGH CONFIDENCE:
- p446: перед connect-9107 шлётся 9104 `matchmaking=4`; native-путь p446 — отмена mode-0 check и перевооружение в `(2,1)` (`kind==1 ∧ stage==2`), при неудаче fallback на 9107 `game_type=0`.
- Retail-GC отправляет клиентам 9107 (не-accept) после того, как сервер (Valve-DS `server.dll`) репортит `minStage>=2` через `ReportGCQueuedMatchStart` (замыкание на клиенте + engine-хук + независимая реализация p446). Доказательство GC-звена отсутствует.
- Во время Accept-сессии бит `dword_1520ED8C&2` не выставлен (устанавливается только при старте из файла).
- Ctor callback персистит reservation в `cfg/qmmconnect.dt`.

HYPOTHESIS:
- Во втором retail-9107 `reservation` отсутствует либо `game_type=0` (клиенту неважно).
- Локальный reconnect `(2,1)` мог бы служить альтернативным test-триггером без 9107 #2 (условия `sub_103F7400` не сняты до конца).

UNRESOLVED:
- Точное retail-поведение GC (когда/какими полями шлёт 9107 #2); Valve-DS `server.dll` реализация `ReportGCQueuedMatchStart`.
- Способ у нас: источник сигнала «все приняли» для `ClientGC` (нет канала `srcds`→клиент), account реального игрока на сервере, fake-stage 2, Danger Zone Q-roster, cookie lifecycle (§34).

**Ответ на главный вопрос**: **Является ли post-Accept trigger вторым `MatchmakingGC2ClientReserve (9107)`? — ДА** (для внутрисессионного GC→client канала это единственный возможный вариант; CONFIRMED на клиентской стороне). Сообщение: `CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (msgid 9107, protobuf-флаг) с полями `reservationid`, `direct_udp_ip`, `direct_udp_port` (или `server_address`), `map` и **без** `reservation` либо с `reservation.game_type & 0xF ∉ {8,9,10,11,13}`; приходит при активном `game/mmqueue` (иначе клиент шлёт `MatchmakingStop`). Что оно вызывает: уничтожение accept-callback `(1,0)`, создание `(2,2)`, 0x21 stage 2 → 0x25 status 4 → KV `QueueConnect` → `QueueConnectToServer(map,2.1)` → connect.

### NEXT STEP AFTER §39

1. **Решение пользователя по каналу сигнала «все приняли»** (единственный оставшийся блокер для harness): (a) test-only канал `ServerGC`→`ClientGC` (например, простое TCP/UDP-уведомление; `ServerGC` получает `minStage==2` через vtable-хук slot 47 `ReportGCQueuedMatchStart`, как p446 `sub_100BAD90`, либо через собственный учёт 0x21); (b) локальная test-эвристика на клиенте (таймер после Accept) — не retail-эквивалент; (c) reconnect-путь `(2,1)` (нужен отдельный RE условий `sub_103F7400`, `dword_1520ED8C&2`, `*(this+4)`).
2. После решения — **дизайн** (не код) двухфазного 9107 для `ClientGC`: 9107 #1 `reservation{game_type∈accept,…}`, 9107 #2 `{reservationid, direct_udp_ip/port, map}` без `reservation`; и серверной части (`Q`-roster `[real][fake…]`, fake-0x21 stage 1/2, источник account реального игрока).
3. Перед кодом — короткая **live-проверка только логами существующего прототипа**: отправить первый 9107 accept-типа с корректным `reservation` и убедиться (по `console.log` + `developer 2`), что клиент показывает ready-up (`Server reservation check … ready-up!`) и после ручного Accept зависает на `mode 0` (ожидаемое поведение §38.4) — валидирует модель без правок логики.
4. Отложено: cookie lifecycle/повторная reservation (§34), Danger Zone Q-roster, полный retail-GC (недоступен).

---

## 40. ReportGCQueuedMatchStart / Server→GC Completion Path (read-only статический RE)

**Режим**: только чтение. Код/`.cpp`/`.h`/CMake/config/protobuf/server.dll/engine/backend не менялись, сборка/деплой/Git не выполнялись, runtime не понадобился. Источники: оба source-дерева (`D:\cstrike15_src-master` = более ранний дамп с комментариями в `.proto`; `D:\ida_deobfuscated_grok\...\src` = более поздний), IDA-базы `server.dll`, `engine.dll`, `client.dll`, `p446_csgo_gc.dll`, а также Linux-сборки из `csgo\bin` (`server.so` 32-бит — новая; `server_i486.so` — старая). Скрипты/дампы: scratchpad `...\re\q40_a..q.py`. Уровни: **CONFIRMED / HIGH / HYPOTHESIS / UNRESOLVED**.

### 40.1 Короткий ответ

- **Контракт `ReportGCQueuedMatchStart` восстановлен полностью** — по source `engine/baseserver.cpp` (совпадает с бинарником `engine.dll sub_101BC1D0`): engine вызывает её **не «на каждый 0x21»**, а **только когда 0x21 повысил stage конкретного участника roster'а** (`bThisClientJustConfirmed`, т.е. 0→1 и 1→2), для резервации типа `Q`. Параметры: `minStage` = минимальный stage по всему roster'у; `accounts[]` = `uint32` AccountID участников с `stage>=2 ∨ stage>minStage` в порядке roster'а; `count` = их число. Возврат `bool` (`bReady`) включает встроенный spoof auto-accept.
- **Тело функции в оригинальных исходниках УДАЛЕНО** («`Removed for partner depot`») — в обоих деревьях; в retail-бинарниках (Windows `server.dll` slot 47 `0x10145510` и Linux `server.so` `0xAF9BA0`) — заглушка `return 0`. То же удалено: GC-job на `9105`/хранилище `sm_QueuedServerReservation`, `ReportRoundEndStatsToGC`, `NetworkIDValidated`. **Оригинальная реализация server→GC для queued-match НЕ сохранилась ни в source, ни в binary.**
- **Где формируется GC→client 9107** — ни в одном доступном дереве/бинарнике: это код внешнего GC (`k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve`: «GC reports to clients matchmaking reservation»). Известно только назначение и то, что клиент делает с сообщением (§38-§39).
- **Архитектурный вариант**: literal **A опровергнут**, **B подтверждён** для оригинального server→GC пути (реализация удалена, отправителя нет, у нас нет транспорта `ServerGC`↔`ClientGC`), но RE выявил **C′**: та же информация «все приняли» **уже доставляется каждому клиенту по UDP** (`0x25 awaiting=0`, broadcast всем участникам) и попадает в client-callback (`sub_103F7C00`, no-op при `mode 0`) — отдельный server→GC→client bridge не единственный способ (40.9).

### 40.2 Где объявлена и реализована — таблица

| Место | Что |
|---|---|
| `public/eiface.h` (grok `:688`; ранний `:679`) | `virtual bool ReportGCQueuedMatchStart( int32 iReservationStage, uint32 *puiConfirmedAccounts, int numConfirmedAccounts ) = 0;` (в раннем дереве `void`) — метод `IServerGameDLL` |
| `game/server/gameinterface.h` (`:147`) | `CServerGameDLL::ReportGCQueuedMatchStart`, комментарий **«Marks the queue matchmaking game as starting»**, стоит сразу после `UpdateGCInformation()` |
| `game/server/cstrike15/cs_gameinterface.cpp` (grok `:156-160`; ранний `:155-158`) | **тело пустое**: `/** Removed for partner depot **/` (grok-дерево `return true;`, ранний — `void`) |
| `engine/baseserver.cpp` (grok `:2226`; ранний `:2183`) | **единственный вызывающий**: `CBaseServer::ReplyReservationCheckRequest` |
| retail `server.dll` (1352) | `CServerGameDLL` vtable `0x1085cb10` slot 47 `sub_10145510` = `return 0` (5 байт) |
| retail Linux `server.so` (32-бит, 24 МБ) | vtable `_ZTV14CServerGameDLL` `0x114A5C0`, slot 47 `sub_AF9BA0` = `return 0` — **идентично Windows**; slot 44 `IsValveDS` `sub_6BFAA0` = `return 0`; slot 46 `sub_AFAFD0` (реальный код, ~935 байт) |
| Linux `server_i486.so` (12 МБ) | **старая** сборка: vtable `CServerGameDLL` 29 слотов (до `GetStandardSendProxies`), QMM-интерфейса нет — не релевантна |
| `engine.dll` | вызов `(*(dword_139097E8 vtbl+188))` (`sub_101BC1D0`, `0x101bc69e-0x101bc6b8`), `dword_139097E8` = `serverGameDLL` |
| других переопределений/подклассов/function pointers/registration | **не найдено** ни в одном дереве (grep по `ReportGCQueuedMatchStart|QueuedMatchStart`) |

**Раскладка слотов `CServerGameDLL` (по порядку объявления в `gameinterface.h`, подтверждена содержимым слотов в retail `server.dll` и `server.so`)**: 41 `ShouldAllowDirectConnect` (тело печатает `"Not Valve ds: Direct-connect is allowed\n"`, `return 1` — это **штатный код source**, `gameinterface.cpp:2083`), 42 `FriendsReqdForDirectConnect`, 43 `IsLoadTestServer` (`-sv_load_test`), **44 `IsValveDS`** (`return IsValveDedicated()`; в public-дереве `public/const.h:472` **`#define IsValveDedicated() false`** ⇒ retail `return 0`), 45 `GetExtendedServerInfoForNewClient`, **46 `UpdateGCInformation`** (retail: реальный код `sub_103E7AE0`), **47 `ReportGCQueuedMatchStart`**. **Поправка к §39.6**: строка «Not Valve ds…» — это slot 41 и обычный source-код, а не признак «урезанной сборки»; признак — slot 44 `IsValveDS ≡ false` (макрос `IsValveDedicated()=false` в public-сборке).

### 40.3 Точная семантика вызова (source ≡ binary)

Код (`engine/baseserver.cpp:2165-2308`, `CBaseServer::ReplyReservationCheckRequest`), условия входа: `uiReservationStage!=0 ∧ reservationMatch(cookie) ∧ uiClientSteamID!=0 ∧ sv_mmqueue_reservation[0]=='Q'`; для `'G'` вызова **нет** (там `awaiting=0`).

1. Игрок ищется в `m_arrReservationPlayers` по `CSteamID(uiClientSteamID).GetAccountID()` (`bThisClientShouldJoin`); сохраняются `adr`, `token`; **`bThisClientJustConfirmed = (qmp.m_uiReservationStage < uiReservationStage)`** — только если stage игрока **строго вырос**; затем `qmp.stage = uiReservationStage`. Считается `uiActualAwaitingClients` = число игроков со stage `< uiReservationStage`.
2. **Только при `bThisClientJustConfirmed ∧ serverGameDLL`**: `uiMinReservationLevel` = `min(stage)` по roster'у (стартует с `0xFFFF`); `arrConfirmedAccounts` = `m_uiAccountID` (в порядке roster'а) тех, у кого **`stage>=2 ∨ stage>min`**; `DevMsg("Match start status: %u/%u\n", min, count)`; **`bReady = serverGameDLL->ReportGCQueuedMatchStart(min, arrConfirmedAccounts.Base(), count)`**. Комментарий в source: *«Report to the GC the new level of confirmations»*.
3. **`bSpoofForcefulConnect = uiActualAwaitingClients ∧ bReady ∧ (uiReservationStage==2)`**: при `true` всем игрокам roster'а (с ненулевым account) ставится `stage=2` («We are spoofing auto-accept»), `uiActualAwaitingClients=0`.
4. **`if (!uiActualAwaitingClients ∨ bSpoof)`**: `m_flReservationExpiryTime = net_time + sv_mmqueue_reservation_extended_timeout` (21 c по умолчанию), `DevMsg("Reservation time extended +%d sec (%d, %d)")` — «match is now confirmed to be starting».
5. Ответ `0x25` отправителю; **если `!uiAwaitingClients` — broadcast `0x25` всем участникам roster'а** на сохранённые `adr/token` (source: «force them to connect … as soon as everybody confirmed»).

**Binary-сверка**: `engine.dll sub_101BC1D0` строки `0x101bc533 "Reservation from client %u: %u"` / `0x101bc69e "Match start status: %u/%u"`; переменная `v69` (=`bThisClientJustConfirmed`) ставится только при росте stage игрока (`*(v36+40) < v66`), вызов идёт под `if (v65 ∧ v69 ∧ dword_139097E8)`; аргументы `(v42=minStage, v79=accounts ptr, HIDWORD(v79)=count)`; отбор `v46>=2 ∨ v46>v42`; spoof/extend — как в source. **CONFIRMED (source и decompile совпадают)**.

**Ответы по параметрам**:
- `minStage` — минимум `m_uiReservationStage` по roster'у (`0` пока кто-то ещё не прислал stage 1; `1` когда все «пробили» соединение; `2` когда все нажали Accept). Stage-значения из proto-комментария `RoundStats.reservation_stage`: **«1 = connection probing, 2 = ready-up»**.
- `accounts[]` — `uint32` AccountID (не SteamID64) **в порядке токенов `[%x]` Q-payload** (нулевые пропущены, `SetReservationCookie`, `baseserver.cpp:4217-4296`); содержит игроков, продвинувшихся выше текущего минимума либо достигших stage 2. `Base()` пустого вектора может быть `NULL` (p446 проверяет `a3!=null`).
- `count` — размер `accounts[]`; **не** размер roster'а.
- Вызывается **и на stage 1, и на stage 2, и отдельно для каждого игрока** (каждое повышение), **не только при полной готовности**. Последний вызов при полном Accept: `minStage=2`, `count==размер roster'а`.
- Не вызывается: при повторных 0x21 того же stage (retry engine-запроса ~1 c), при неизвестном account (`awaiting=127`), для `G`, при `serverGameDLL==NULL`.

**Пример** (roster `[R,F1,F2]`, все stage 0): `R:1` → `Report(0,[R],1)`; `F1:1` → `Report(0,[R,F1],2)`; `F2:1` → `Report(1,[],0)` + `awaiting=0`⇒broadcast⇒попап; `R:2` → `Report(1,[R],1)`; `F1:2` → `Report(1,[R,F1],2)`; `F2:2` → **`Report(2,[R,F1,F2],3)`** + `awaiting=0`⇒broadcast + `Reservation time extended`.

**Поправка к §39.6/C4**: «на каждый 0x21» неточно — только на **повышение stage** участника.

### 40.4 Что именно удалено и что осталось (server-side GC-инфраструктура)

Помечено **`Removed for partner depot`** (grok-дерево): `CServerGameDLL::UpdateGCInformation` (`cs_gameinterface.cpp:150`), **`ReportGCQueuedMatchStart`** (`:156`), `CServerGameClients::NetworkIDValidated` (`:163`), `CCSGameRules::ReportRoundEndStatsToGC` (`cs_gamerules.cpp:15085`, параметр `CMsgGCCStrike15_v2_MatchmakingServerRoundStats**`), клиентские куски `cdll_client_int.cpp`/`clientmode_shared.cpp`, `cstrike15_matchmaking_utils.cpp:32`, `cs_gamerules_survival.cpp:2564`, инвентарь и др.

**Сохранились остатки, доказывающие удалённый код**: (1) `static CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve CCSGameRules::sm_QueuedServerReservation` (`cs_gamerules.h:1223`, `.cpp:1673`) — **читается** (tournament event/stage name, `pre_match_data`, best-of-N; `cs_gamerules.cpp:4659-5020`), но **ни одной записи** в дереве (только `#if 0`-тестовые `mutable_*`); (2) `CCSGameRules::m_pQueuedMatchmakingReportedRoundStats` (`CMsgGCCStrike15_v2_MatchmakingServerRoundStats*`) — инициализация/`delete`; (3) `m_pQueuedMatchmakingReservationString` — копия `sv_mmqueue_reservation`; (4) в `cs_gameinterface.cpp` сохранён ТОЛЬКО job `ClientJob_EMsgGCCStrike15_v2_GC2ServerReservationUpdate` (9142, `engine->UpdateHltvExternalViewers`) и `Helper_FillServerReservationStateAndPlayers`. **Единственный вызывающий `IVEngineServer::ReserveServerForQueuedGame` в обоих деревьях отсутствует** (только объявление в `eiface.h:525`, реализация `vengineserver_impl.cpp:1609`) — вызывавший GC-job удалён. `gcsdk` в деревьях — только proto-includes (`steammessages.proto`), клиента/сервера GCSDK нет; `base_gcmessages.proto`: `//k_EMsgGCServerWelcome = 4005; // GC => server`, `//k_EMsgGCServerHello = 4007; // server => GC`.

**Retail `server.dll` (Windows) — что реально есть**:
- `sm_QueuedServerReservation` = `unk_10AE6AC8`: **0 записей, 47 чтений, 2 address-of (ctor/dtor)** ⇒ **код, сохраняющий GC-резервацию, отсутствует** (CONFIRMED xref-скан). Job для 9105 отсутствует (RTTI-перечень §39.6).
- Реализован **`UpdateGCInformation`** (slot 46, `sub_103E7AE0`; в source удалён): шлёт `MatchmakingServerReservationResponse` (9106) — **status/advert**, а не completion: перепосылка раз в `RandomInt(480,720)` c или при смене карты/флагов/версии/адреса; поля (`Helper_Fill…`, `sub_103E7890`): `server_version`, `map`, `tv_info`, `reward_player_accounts` (игроки на CT/T) / `idle_player_accounts`; **`reservation`/stage не заполняются**. Engine вызывает `UpdateGCInformation` из: `SvMmQueueReservationChanged` (смена `sv_mmqueue_reservation`, `baseserver.cpp:239`), `baseserver.cpp:3267`, `sv_main.cpp:2896` (SV_ActivateServer), `sv_steamauth.cpp:1169`, `hltvclientstate.cpp:599`.
- `CGCClientJobServerWelcome` (`sub_1059BEB0`) — единственный вызов slot 149 `ReserveServerForQueuedGame` со строкой **`"R%p"`** (от `gscookieid`, поле 18 `CMsgCStrike15Welcome`), затем `serverGameDLL->UpdateGCInformation()`.
- `Server2GCClientValidate` (9153): отправитель `sub_103E7CE0` — vtable-метод (data `0x1085c73c`), к Accept отношения не имеет.
- `CProtoBufMsg<MatchmakingServerRoundStats>` / `<MatchEnd>` / `<Server2GCKick>` в `server.dll` **нет** ⇒ эти server→GC сообщения **не отправляются** (классы protobuf присутствуют только как сгенерированный код).

### 40.5 Server→GC / GC→server сообщения по proto-комментариям (ранний source-дамп `cstrike15_gcmessages.proto`)

| ID | Сообщение | Направление / назначение (комментарий Valve) | Есть отправитель в retail |
|---|---|---|---|
| 9105 | `GC2ServerReserve` | **«GC reports to server matchmaking reservation»** (GC→server). Поля после `pre_match_data`-блока: *«next section … doesn't have to be filled out by the server.dll when communicating with GC»* ⇒ **то же сообщение сервер вкладывает как «Current matchmaking reservation of the server»** в 9106/9108/9113 (только `account_ids`, `game_type`, `match_id`, `server_version`) | приёма нет |
| 9106 | `ServerReservationResponse` | **«Server reports its state to GC»** (`reservationid`, вложенная `reservation`, `map`, `gc_reservation_sent`, …) | **да** (`UpdateGCInformation`, без reservation) |
| 9107 | `GC2ClientReserve` | **«GC reports to clients matchmaking reservation»**; тем же сообщением GC может просить другой сервер стать GOTV-relay (сервер отвечает 9106) | клиентский job есть (§39.2) |
| 9108 | `ServerRoundStats` | **«Server reports its round stats to GC»**; поля **`confirm`** (`GC2ServerConfirm`: «Confirmation to deliver») и **`reservation_stage`** (**«1 = connection probing, 2 = ready-up»**) | нет (`ReportRoundEndStatsToGC` удалён) |
| 9111 | `ServerMatchEnd` | «Server reports its match end stats to GC» (`stats`, `confirm`, `rematch`, `aborted_match`, …) | нет |
| 9113 | `Server2GCKick` | «Server notifies the GC that a client has been kicked» (+ `reservation`) | нет |
| 9114 | `GC2ServerConfirm` | подтверждение доставки для 9108/9111 (`token, stamp, exchange, retry`) | нет |
| 9116 | `GC2ServerRankUpdate` | «GC reports players rank updates to the game server» | нет |
| 9118 | `ServerNotificationForUserPenalty` | GC→server | job есть (только приём) |
| 9142 | `GC2ServerReservationUpdate` | GC→server, зрители (Twitch) | job есть |

**Вывод по `GC2ServerReserve` (п.4 задания)**: (а) **создаёт его GC** (9105 GC→server) как начальную резервацию — *initial reservation*, до Accept; (б) **принимал** его удалённый server-job (в retail отсутствует); (в) это **не completion-signal**: как вложенное в 9106/9108/9113/9107 сообщение он лишь описывает «текущую резервацию» (accounts/game_type/match_id); (г) в клиентском 9107 вложенное `reservation` используется клиентом только ради `game_type&0xF` (§38.5); (д) с `ReportGCQueuedMatchStart` напрямую не связан, кроме того, что **его `account_ids` — тот же набор аккаунтов, чьи токены `[%x]` формируют roster**, на который ссылается `accounts[]`. **Сообщением, в которое server.dll должен был упаковывать `iReservationStage`, по proto-схеме является `MatchmakingServerRoundStats` (`reservation_stage`, `reservation{account_ids…}`, `confirm`)** — **HYPOTHESIS**: единственное поле во всей схеме с точной семантикой «stage резервации (1=probing, 2=ready-up)», ни одного C++-потребителя в source/binary нет (grep `reservation_stage` только в `.proto`/`.pb.h`).

### 40.6 Server GC lifecycle (п.5)

**Подтверждено retail-бинарником + source**: `srcds` (`server.dll` как GC-клиент через GCSDK) → `CMsgServerHello` (4007) → GC `ServerWelcome` (4005; `CMsgClientWelcome`+`CMsgCStrike15Welcome{gscookieid,…}`) → `CGCClientJobServerWelcome`: проверка версии (`"Version out of date (GC wants %d, we are %d)!"`), лог `"GC Connection established for server version %d, instance idx %d"`, при наличии `gscookieid`: `ReserveServerForQueuedGame("R…")`, затем `UpdateGCInformation` (9106). Далее периодически/по изменениям — 9106.
**Оригинальная (удалённая) часть**: GC → `9105 GC2ServerReserve` (server-job: сохранить в `sm_QueuedServerReservation`, собрать payload `Q<cookie>,<matchid>,<n>:[acct]…{caster}` → `engine->ReserveServerForQueuedGame`) → игроки: 9107 (GC→clients, «ready-up» reserve) → клиенты шлют UDP `0x21` stage 1/2 → engine на каждое повышение вызывает `ReportGCQueuedMatchStart(minStage,accounts,count)` → удалённый код формирует server→GC сообщение (**HYPOTHESIS**: `9108` с `reservation_stage`; **UNRESOLVED**) → GC-state → GC→clients **9107** (второй, не-accept `game_type`; §39). Звенья после `9105` в доступных бинарниках/исходниках **отсутствуют**; реконструирован только контракт по интерфейсам и proto.

### 40.7 project446 — сопоставление (п.6)

- **Аналог есть и он именно тот же вызов**: `sub_100BADE0` (вызывается из `sub_10094C50` на srcds) ставит vtable-хук `server.dll` `IServerGameDLL` (`ServerGameDLL005/001`): slot **47 → `sub_100BAD90`** (env `GC_VTBL_REPORT_MATCH_START` меняет номер слота), slot 4 `GameFrame` → `sub_100BAD70`, slot **41 → `IsGCSendAvailable`** (p446 в логах называет slot 41 «IsValveDS», но по раскладке 40.2 это **`ShouldAllowDirectConnect`** — их именование неточно). Строки p446: `"ReportGCQueuedMatchStart hooked at VTable[%d]… (ready-up accepts now come from the engine)"` vs `"…hook disabled by env — ready-up accepts stay a guess from MatchmakingStop"` — то есть **p446 сначала «угадывал» Accept по 9102, затем перешёл на engine-репорт**.
- **Caller/параметры/downstream**: хук `sub_100BAD90(this, minStage, accounts*, count)` (thiscall): вызывает оригинал (retail = 0) и при `accounts!=null ∧ 1≤count≤64` вызывает **`sub_100992A0`** — пакует `[minStage u8][count u8][account u32 × count]` и отправляет **своим транспортом** `sub_10115780(10024, payload)` через WS-сессию srcds→«C# backend» (`dword_1074AB84+264`, флаг подключения `+424`). Возвращаемое значение оригинала не подменяется (spoof не включается).
- **Собственный транспорт p446**: srcds-сторона использует нумерацию **10000-10025** поверх WS (10000/10001 match-report/disconnect, **10010 RoundStats bridge** (`"[GC] RoundStats bridge WS10010: match_id round players mvp"`), **10024 report queued-match-start**, 10008 `CanJoin`, 10025 quest…) — то есть p446 **не восстанавливал GC-job, а построил отдельный bridge srcds→backend**; клиентская сторона — ID 9101/9102/9103/9109/… (GC-протокол) + 1000-/2500-/4006-/9700-серии. Для Accept на стороне client-DLL — 12-байтные «ReadyUp counter»-события `(resId u64, count u32)` от backend (`sub_100F5D00 case 3`, §39.7).
- **Откуда p446 знает «все приняли»**: (1) engine-репорт `10024` → backend; (2) `9102` Accept-hook (`sub_100787F0`, запасной «guess»); backend формирует counter-события и «Connect reserve» (`MatchFoundInfo.game_type==0`, §39.7). Аналога `sm_QueuedServerReservation`/9105-job у p446 нет — reservation строит их GC-DLL (§28.5).

### 40.8 Что ещё показал engine: «completion» уже доходит до клиента без GC

`0x25` при `awaiting==0` (source `:2288-2307`) **рассылается всем участникам roster'а**, и на каждом клиенте engine `sub_1008A610` ставит `state=4` и вызывает `CServerConfirmedReservationCheckCallback::vftable[0]` (§38.3). В accept-режиме (`stage 2, mode 0`) `sub_103F7C00` затем возвращается без действий, потому что retail **ожидает 9107 от GC**. То есть информация, которую GC получал бы от сервера через `ReportGCQueuedMatchStart`, **параллельно и независимо** приходит клиенту UDP-путём; p446 использует ровно этот клиентский сигнал (`sub_100EA7B0`: перевооружение callback в `(mode 1, stage 2)` + fallback на 9107 `game_type=0`, §39.7).

### 40.9 Архитектурный вывод (п.7): A / B / C

- **A (existing GC connection, достаточно восстановить server job) — опровергнут в буквальной форме.** (i) «Server job», который надо восстановить, — не GC-job, а **тело метода `ReportGCQueuedMatchStart`** в `server.dll` (stub в Windows и Linux binary); GC-job'ов на стороне сервера для этого механизма нет ни в source, ни в binary. (ii) У нас `ServerGC`/`ClientGC` — **два изолированных in-process объекта** (`SharedGC`, `InstallGC(dedicated)`), в проекте нет никакого сетевого кода (только неиспользуемый `TestMM` UDP-responder) — даже работающий srcds→`ServerGC` сигнал не доходит до `ClientGC` (разные процессы/машины).
- **B (в retail server→GC completion path реально отсутствует из-за удалённого partner-depot кода → нужен отдельный bridge для собственного GC) — ПОДТВЕРЖДЁН** для оригинального пути: реализация удалена из source и заглушена в обоих retail-бинарниках; p446 пошёл по этому же пути (собственный WS-bridge srcds→backend, ID 10024).
- **C (другой существующий механизм) — частично найден**: не server→GC, а **client-local**: тот же факт «все приняли» существует на клиенте как `state=4` engine-запроса (`0x25` broadcast) и доходит до `vftable[0]` callback'а; retail-клиент его игнорирует только на `mode 0`. Возможность для нашего `ClientGC` (в процессе клиента) использовать этот локальный сигнал без srcds→ClientGC bridge следует из RE, но **не проверена**, требует hook/polling (сигнатура/vtable-hook `sub_103F7C00` или чтение `cb+128→state`) — **HIGH как возможность, HYPOTHESIS как реализация**.

Итог: выбор между «bridge (B)» и «client-local trigger (C′)» — **решение следующего этапа**, оба варианта следуют из RE; RE не даёт основания считать существующий GC-канал (A) достаточным.

### 40.10 Поправки к §37-§39 (старое не удалялось)

| # | Было | Стало |
|---|---|---|
| C1 | §39.6/C4: «engine вызывает `ReportGCQueuedMatchStart` при каждом `0x21`» | Только при **повышении stage** участника (`bThisClientJustConfirmed`), для `Q`; список `accounts[]` = `stage>=2 ∨ stage>min`. |
| C2 | §39.6: «`server.dll` — публичная сборка „Not Valve DS“ (строка `Not Valve ds…`)» | Строка — штатный код `ShouldAllowDirectConnect` (slot 41, есть в source). Признак public-сборки — slot 44 `IsValveDS ≡ 0` (`#define IsValveDedicated() false`, `public/const.h:472`); Linux `server.so` идентичен. |
| C3 | §37.8/§39: `ReportGCQueuedMatchStart` — «заглушка»; неясно, удалена ли | Тело **удалено в source** («Removed for partner depot») и **stub в обоих binary**; вместе с ним удалены GC-job 9105 и хранилище `sm_QueuedServerReservation` (0 записей в `server.dll`). |
| C4 | §39.6: slot 46 — «`CServerGameDLL` метод, шлёт 9106» | Это **`UpdateGCInformation`** (по порядку объявления); в source удалён, в retail реализован; вызывается engine из 5 мест (смена `sv_mmqueue_reservation`, activate, heartbeat-функции). |
| C5 | §39.7: p446 патчит slot 41 = «`IsValveDS`» | По раскладке — `ShouldAllowDirectConnect`; настоящий `IsValveDS` — slot 44. |
| C6 | §38.13/§39.10: канал «srcds→клиент» — единственный блокер | Есть второй источник того же сигнала на клиенте (40.8); блокер — выбор между bridge и client-local trigger. |

### 40.11 Не удалось установить

Что именно удалённый код отправлял в GC (сообщение/поля) при вызове; реакция GC; формат 9107 #2 в retail (§39); точное назначение `gc_reservation_sent`/`confirm` в этом контексте; содержимое p446-сообщения 10024 на стороне backend; нет ли у Valve-DS `server.so` (не публичного) иного тела.

### §40 STATUS

CONFIRMED:
- Источник контракта (source): `eiface.h`/`gameinterface.h`/`baseserver.cpp` — параметры `iReservationStage, puiConfirmedAccounts, numConfirmedAccounts`; единственный вызывающий — `CBaseServer::ReplyReservationCheckRequest`; условия вызова (только `'Q'`, только повышение stage участника, `serverGameDLL!=NULL`), состав `accounts[]` (`stage>=2 ∨ stage>min`, порядок roster'а, `uint32` AccountID), `minStage` = min по roster'у; `bReady`→spoof при `stage==2` и `awaiting>0`; «Reservation time extended»; broadcast `0x25` при `awaiting==0`. Совпадает с `engine.dll sub_101BC1D0`.
- Тело `ReportGCQueuedMatchStart` удалено в обоих source-деревьях («Removed for partner depot»); в retail Windows `server.dll` (slot 47 `0x10145510`) и Linux `server.so` (32-бит, `0xAF9BA0`) — `return 0`; `IsValveDS`(slot 44) ≡ 0.
- В retail `server.dll` нет GC-job'а 9105, нет отправителей `RoundStats`/`MatchEnd`/`Server2GCKick`; `sm_QueuedServerReservation` (`0x10AE6AC8`) — 0 записей; единственный `ReserveServerForQueuedGame` — `"R%p"` из `ServerWelcome`; `UpdateGCInformation` (slot 46) реализован и шлёт 9106 без reservation/stage.
- В обоих деревьях отсутствуют вызывающий `ReserveServerForQueuedGame`, клиент/сервер GCSDK и любая реализация server→GC для queued match; сохранился только `GC2ServerReservationUpdate`-job и `Helper_FillServerReservationStateAndPlayers`.
- Proto-комментарии: `9105` GC→server reservation, `9106` server→GC state, `9107` GC→clients reservation, `9108 RoundStats` с `reservation_stage` («1 = connection probing, 2 = ready-up») и `confirm`, `9114 GC2ServerConfirm`.
- project446: хук slot 47 `sub_100BAD90` → `sub_100992A0` → собственный WS-транспорт msg `10024` `[minStage][count][accounts]`; оригинал возвращает 0; p446 не восстанавливал GC-job.
- В проекте нет сетевого кода между `ServerGC` и `ClientGC`.
- `0x25 awaiting=0` рассылается всем участникам и доходит до client-callback (state 4).

HIGH CONFIDENCE:
- Удалённая реализация `ReportGCQueuedMatchStart`/9105-job/`ReportRoundEndStatsToGC` существовала во внутреннем дереве Valve (сохранившиеся остатки: `sm_QueuedServerReservation`, `m_pQueuedMatchmakingReportedRoundStats`, `m_pQueuedMatchmakingReservationString`, `reservation_stage`).
- Оригинальный GC получал стадию/список подтверждённых аккаунтов от сервера, а GC→clients 9107 (второй) формировался на стороне GC (внешний код).
- Вариант A в буквальной форме невозможен в нашем стеке; вариант B верен для retail-пути; client-local trigger (C′) возможен по данным RE.

HYPOTHESIS:
- Удалённый код отправлял `MatchmakingServerRoundStats` с `reservation_stage=iReservationStage`, `reservation.account_ids` и `confirm` (по proto-схеме единственное подходящее поле).
- `9114 GC2ServerConfirm` — подтверждение доставки для 9108/9111 (exchange/retry).
- Client-local trigger (hook/polling `sub_103F7C00`) реализуем без bridge.

UNRESOLVED:
- Точный формат/момент retail server→GC сообщения и реакция GC; точное содержимое второго 9107.
- Полное содержимое сообщения `10024` у p446 на backend; поведение Valve-DS-сборки (нет бинарника).
- Выбор архитектуры для нас (bridge vs client-local), account реального игрока на сервере, fake-stage 2, Danger Zone Q-roster, cookie lifecycle (§34).

### NEXT STEP AFTER §40

1. **Решение пользователя по архитектуре сигнала «все приняли»** — теперь на основании RE: (B) test-only bridge `ServerGC`(srcds, через vtable-хук slot 47 как p446 `sub_100BADE0/BAD90`)→`ClientGC` с payload `[minStage][count][accounts]` (контракт из 40.3), либо (C′) client-local trigger внутри клиента (наблюдение `state==4` при `stage 2, mode 0` на `CServerConfirmedReservationCheckCallback`, RVA §38.14). Вариант A отпадает.
2. Перед выбором (read-only): для C′ — определить надёжную точку наблюдения в `client.dll` (сигнатура `sub_103F7C00`/чтение `dword_152EAD0C→+128→state` и `+132/+140`), что p446 уже делает через `sub_100EA580` (сигнатура 0x33 байт), и поведение при повторных 0x25; для B — какой транспорт допустим (файл/UDP/loopback) и как передать account реального игрока.
3. После выбора — дизайн (не код) двухфазного 9107 и fake-0x21 (stage 1/2) по контракту 40.3; сначала лог-проверка существующего прототипа (без изменения логики).
4. Отложено: cookie lifecycle (§34), Danger Zone Q-roster, восстановление формата 9108/9114 (не требуется для нашего GC).

---

## 41. Compact Matchmaking Checkpoint — основной контекст для продолжения работы

**Это консолидация §34–§40; новых исследований нет.** При расхождении с более ранними разделами приоритет у этого раздела, а в нём — у выводов §38–§40 (они исправили часть гипотез §35–§37). Детали, RVA-таблицы и доказательства — в §38 (цепочка Accept→QueueConnect, RVA `client.dll`), §39 (второй 9107), §40 (`ReportGCQueuedMatchStart`). Обозначения: **[C]** подтверждено, **[H]** высокая уверенность, **[?]** не доказано.

### 41.1 Рабочая цепочка (retail-модель; `client.dll` — живой клиент, `engine.dll` жёстко грузит `bin\client.dll`)

```
1  Play → CLobbyMenuSingleton::vfn2 (client.dll sub_103F6240): game/mmqueue=registering; шлёт 9101 MatchmakingStart
2  GC → client: 9107 #1 (reservationid, адрес, map, [reservation{game_type}])  → handler sub_103F49F0: mmqueue=reserved,
       callback (stage 1, mode 0) если game_type&0xF ∈ {8,9,10,11,13}, иначе (stage 2, mode 2)
3  GC → server: reservation Q (или G) → IVEngineServer::ReserveServerForQueuedGame → roster на сервере (m_arrReservationPlayers)
4  client → server: UDP 0x21 stage=1 (engine-запрос, повтор ~1 c) → server ReplyReservationCheckRequest → 0x25 (awaiting,total);
       awaiting==0 ⇒ 0x25 рассылается ВСЕМ участникам → client state=4 (stage 1) → mmqueue=reserved, JS-попап, RaiseReadyUp(true,0,total)
5  Accept: JS LobbyAPI.SetLocalPlayerReady('accept') → (stage→2) → новый UDP 0x21 stage=2
6  server: 0x25 (awaiting,total); частичный → RaiseReadyUp(true,total−awaiting,total) (счётчик попапа)
7  0x25 awaiting=0 (все приняли) → client state=4 при (stage 2, mode 0) → callback НИЧЕГО НЕ ДЕЛАЕТ   ← точка остановки accept-режимов
8  GC → client: второй 9107 (не-accept game_type / без reservation) → destroy старого callback, новый (2,2) → 0x21 stage 2 → 0x25 state 4
9  callback (mode≠0): mmqueue=connect, KV "QueueConnect" сохраняется → per-frame poller (helper_time+2100 мс, нет loading plaque, та же сессия)
       → session->Command("QueueConnect") → matchmaking.dll → UpdateClientReservation + StartLoadingScreenForCommand("connect <adr>")
10 Cbuf_AddText → консольная `connect` → CBaseClientState::Connect (сервер сверяет reservation cookie) → загрузка карты
```
Для **не-accept режимов (Casual)** шаги 4-7 не нужны: 9107 #1 без `reservation` (⇒ `game_type=0` ⇒ callback (2,2)) сразу идёт по пути 8→10 — так работает наш прототип.

### 41.2 Что уже реально работает (live-проверено на Casual, клиент + отдельный `srcds` 192.168.1.150:27016) [C]

- `csgo_gc.dll` инжектирован в `csgo.exe` (`InstallGC(false)`) и `srcds.exe` (`InstallGC(true)`); общий compile-time cookie **`GameServerCookieId = 0x293A206F6C6C6548`** (клиент и сервер собраны из одной сборки).
- Клиент: `ClientGC::OnMatchmakingStart` (только eGame 7/Casual) шлёт 9107 (`reservationid`=cookie, адрес `matchmaking.test_server_address/port` из runtime `config.txt`, `map`, **без `reservation`**) → штатный client-side 9107 → QueueConnect → `connect`.
- Сервер: `ServerGC` после `k_EMsgGCServerHello` ставит reservation `G<cookie>,<cookie>,1:` — постановка через `PostToHost(HostEvent::ReserveServerForQueuedGame)` → выполнение на **главном потоке** из `Hk_SteamGameServer_RunCallbacks` (`steam_hook.cpp`), вызов `IVEngineServer` (`VEngineServer023`, **vtable slot 149**, offset 0x254, `bool(const char*)`).
- Fallback интерфейса `SteamGameServer014→013→012→011→010` (для v010 — свой `BLoggedOn` slot 2, в 011–014 slot 8).
- Reservation-check 0x21/0x25 против retail-engine srcds работает (при `G` сервер отвечает `awaiting=0`); первый матч Casual доходил до сервера и подключал клиента.
- **Известная проблема (не чинится в этом цикле)**: `ReserveServerForOurCookie()` вызывается один раз за жизнь `srcds` ⇒ второй матч даёт «Failed to connect to the match» (§34).
- **Не проверено live**: любой accept-режим (8/9/10/11/13), `Q`-reservation, попап Accept, второй 9107.

### 41.3 Ключевые подтверждённые функции

| Роль | RVA / имя |
|---|---|
| `IServerGameDLL::ReportGCQueuedMatchStart` (`CServerGameDLL` slot 47) | retail `server.dll` `0x10145510` = `return 0`; Linux `server.so` `0xAF9BA0` = `return 0`; vtable `server.dll` `0x1085cb10`; тело удалено и в source («Removed for partner depot») |
| `CBaseServer::ReplyReservationCheckRequest` | `engine.dll sub_101BC1D0` (source `engine/baseserver.cpp:2165-2308`); `SetReservationCookie` — `baseserver.cpp:4217-4296` |
| `ReserveServerForQueuedGame` | `IVEngineServer` slot 149 → `engine.dll sub_101B50F0` → `sub_101BFAA0` |
| Приём `0x25` / callback | engine `sub_1008A610` (`state=4` при `awaiting==0`); `CServerConfirmedReservationCheckCallback::vftable[0]`: **`client.dll sub_103F7C00`** (`client_panorama sub_103DF430`) |
| Callback ctor `(stage, mode)` | `client.dll sub_103F7980` (`client_panorama sub_103DF1C0`); поля `cb+128` handle, `cb+132` mode, `cb+140` stage; глобал активного cb `dword_152EAD0C` |
| **Accept handler (stage→2 + новый 0x21)** | `client.dll sub_103F7B60` (`client_panorama sub_103DF3A0`); вызывается из classifier `client.dll sub_105C3D90` (`client_panorama sub_10580C20`) при `reason != "deferred"` (`stricmp`-семантика: `0`=равно) |
| `LobbyAPI.SetLocalPlayerReady(reason)` | JS-мост `sub_10583500`→`sub_10584F70`→`sub_10585B00`→classifier (только локальный игрок); JS: кнопка Accept ⇒ `'accept'`, NQMM-автоприём ⇒ `'deferred'` |
| **9107 handler** | job `ClientJob_…MatchmakingGC2ClientReserve` (msgid 9107, factory `sub_103F4D30`) → **`client.dll sub_103F49F0`** (`client_panorama sub_103DC4B0`) |
| **QueueConnect builder** | внутри callback `sub_103F7C00` (единственный xref строки `"QueueConnect"`); сохранение KV `sub_103F7240` (`client_panorama sub_103DEA80`), глобал KV `dword_152EAD18` |
| Исполнение QueueConnect | per-frame poller `sub_103F7350` (из `CUiComponent_Lobby::Update` `sub_105C2330`) → `session->Command`; matchmaking.dll `sub_1001B290` |
| `RaiseReadyUp(bool,int,int)` | `client_panorama sub_1057FF90` (slot 0 lobby-подобъекта; событие `PanoramaComponent_Lobby_ReadyUpForMatch`, регистрация `client.dll 0x105C4900`); JS `PopupAcceptMatch.ReadyForMatch` |
| События попапа | `ServerReserved(map)` (`word_10D501B0`, raiser `sub_103E2DF0`/`client.dll sub_103FB5C0`) → `PartyMenu.ShowMatchAcceptPopUp`; `QueueConnectToServer(map,2.1)` (`sub_103DEA80`) → `LoadingScreen.Init` |
| 9104 handler | `client.dll sub_103F1C00` (`client_panorama sub_103D9900`): `matchmaking==4`→`connect`, `≠0,4`→`searching`, `3`→`RaiseReadyUp(false,0,0)`; connect не запускает |

### 41.4 Accept semantics [C]

- `game_type` в 9101/9107 = `(eGame & 0xF) | (eMapGroup << 8)`; клиент использует `& 0xF`.
- **Требуют Accept** (`{8,9,10,11,13}`): 8 Competitive, 9 Cooperative, 10 Wingman (`scrimcomp2v2`), 11 ScrimComp5v5, 13 Danger Zone (`survival`).
- **Не требуют**: 4 ArmsRace, 5 Demolition, 6 Deathmatch, **7 Casual**, 12 Skirmish. (project446 использует более узкий набор `{8,10,13}` — их выбор.)
- **stage 1** = «connection probing» (клиент достучался до сервера; при `awaiting==0` показывается Accept-попап); **stage 2** = «ready-up» (игрок нажал Accept). Stage хранится per-player на сервере; меняют его только входящие `0x21`.
- **`0x25 awaiting=0`** ⇒ **все** игроки roster'а прислали stage ≥ запрошенного (для stage 2 — полный Accept); сервер в этом случае рассылает `0x25` всем участникам (по сохранённым `adr/token`). `awaiting=127` = аккаунта нет в roster'е (status 4 не придёт). Клиентских `required_count/accepted_count` не существует; счётчик попапа = `total − awaiting` из ответов `0x25` (`awaiting`=byte0, `total`=byte1).
- **Критично**: callback для accept-режимов создаётся как `(stage 1, mode 0)`, и при `stage 2 ∧ mode 0` `sub_103F7C00` на `state==4` **ничего не делает**. `mode` пишет только ctor; connect возможен только после нового 9107 (или локальных reconnect-путей `(2,1)`).
- Наш текущий 9107 #1 **без `reservation`** ⇒ `game_type=0` ⇒ callback `(2,2)` ⇒ **Accept обходится** (для Casual это правильно). Для accept-режимов 9107 #1 **обязан** нести `reservation.game_type ∈ {8,9,10,11,13}`.

### 41.5 Server reservation [C]

- Payload (`ReserveServerForQueuedGame`): **`G<cookie_hex>,<matchid_hex>,<bReserve>:`** (без roster, `awaiting=0` всегда — Casual/in-progress) и **`Q<cookie_hex>,<matchid_hex>,<n>:[acct_hex][acct_hex]…`** (+ `{caster}` токены). Токены `[%x]` = `uint32` AccountID; нулевые токены пропускаются, но занимают позицию (draft index / team padding); порядок токенов = порядок roster'а.
- Roster `m_arrReservationPlayers` = `QueueMatchPlayer_t{m_uiAccountID, m_adr, m_uiToken, m_uiReservationStage}` строится в `SetReservationCookie` **только при смене cookie** (при том же cookie повторный вызов ничего не пересобирает).
- `0x21` (33 байта): `0xFFFFFFFF`, `0x21`, protocol const (13805), поля запроса, cookie, **SteamID клиента**; `0x25` (19 байт): `0xFFFFFFFF`, `0x25`, `hostVersion`, `token`, `stage`, `awaiting` u8, `total` u8. Условия обработки Q: cookie совпал, `stage≠0`, `steamid≠0`, `sv_mmqueue_reservation[0]=='Q'`; игрок ищется по `CSteamID(steamid).GetAccountID()` (нет в roster ⇒ `awaiting=127,total=0`).
- **`ReportGCQueuedMatchStart(minStage, accounts[], count)`**: engine вызывает **только при повышении stage участника** (0→1, 1→2) для `Q`; **`minStage`** = минимум stage по roster'у (0 — кто-то ещё не прислал stage 1; 1 — все «пробили»; 2 — все приняли); **`accounts[]`** = `uint32` AccountID игроков со `stage>=2 ∨ stage>minStage` в порядке roster'а; **`count`** = размер `accounts[]` (не roster'а). Финальный вызов при полном Accept: `minStage=2`, `count==размер roster'а`. Возврат `true` при `stage==2 ∧ awaiting>0` включает встроенный spoof (всем `stage=2`) — в retail он `false`. После завершения сервер продлевает резервацию (`sv_mmqueue_reservation_extended_timeout`=21 c).
- Retail-реализация отсутствует (source удалён, binary — stub); GC-job 9105 и хранилище GC-резервации в `server.dll` отсутствуют; сервер шлёт GC только status `9106` (без stage).
- С **одним реальным игроком** в `Q`-roster (`[real]`) логика сервера даёт `awaiting=0,total=1` на stage 1 и на stage 2 — то есть одиночный Accept возможен без fake players (выведено из source-логики; **[H]**, live не проверялось). Требуется account реального игрока в Q-payload на стороне `srcds` (источник — **[?]**).

### 41.6 Второй 9107 — только подтверждённое

- **Необходим** для перехода после полного Accept: клиентский код не имеет другого внутрисессионного пути (замыкание по job'ам: у `client.dll` ровно 5 GC→client matchmaking-job'ов — 9103, 9104, **9107**, 9110, 9112; 9104/9110 connect не запускают). Клиент **ничего не шлёт в GC при Accept** (только UDP `0x21 stage 2`).
- **Именно его handler `sub_103F49F0`** запускает reservation callback → QueueConnect: уничтожает accept-callback `(1,0)`, создаёт `(2,2)`, ctor сразу регистрирует 0x21 stage 2 → `0x25` state 4 → KV `QueueConnect`.
- Требования клиента к содержимому: `reservationid` (== cookie сервера, идёт в 0x21, KV и `UpdateClientReservation`) и **адрес** (`direct_udp_ip`+`direct_udp_port` или `server_address`) — **перечитываются из нового сообщения, fallback на сохранённые нет**; `reservation` отсутствует либо `game_type & 0xF ∉ {8,9,10,11,13}` (0 подходит); `map` желательно; `account_ids/party_ids/rankings` клиентом не используются; `game/mmqueue` в этот момент должен быть непустым (иначе клиент отвечает `MatchmakingStop`).
- **Точное содержимое и источник retail-9107 #2 неизвестны [?]**: GC-код (`ReportGCQueuedMatchStart`-реализация → GC → clients) отсутствует и в source, и в binary. project446 шлёт connect-вариант как 9107 с `reservation.game_type=0` (перед ним 9104 `matchmaking=4`), а также имеет native-путь «перевооружения» accept-callback в `(mode 1, stage 2)`; это **не** доказательство retail-поведения.
- Доступная точка client-local наблюдения сигнала «все приняли» (возможность, не проверено): `state==4` на активном callback при `stage 2 ∧ mode 0` (`client.dll sub_103F7C00`, `dword_152EAD0C`, handle `cb+128`).

### 41.7 Что НЕ исследовать сейчас

- Повторные reservation / cookie lifecycle (§34) — позже.
- Fake players (roster `[real][fake…]`, fake-`0x21`) — после рабочего single-player Accept.
- Danger Zone Q-roster / team-split — позже.
- Backend (Java/C#, admin panel) — позже.
- Inventory/skins — к текущей задаче не относится.
- Не менять: retail Accept matrix, `required_count`, bypass Accept, `QueueConnect`-логику; Git не трогать.

### 41.8 Unresolved (для справки)

Retail-формат и триггер 9107 #2 на стороне GC; источник сигнала «все приняли» у нас (bridge `ServerGC`→`ClientGC` vs client-local наблюдение `state==4`); источник account реального игрока на `srcds` для Q-payload; fake-stage 2; Danger Zone Q-roster; cookie lifecycle/повторная reservation; источник глобального списка аватаров попапа (`dword_10DBD140`); назначение `serverid` в 9107.

### 41.9 Текущая задача после checkpoint

**Реализовать минимальный trigger второго `MatchmakingGC2ClientReserve (9107)` после `0x25 awaiting=0`, используя уже существующий client-side 9107 → QueueConnect путь** (9107 #2: те же `reservationid`+адрес+`map`, без `reservation`/с не-accept `game_type`). Предусловия для проверки одним игроком: 9107 #1 с `reservation.game_type ∈ accept-set` (иначе Accept обходится), `Q`-reservation на `srcds` с account реального игрока. Выбор источника сигнала (`0x25 awaiting=0` на клиенте vs сигнал от сервера) — решение при постановке реализации; код/сборка/деплой в рамках этого checkpoint'а не выполнялись.


---

## 42. Accept flow MVP (TEST ONLY): fake roster + client-side trigger второго 9107 — реализация

**Статус: код написан и собран (`build_local.bat`, 0 warnings/errors), НЕ развёрнут в игру, live не проверялся.** Это первая реализация по §41.9; она не отменяет §34–§41. Обозначения: **[C]** подтверждено (source/binary), **[H]** высокая уверенность (вывод из source/RE, не live), **[?]** не проверено.

### 42.1 Новые подтверждённые факты (из source `engine/baseserver.cpp` и PE-анализа, без нового широкого RE)

- **[C] Третье поле payload `ReserveServerForQueuedGame` — `bReserve`, а не число игроков.** `sscanf(payload+1, "%llx,%llx,%d:", &cookie, &matchId, &bReserve)` (`baseserver.cpp:4187`). Поправка к §41.5 (там `<n>`): `Q<cookie>,<matchid>,1:[acct]…` — «1» = reserve; `…,0:` = `Unreserve()` (только если cookie совпал). Число игроков определяется числом `[%x]`-токенов.
- **[C] Резервация без клиентов сама не живёт долго (21 с) — и только так и сбрасывается.** `IsReserved()` = `m_nReservationCookie != 0` (`baseserver.h:265`); `sv_mmqueue_reservation_timeout` = 21 (`baseserver.cpp:242`, диапазон 5..180) задаёт только `m_flReservationExpiryTime = net_time + timeout` при каждом успешном `bReserve=1` (повторный вызов с тем же cookie разрешён: `!IsReserved() || cookie == GetReservationCookie()`, `baseserver.cpp:4195`). Cookie (и roster) сбрасывает `CGameServer::UpdateHibernationState` (`sv_main.cpp:~1975-2030`): `SetReservationCookie(0)`, когда сервер зарезервирован ∧ клиентов нет ∧ прошло `sv_hibernate_postgame_delay` с момента последнего ухода клиента ∧ `m_flReservationExpiryTime` истёк (или 0); также при `SetHibernating(true)` (`sv_main.cpp:1915`). Следствие **[H]**: известная проблема §34 («второй матч не подключается») согласуется с этим сбросом; для Accept-теста одноразовой резервации при старте srcds недостаточно — игрок должен успеть нажать Play и Accept.
- **[C] `CBaseServer::Unreserve()` (source `baseserver.cpp:4298`) только обнуляет `m_flReservationExpiryTime` и вызывает `UpdateHibernationState`; cookie/roster он НЕ очищает немедленно** (они уйдут через механизм выше). Поэтому «свежий roster» = `bReserve=0` + ожидание сброса cookie движком + новый `Q…`; немедленный повторный `Q` с тем же cookie roster (и stage'и участников) не пересоберёт.
- **[C] `m_arrReservationPlayers` пересобирается только при смене cookie** (`SetReservationCookie`): повторный `Q…` с тем же cookie roster/stage не трогает; сброс roster'а происходит только при `SetReservationCookie(0)` (см. выше).
- **[C] Семантика popup'а (поправка к формулировке flow из задачи):** `awaiting` в `0x25` = число игроков roster'а со `stage < stage запроса`. Поэтому **popup (state 4 при stage 1) появляется, когда ВСЕ участники прислали stage 1** (awaiting=0 на stage 1), а **ненулевой awaiting виден на stage 2** (пока кто-то не принял) и как счётчик `RaiseReadyUp(true, total−awaiting, total)`. Fake-участники поэтому должны «пробить» stage 1 раньше/вместе с реальным игроком, иначе popup не покажется.
- **[C] `bSpoofForcefulConnect` в `ReplyReservationCheckRequest` вступает в силу только со следующего `0x21`**: `uiAwaitingClients` присваивается ДО вызова `ReportGCQueuedMatchStart`, а spoof обнуляет только локальный `uiActualAwaitingClients`. Поэтому для fake-accept выбран путь «fake шлёт настоящие `0x21 stage 2`», а не hook `ReportGCQueuedMatchStart` (slot 47) с `return true`.
- **[C] `engine.dll` принимает UDP через `wsock32!recvfrom` (импорт по ordinal 17), а `wsock32!recvfrom` — не forwarder, а обёртка, вызывающая `ws2_32!WSARecvFrom` синхронно (без overlapped, 1 буфер)** (проверено разбором PE-импортов `engine.dll` и машинного кода `SysWOW64\wsock32.dll` RVA `0x15d0`). Хук `ws2_32!recvfrom` НЕ увидел бы клиентские `0x25`; надо хукать `WSARecvFrom` (`sendto` wsock32 — forwarder на `ws2_32!sendto`, `recv`/`recvfrom` — обёртки).
- **[C] Формат `0x21`** (проверено по `baseclientstate.cpp:370` и `ReplyReservationCheckRequest`): `FFFFFFFF | 0x21 | hostVersion u32 | token u32 | stage u32 | cookie u64 | steamid u64` (33 байта); `0x25`: `FFFFFFFF | 0x25 | hostVersion | token | stage | awaiting u8 | total u8` (19 байт).

### 42.2 Что реализовано (файлы: `csgo_gc/test_accept.h/.cpp` (новые), `gc_client.h/.cpp`, `gc_server.h/.cpp`, `gc_shared.h`, `config.h/.cpp`, `main.cpp`, `CMakeLists.txt`, `examples/config.txt`)

1. **Client, `ClientGC::OnMatchmakingStart`**: Casual (eGame 7) — без изменений (прямой 9107 без `reservation` + локальный `PostToHost` резервации). Accept-режимы **8 Competitive / 10 Wingman / 13 Danger Zone** (`AcceptTest::FindModeByGame`): первый 9107 = `reservationid`(cookie) + `direct_udp_ip/port` + `server_address` + `map` (`de_dust2` / `de_lake` / `dz_blacksite`) + **`reservation.game_type = request.game_type()`** (⇒ клиентский handler создаёт accept-callback `(stage 1, mode 0)` = штатный Accept-popup); локальная резервация с клиента НЕ шлётся (srcds резервирует себя сам). Режимы 9 и 11 пока игнорируются (лог).
2. **Client, детект «все приняли»**: `AcceptTest::InstallClientRecvHook()` (из `InstallGC(false)`, funchook на `ws2_32!WSARecvFrom`). Пока «armed» (после accept-9107 #1, таймаут 180 с, снимается на новый 9101/`MatchmakingStop`/после срабатывания), хук разбирает только дейтаграммы длиной 19 с `FFFFFFFF 25` от адреса резервации; условие **`stage == 2 ∧ awaiting == 0`** ⇒ `ClientGC::PostToGC(GCEvent::ReservationFullyAccepted)`. Это единственное место, где определяется `awaiting=0` на нашей стороне (само вычисление — движок srcds).
3. **Client, второй 9107** (`ClientGC::OnReservationFullyAccepted`): `serverid=1`, `direct_udp_ip/port`, `reservationid=GameServerCookieId`, `map`, `server_address` — те же значения, что в первом; **`reservation` не задаётся** (`game_type` читается как 0 ⇒ вне accept-набора) ⇒ существующий путь: новый callback `(2,2)` → `0x21 stage 2` → `0x25` → QueueConnect → `connect ip:port` (§38/§39).
4. **Server (srcds), `ServerGC::StartAcceptTestRoster`** при `matchmaking.test_accept_mode` ∈ {`competitive`,`wingman`,`dangerzone`} (иначе — прежняя `G`-резервация Casual, ничего не меняется): `Q<cookie>,<cookie>,1:[real][fake1]…` с roster 10 / 4 / 16 (1 реальный + 9 / 3 / 15 fake). Реальный AccountID — из `matchmaking.test_real_account_id` (srcds сам его узнать не может; клиент печатает его в `[MM-ACCEPT]` при 9101). Fake AccountID детерминированы: **`0xFA4E0000 + i`** (i=1..N−1; выше любого реального Steam account id).
5. **Fake players** (`AcceptTest::FakeRoster`, поток внутри srcds): один UDP-сокет, шлёт байт-точные `0x21` на `test_server_address:test_server_port` с fake SteamID (universe 1, type 1, instance 1). Фазы: **Probe** — каждую секунду stage 1 для всех fake (после первой задержки 1.5 с); при получении `0x25 stage 1 awaiting 0` (popup у клиента) → **Accept** — через `test_fake_accept_delay_ms` (2500) fake по очереди (шаг 150 мс) шлют stage 2. Итоговый `awaiting=0` наступает, когда stage 2 выставлен и у реального игрока (его `0x21 stage 2` после нажатия Accept) — при любом порядке; сервер сам шлёт `0x25` всем участникам (клиенту в том числе). Никаких GC-клиентов/игровых процессов у fake нет — только записи roster'а + UDP-пакеты.
6. **Keep-alive/повторный матч (только для Accept-roster)**: поток раз в 8 с повторяет `Q…` (обновляет 21-секундный таймаут; roster не пересобирается); при первом NetMessage от подключившегося клиента keep-alive и fake замолкают; когда отключается последний клиент (`ClientSOCacheUnsubscribe`) — `Unreserve`, пауза 12 с (пока движок сбросит cookie/roster) и новый `Q` (свежий roster, stage 0); та же перезарядка через 90 с, если после fake-accept никто не подключился.

### 42.3 Что НЕ доказано [?] / известные допущения

- **[?] Весь flow live не запускался** (сборка ≠ проверка). Особенно: (a) хук `WSARecvFrom` реально видит `0x25` в `csgo.exe` (обоснование — импорт по ordinal 17 + машинный код `wsock32!recvfrom`); (b) `hostVersion=13805` в fake-`0x21` совпадает с `GetHostVersion()` srcds (косвенно: реальный клиент с тем же значением получал `0x25` от srcds); (c) сервер принимает `0x21` с произвольным SteamID из loopback/LAN-адреса; (d) `total` в popup = размер roster'а.
- **[?] Retail-условия, при которых Valve-GC посылает 9107 #2, остаются неизвестными** (§41.6); реализованный trigger — TEST ONLY замена (вариант C′ §40.9, но через сетевой слой, а не через `client.dll sub_103F7C00`).
- **[H] Соответствие адреса `0x25`**: фильтр по `IP:port` источника = `test_server_address:test_server_port`; при multi-homed srcds ответ может прийти с другого IP.
- Один реальный игрок на srcds; несколько реальных игроков (party) — не поддерживается. Режимы Cooperative (9) и ScrimComp5v5 (11) не подключены. Cookie по-прежнему compile-time константа; fake/keep-alive не решают §34 для Casual `G`.

### 42.4 Что проверить вручную (live)

1. srcds `csgo_gc/config.txt`: `"test_accept_mode" "competitive"`, `"test_real_account_id" "<steamid64 & 0xffffffff>"`; клиент и srcds — одна и та же сборка DLL, тот же `test_server_address/port`.
2. Логи srcds: `[MM-ACCEPT] FakeRoster started`, `arming Q reservation`, `ReserveServerForQueuedGame result: 1`, `-> Reservation cookie …`; при Play — `Reservation from client …`, `server reports stage 1 awaiting=0 total=10`.
3. Клиент: Match Found/Accept popup (Competitive), счётчик принявших растёт после нажатия; логи `[MM-ACCEPT] 0x25 from reservation server: stage=2 awaiting=0`, `sending the second MatchmakingGC2ClientReserve`, затем QueueConnect → `connect` → загрузка карты.
4. Casual регрессия: без `test_accept_mode` Casual идёт по старому пути (9107 без `reservation`, `G`).

### §42 STATUS
Реализованы (собраны, не проверены live): Q-reservation с fake-roster, fake stage 1/2 через реальные `0x21`, client-side детект `0x25 stage 2 awaiting 0` (хук `WSARecvFrom`), второй 9107 → существующий QueueConnect-путь; keep-alive резервации на srcds для Accept-режимов. Новые CONFIRMED: `bReserve` (не число игроков), 21-секундный таймаут резервации, `wsock32!recvfrom → WSARecvFrom`, семантика `awaiting` (popup на stage 1 awaiting=0).

### NEXT STEP AFTER §42
Live-тест Competitive (1 real + 9 fake) по 42.4; по логам определить, на каком шаге цепочка обрывается (хук / fake `0x21` / popup / второй 9107 / connect). Затем Wingman/Danger Zone. Замена TEST ONLY триггера на backend-driven (сигнал сервер→GC) и cookie lifecycle (§34) — только после подтверждения.


---

## 43. Второе окно «Match Found» без Accept после успешного Accept-flow — диагностика

**Контекст:** Competitive (1 real + 9 fake) работает end-to-end (§42, live-подтверждено пользователем). После входа в матч появляется ещё одно уведомление «матч найден» без кнопки Accept. Задача: определить источник, ничего не чинить.

### 43.1 CONFIRMED — источник второго окна (по логам прогона + статический RE `client.dll`)

- **[C] Наш GC отправил ровно два 9107 за попытку** (`gc_log.txt`/`console.log`: одно `Sending MatchmakingGC2ClientReserve` при 9101 и одно `sending the second MatchmakingGC2ClientReserve (connect)` после `0x25 stage=2 awaiting=0`). Других matchmaking-сообщений GC→client (9104/9110/9112…) в этой попытке нет; третьего 9107 нет.
- **[C] `ServerReserved` (Panorama-событие «Match Found» попап) поднимается только из `CServerConfirmedReservationCheckCallback` (`client.dll sub_103F7C00`)** — у raiser `sub_103FB5C0` ровно два вызывающих места, оба внутри этого callback'а:
  1. `0x103F7D49` — ветка ready-up (`state==4`, `stage==1`, `mode==0`): `RaiseReadyUp` + звук `popup_accept_match_found` + `ServerReserved(map)` = **первое окно, с Accept**;
  2. `0x103F7E1C` — ветка queue-connect (`state==4`, `mode!=0`) при **`sub_103EF770(callback+92 = game_type) == false`** (game_type ∉ {8,9,10,11,13}): звук `popup_accept_match_found` + `ServerReserved("@<map>")` (формат `"@%s"`, `0x103F7E06`) и `helper_time = MSTime + 2000` = **NQMM-уведомление без Accept, автоматическое, connect откладывается на ~2 с**. Для accept-типов та же ветка играет только `popup_accept_match_confirmed` без попапа.
- **[C] Вывод:** второе окно — **прямое следствие нашего второго 9107**: он без `reservation` ⇒ `game_type=0` ⇒ 9107-handler (`sub_103F49F0`) создаёт callback `(stage 2, mode 2)` c `game_type=0` ⇒ `0x25 state 4` ⇒ не-accept ветка `0x103F7E1C` ⇒ попап `@de_dust2` (то самое «обычное уведомление classic-режима»). Ни отдельного третьего 9107, ни иного matchmaking-события для этого не требуется.

### 43.2 Что добавлено для проверки runtime (только логирование, поведение не менялось)

`csgo_gc/test_diag.h/.cpp`, ключ `matchmaking.test_diag` (по умолчанию 1, только клиент): строки `[MM-DIAG a<попытка> t=+<сек>]`:
GC-сторона — каждый отправленный 9107 (порядковый номер, наличие/`game_type` `reservation`, `reservationid`, `map`, `direct_udp`, `server_address`) и каждое matchmaking-сообщение, реально полученное игрой (`RetrieveMessage`); `client.dll` (inline-хуки, вызывают оригинал; RVA живого `client.dll`, проверка prologue): вход/выход 9107-handler (`game/mmqueue` + активный callback stage/mode/game_type), вход/выход callback (state запроса, `game/mmqueue`), создание callback (stage, mode), Accept, `ServerReserved`, `PlaySoundEffect`, `RaiseReadyUp`, `QueueConnectToServer`; сеть — клиентский `0x21` (stage) и любой принятый `0x25`.

### §43 STATUS
Источник второго окна установлен статически и по логам: callback `(2,2)` c `game_type=0` от нашего второго 9107 (не-accept ветка `0x103F7E1C`, `@map`). Runtime-подтверждение — логи `[MM-DIAG]` следующего прогона (DLL собрана, в игру не устанавливалась).

### NEXT STEP AFTER §43
Если runtime-логи подтвердят — решение по устранению (пользователь): второй 9107 должен приводить к connect без не-accept попапа (варианты обсуждать отдельно; не реализовано).

### 43.3 Финальный переход после Accept без второго popup — что определяет ветку и фикс (новые подтверждённые сведения)

**CONFIRMED (статический RE `client.dll`, без runtime; функции: 9107-handler `sub_103F49F0`, callback `sub_103F7C00`, ctor `sub_103F7980`, copy-in `sub_103F7A80`, msg→source `sub_103F83B0`):**
- **9107-handler всегда** уничтожает активный callback (`sub_103F7AF0` + delete, `0x103F4B8A`) и строит новый; тип выбирается по `(reservation ? reservation.game_type : default(0)) & 0xF`: ∈ {8,9,10,11,13} → ctor `(stage 1, mode 0)` (`0x103F4C09`), иначе → `(stage 2, mode 2)` (`0x103F4BEE`).
- **`callback+92` (game_type для `sub_103EF770` и `gametype/gamemode` в QueueConnect-KV) = `reservation.game_type & 0xF`, если у сообщения есть `reservation` (has-bit 0x10), иначе 0** (`sub_103F83B0` → source+84, копируется `sub_103F7A80` в callback+8+84=+92). Значение и выбор `(stage, mode)` происходят из ОДНОГО поля ⇒ **никаким 9107 нельзя получить callback «mode≠0 + accept game_type»**.
- **Вариант «9107 #2 с `reservation.game_type=8`» — неприемлем:** handler пересоздаст accept-callback `(stage 1, mode 0)`, отправит `0x21 stage 1`, сервер (все stage ≥ 1) ответит `awaiting=0` ⇒ ветка ready-up `0x103F7D49`: снова `RaiseReadyUp` + `popup_accept_match_found` + `ServerReserved`, т.е. второй Accept; путь в QueueConnect потребует ещё один 9107 (цикл).
- **Вариант «9107 #2 без `reservation`» (наш прежний):** callback `(2,2)` c `game_type=0` ⇒ на `0x25 state 4` ветка queue-connect: `sub_103EF770(0)=false` ⇒ `v29=1`, `popup_accept_match_found` + `ServerReserved("@map")` (`0x103F7E1C`) и `helper_time+2000`; в KV `gametype/gamemode="unknown"`.
- **Для `game_type ∈ accept` в той же ветке (mode≠0):** только `popup_accept_match_confirmed`, без `ServerReserved`, `helper_time+0`, KV `gametype=classic/gamemode=competitive`, `QueueConnect` сохраняется через `sub_103F7240(…, 0)`, затем (`LABEL_114`) `RaiseReadyUp(false,…)` закрывает ready-up попап — это штатный вид accept-mode connect-callback.
- Замечание: retail-путь, дающий accept game_type при `mode≠0` — локальный `(2,1)` из `sub_103F7400` (сохранённая резервация `dword_1520ED18`, «reconnect to ongoing match»), не через 9107; project446 «native re-arm (mode 1, stage 2)» — то же самое по смыслу.

**Реализованный фикс (минимальный, только клиент, Casual не затронут):** второй 9107 остаётся без `reservation`; сразу после того, как 9107-handler построил `(stage 2, mode 2, game_type 0)`, хук handler'а (`test_diag.cpp`, `Hk_Handler9107`) один раз выставляет `callback+92 = eGame` (8/10/13) — только при флаге, взведённом `ClientGC::OnReservationFullyAccepted` непосредственно перед отправкой 9107 #2 (`AcceptTest::SetFinalAcceptGameType`), и только если активный callback ровно `(2,2,0)`. Безопасность: `+92` читается лишь на `0x25 state 4` (в следующих кадрах), ctor его не использует. Handler-хук ставится всегда (проверка prologue), остальные диагностические — при `matchmaking.test_diag=1`; при недоступном хуке — прежнее поведение (лог `[MM-ACCEPT] client.dll 9107 hook not available…`). Лог успешного применения: `[MM-ACCEPT] final transition: callback (stage=2 mode=2) game_type 0 -> 8 …`.

**[?] Не проверено live:** отсутствие второго popup, загрузка карты после фикса, поведение KV `gametype/gamemode` (`classic/competitive`) на connect.


---

## 44. Аудит текущего состояния + первопричины Deathmatch / Danger Zone / Wingman; декодирование выбора карт (read-only, кода не менялось)

**Статус:** только чтение кода/логов/RE. Основание — `gc_log.txt`/`console.log` последнего прогона (клиент + srcds на 192.168.1.150:27016), исходники `csgo_gc`, `client.dll`/`engine.dll` (IDA), `csgo/gamemodes.txt`. Обозначения **[C]** подтверждено, **[H]** высокая уверенность, **[?]** не проверено.

### 44.1 Что реально работает сейчас (стабильный client-side MM flow) [C]
- Casual/non-accept: 9101 → 9107 без `reservation` → callback `(2,2)` → `0x21 stage 2` → `0x25` → QueueConnect → `connect` (srcds с `G`-резервацией).
- Accept (Competitive/Wingman/Danger Zone, live-подтверждено пользователем): 9107 #1 с `reservation.game_type=eGame` → callback `(1,0)` → `0x21 stage 1` → `0x25 awaiting=0` → штатный popup (`PlaySoundEffect('popup_accept_match_found')` + `RaiseReadyUp(true,0,total)`) → Accept → `0x21 stage 2` → `0x25 awaiting=0` → (хук `WSARecvFrom`) 9107 #2 без `reservation` + патч `callback+92=eGame` (§43.3) → QueueConnect → `connect`.
- Источник MM-запроса: 9101 приходит в `ClientGC::OnMatchmakingStart` (in-process, транспорта нет); решения принимает сам GC-DLL: режим (`FindModeByGame`: 8/10/13 — Accept, 7 — Casual), карта (жёстко `de_dust2`/`de_lake`/`dz_blacksite`), адрес сервера (`matchmaking.test_server_address/port`), размер roster (конфиг **srcds** `test_accept_mode`), реальный AccountID для srcds (конфиг `test_real_account_id`).

### 44.2 Deathmatch — причина [C]
Лог: `MatchmakingStart received: game_type=151977990 (eGame=6) … eGame=6 is neither Casual nor a supported Accept mode (8/10/13) -- ignoring`, через ~2.4 с клиент шлёт `MatchmakingStop` (9102). Причина — фильтр `if (eGame != 7 && !acceptMode) return;` в `ClientGC::OnMatchmakingStart` (`gc_client.cpp`): DM (и 4/5/12) отбрасываются, 9107 не отправляется, клиент ждёт и отменяет поиск. `game_type=0x090F0006` = `eGame 6 | mask 0x90F00<<8`, где `0x90F00 = mg_hostage` (см. 44.5) — выбор карт клиента до GC доходит, просто не используется.

### 44.3 Danger Zone «около 10 участников» и Wingman «10 вместо 4» — общая первопричина [C, трассировка по логу]
- `0x25.total` = `m_arrReservationPlayers.Count()` на srcds = **число `[acct]`-токенов в `Q`-payload, поставленной в момент старта srcds** (`baseserver.cpp` `SetReservationCookie`); клиент лишь показывает это (`RaiseReadyUp(true, total−awaiting, total)`), собственного «required» у него нет.
- Лог: `[MM-ACCEPT] arming Q reservation: Q293a…,1:[3e9846d5][fa4e0001]…[fa4e0009]` (**10** токенов = `competitive`); попытка DZ (`game_type=4877`): `0x25 … total=10`, `RaiseReadyUp(bool=1, 0, 10)`. При этом `config.txt` на диске уже содержал `test_accept_mode "dangerzone"` — **srcds не был перезапущен** (конфиг читается один раз при старте процесса: `GetConfig()` static; `FakeRoster` создаётся при `k_EMsgGCServerHello`).
- Т.е. размер roster — свойство **процесса srcds**, а не режима, выбранного клиентом: game_type клиента до srcds не доходит (нет транспорта GC↔GC). Код построения roster для DZ корректен по чтению: `{"dangerzone",13,16,"dz_blacksite"}` → `BuildQueuedReservationPayload` даёт 1 real + 15 fake = 16 токенов; **live-прогона DZ с 16 не было**. Wingman: та же механика (roster 10, если srcds стартовал как `competitive`); отдельного лога Wingman в текущем прогоне нет — вывод по коду и по идентичной трассе DZ.
- Следствие: Casual против srcds в `Q`-режиме тоже ведёт себя как Accept (`total=10`, awaiting=9; в логе a2 сработали ready-up/`variant=1`/`variant=2`), т.е. один srcds = одна модель резервации; для разных режимов нужны разные srcds (в будущем — реестр серверов с режимом в Java).

### 44.4 Неполный Accept у Danger Zone (`awaiting=4` навсегда) — [H] rate limit, вызванный нашим fake-драйвером
- Наблюдение: `0x25 stage=2 awaiting=4 total=10` неизменно ~20 с (`RaiseReadyUp(1,6,10)`), затем `state=3` (таймаут) — 4 записи roster не дошли до stage 2.
- **[C]** retail `engine.dll` (`sub_10013450…`, ConVar-строки `sv_max_queries_sec`="10.0", `sv_max_queries_window`="30", `sv_max_queries_sec_global`="500") + source `sv_ipratelimit.cpp:CIPRateLimit::CheckIP`: **все connectionless-пакеты с одного IP** (в т.ч. `0x21`) отбрасываются, когда `count/30 > 10` (>300 пакетов за окно 30 с; счётчик растёт и на отброшенных). Клиент и наши fake-игроки шлют с ОДНОГО IP (192.168.1.150).
- **[C]** Наш драйвер в фазе Probe шлёт `0x21 stage 1` **каждую секунду всем fake** (9 pps для Competitive, 15 pps для DZ, 3 для Wingman) — пока игрок не нажал Play; за 30 с это 270…450 пакетов ⇒ Competitive у границы, DZ гарантированно превышает лимит ⇒ часть пакетов (в т.ч. финальные stage 2) теряется молча; у Accept-фазы повторов нет (каждый fake шлёт stage 2 один раз), roster остаётся с `stage<2` у нескольких записей.
- **[H]** Это же объясняет «неполный набор» у DZ. Подтверждение — строка `IP rate limiting client …` в консоли srcds (сообщения `Msg`) или счётчики acks в драйвере (пока нет).
- Побочная находка **[H]**: после a2 fake-драйвер остался в фазе `Done`, поэтому в a3 повторного Probe/Accept не было — roster нельзя «переиспользовать» между попытками без пересборки (§34/§42.1 lifecycle, вне scope).

### 44.5 Как клиент передаёт выбранные карты [C] (client.dll `sub_10288A90`, вызывается из `sub_103F1240` ← `CLobbyMenuSingleton::vfn2 sub_103F6240` @ `0x103F6437`)
- Отдельного поля карт в 9101 нет (`account_ids, game_type, ticket_data, client_version, tournament_match, prime_only, tv_control, lobby_id`). Карты закодированы в **`game_type = (eGame & 0xF) | (mapMask << 8)`**, `mapMask` — 24-битная маска, строится из KV `game/mapgroupname` (список токенов `mg_*` через запятую, поиск `strstr` с проверкой границы токена).
- Таблицы масок **зависят от eGame; общие для групп режимов** (`switch`: `case 6/7`, `case 8/0xB`):
  - **6 DM / 7 Casual (общая):** `mg_dust247 0x2 (=de_dust2)`, `mg_casualdelta 0x1010B4`, `mg_casualsigma 0x640048`, `mg_hostage 0x90F00`; одиночные: `de_dust2 0x2`, `de_mirage 0x80`, `de_inferno 0x10`, `de_cache 0x1000`, `de_cbble 0x200000`, `de_vertigo 0x40`, `de_train 0x4`, `de_overpass 0x100000`, `de_nuke 0x20`, `de_canals 0x400000`, `cs_agency 0x80000`, `cs_office 0x100`, `cs_italy 0x200`, `cs_assault 0x400`, `cs_militia 0x800`, `de_ancient 0x8`, `de_basalt 0x40000`, `cs_insertion2 0x10000`, `de_grind 0x2000`, `de_mocha 0x4000`.
  - **8 Competitive / 11 ScrimComp5v5 (общая):** те же одиночные карты + флаг `mg_lobby_mapveto 0x20000`.
  - **10 Wingman:** `de_vertigo 0x40, de_inferno 0x10, de_cbble 0x200000, de_overpass 0x100000, de_train 0x4, de_shortnuke 0x20, de_shortdust 0x8000, gd_rialto 0x80, de_lake 0x8, de_guard 0x10000, de_elysion 0x40000, de_calavera 0x100, de_pitstop 0x200, de_ravine 0x400, de_extraction 0x800`.
  - **13 Danger Zone:** `dz_blacksite 0x1, dz_sirocco 0x2, dz_county 0x10`.
  - **4 ArmsRace:** `ar_shoots 1, ar_baggage 2, ar_monastery 4, de_lake 8, de_safehouse 0x20, ar_lunacy 0x80, de_stmarc 0x2000` (пусто ⇒ 8367); **5 Demolition:** `de_bank 1, de_sugarcane 4, de_lake 8, de_safehouse 0x20, de_stmarc 0x2000, de_shortdust 0x8000` (пусто ⇒ 41005); **9 Cooperative:** маска = quest id (`game/questid`); **12 Skirmish:** отдельная ветка, в таблице не разобрана [?].
  - Пустое/неизвестное — клиент пишет `Cannot queue for match on unknown mapgroup` и не ставит в очередь.
- Проверка по логу: `519 = 7|0x2<<8` (Casual `mg_dust247`), `520` (Competitive de_dust2), `4877 = 13|0x13<<8` (DZ: blacksite+sirocco+county = 0x1|0x2|0x10), **`151977990 = 6|0x90F00<<8` (DM, `mg_hostage`)**. Декомпилят функции сохранён в scratchpad (`q45_gametype_composer.txt`).
- `maxplayers` из `csgo/gamemodes.txt` (параметр srcds, НЕ required players матчмейкинга): casual 20, competitive 10, scrimcomp2v2 (Wingman) 4, scrimcomp5v5 10, gungameprogressive (ArmsRace) 10, gungametrbomb (Demolition) 10, deathmatch 16, cooperative 20, coopmission 10, skirmish 12, survival (DZ) 16. `mapgroupsMP` DM = группы casual (`mg_casualsigma/delta/dust247/hostage`).

### 44.6 Источник SteamID/AccountID [C]
- `ISteamUser::GetSteamID()` реального Steam-клиента: `GetUserSteamId(pipe,user)` (`steam_hook.cpp:156`, `s_actualSteamClient->GetISteamUser`) вызывается при создании прокси `ISteamGameCoordinator` (клиент) и передаётся в `ClientGC(steamId)`; `AccountId() = steamId & 0xFFFFFFFF` (в логе: `ClientGC spawned for user 76561199010432725`, account 1050166997 = `0x3E9846D5`).
- 9101 дополнительно несёт `account_ids[]` (solo — локальный AccountID; клиент собирает из KV `members/machine%d/player0/xuid`; для party — все члены). Доверять следует значению из `ISteamUser`, а не из сообщения.
- На srcds AccountID реального игрока автоматически доступен из `0x21` (последние 8 байт — SteamID) и из auth-ticket при подключении; вручную `test_real_account_id` не нужен, когда Java backend передаёт roster по AccountID, полученному от GC клиента.

### 44.7 Замечания по диагностике (test_diag) [C]
- `game/mmqueue=(exception)` во всех логах — чтение `game/mmqueue` из хука падает в `__except`; на работу flow не влияет, исправить/убрать при следующей правке кода.
- Хук `ServerReserved raiser sub_103FB5C0` не установился («unexpected prologue»): 12 байт из IDB содержат абсолютные операнды (`dword_152F36D4`, `word_10DBCEB8`), которые меняются при релокации `client.dll`; для проверки нужен префикс без адресных операндов. Остальные хуки (в т.ч. handler 9107 и патч `callback+92`) установлены.

### 44.8 Инфраструктура для Java-этапа [C]
- В репозитории **нет** HTTP/JSON-транспорта «старого BETA» (`csgo_gc/`, `launcher/`, `vcpkg.json`: только `mbedtls`, `protobuf`); §11 помечает старый BETA HTTP+JSON backend как «не основная архитектура». Транспорт GC↔Java придётся писать (WinHTTP/winsock без новых зависимостей, либо TCP/WebSocket).
- **JDK/Maven/Gradle на машине не найдены** (`java`, `mvn`, `gradle` отсутствуют в PATH) — для Java-этапа нужен JDK 17+ (установка потребует загрузки; согласовать).

### §44 STATUS
Первопричины найдены: DM — фильтр `eGame` в `OnMatchmakingStart`; DZ/Wingman `total=10` — размер roster задаётся конфигом srcds при старте процесса (не режимом клиента); неполный Accept DZ — вероятно, per-IP rate limit движка (10 pps/30 с) из-за Probe-флуда fake-драйвера. Выбор карт декодируется из `game_type>>8` по таблицам `sub_10288A90`.

### NEXT STEP AFTER §44 (по порядку пользователя)
2) DM: обобщить `OnMatchmakingStart` на non-accept режимы {4,5,6,7,12} с картой из декодированной маски; централизованная таблица режимов. 3–4) Fake-драйвер без флуда (ack-based, несколько pps), диагностика `total ≠ ожидаемого`; проверка DZ=16 и Wingman=4 на отдельных srcds (по режиму). 5) Логирование декодированных карт. Затем — контракт GC↔Java.


---

## 45. Центральная таблица режимов, Deathmatch (non-accept режимы), fake-driver без флуда, отдельные srcds по режимам — реализация (TEST ONLY слой GC)

**Статус:** код написан и собран (`build_local.bat`: 0 errors, 0 warnings в нашем коде), **в игру не устанавливался, live не проверялся**. Проверено офлайн: симуляция srcds-логики (`ReplyReservationCheckRequest` + per-IP rate limiter) с реальным `FakeRoster` и таблицей режимов на реальных `game_type` из логов. Не отменяет §34–§44. **[C]** подтверждено, **[H]** высокая уверенность, **[?]** не проверено live.

### 45.1 Что изменено
- **`csgo_gc/mm_modes.h/.cpp` (новые) — центральная таблица режимов** `MM::GameMode {gameType, name, acceptRequired, requiredPlayers, serverGameType, serverGameMode, serverMaxPlayers, mapTable, defaultMap, supported}` + `DecodeMapSelection(mode, game_type)` (mapMask из `game_type>>8`, таблицы бит→карта из `client.dll sub_10288A90`, §44.5), `PickMap` (первая выбранная карта в порядке таблицы, иначе `defaultMap`). Значения: ArmsRace(4) gungame/gungameprogressive, Demolition(5) gungame/gungametrbomb, **Deathmatch(6) gungame/deathmatch**, Casual(7) classic/casual — accept=false; **Competitive(8) required 10**, **Wingman(10) required 4**, ScrimComp5v5(11) required 10 [?], **DangerZone(13) required 16** — accept=true; Cooperative(9) accept=true, required **0 (не подтверждено), `supported=false`**; Skirmish(12) accept=false, маска = варианты skirmish (не карты), карта — `defaultMap`. `serverMaxPlayers` (gamemodes.txt: 20/10/16/20/10/4/10/12/16…) — справочное, не используется как required players.
- **`ClientGC::OnMatchmakingStart` (gc_client.cpp):** условие `if (eGame != 7 && !acceptMode) return;` удалено. Любой `supported` режим: лог `[MM] mode=… accept_required=… required_players=… server=type/mode | maps: mask=… <карты> -> advertised map=… | account=… (9101 account_ids[0]=…)`; **non-accept режимы (Casual, Deathmatch, ArmsRace, Demolition, Skirmish) идут прежним Casual-путём** (9107 без `reservation`, G-резервация); accept-режимы — прежним Accept-путём (9107 #1 с `reservation.game_type`, хук `0x25`, 9107 #2 + патч `callback+92`). Карта в 9107 — из выбранной пользователем маски, а не из конфига. Порт srcds — `TestServerPortForMode(mode)` (`matchmaking.test_server_ports { "<mode>" "<port>" }`, иначе `test_server_port`).
- **Fake-driver (`test_accept.cpp`, `AcceptTest::FakeRoster`) переписан:** каждый fake шлёт **ОДИН** `0x21 stage 1` (пейсинг ≥150 мс между пакетами, ≤6.7 pps), повтор только если за 1.5 с не пришёл `0x25` с его token (максимум 5 попыток, затем `TIMEOUT` в логе и перезапуск roster через 30 с); подтверждение = `0x25` с `token==fake`, `stage==запрошенный`, `awaiting!=127`; когда сервер сообщает `stage 1 / awaiting 0` (popup у клиента) — через `test_fake_accept_delay_ms` каждый fake шлёт один `stage 2` (так же с подтверждением). Простой без игрока — 0 пакетов (раньше 9–15 pps постоянно). Лог `[FAKE-MM]`: `roster expected=N actual=N`, `player=fake-NN(acct=…) stage=S sent / confirmed (awaiting=…,total=…)`, `server roster total=T expected=N OK|MISMATCH`, `awaiting=0 total=N`. Несовпадение `total` с required players пишется явно (и на srcds, и на клиенте — `[MM-ACCEPT] server roster total=… <== MISMATCH`), не маскируется.
- **Реальный AccountID больше не в конфиге:** srcds ставит `WSARecvFrom`-хук (только при `test_accept_mode`/`-gc_mode`) и берёт AccountID из **первого `0x21` с нашим cookie от не-fake аккаунта** (SteamID клиент получил из `ISteamUser::GetSteamID()`), затем создаёт roster; повторы `0x21` клиента (раз в ~1 с) проходят после постановки резервации. `matchmaking.test_real_account_id` остаётся только необязательным override для TEST. [?] live: поведение клиента при ответе `awaiting=127` до постановки резервации (ожидается «продолжает повторы», как при `awaiting>0`).
- **Отдельный srcds на режим:** режим srcds — `-gc_mode <mode>` в командной строке (приоритетнее конфига) или `matchmaking.test_accept_mode`; собственный порт для fake — `-port` (`GCConfig::DedicatedServerPort`), иначе `test_server_port`. Пример: `srcds.exe -game csgo -console -port 27018 -gc_mode dangerzone +game_type 6 +game_mode 0 +map dz_blacksite` (competitive: `+game_type 0 +game_mode 1`; wingman: `+game_type 0 +game_mode 2`; DM: `+game_type 1 +game_mode 2`). Клиент выбирает порт по режиму из `test_server_ports`.
- Прочее: `Platform::CommandLine()`; `GCEvent::TestRealPlayerSeen`; в CMake `/wd4702` (предупреждение C4702 даёт сгенерированный `*.pb.cc` на заголовках vcpkg-protobuf при пересборке protobuf, не наш код); `examples/config.txt` обновлён.

### 45.2 Проверка (офлайн-симуляция, scratchpad `sim/`) [C для симуляции, не для игры]
Симулятор реализует `ReplyReservationCheckRequest` (roster из `Q`-payload, stage per-player, awaiting/total, broadcast при awaiting=0) и rate limiter (`count/30 > 10` ⇒ drop), реальный клиент — сценарий «`0x21 stage 1` раз в секунду → Accept → `stage 2`»; тестируется настоящий `FakeRoster`:
- **Danger Zone 1+15:** `roster expected=16 actual=16`, `server roster total=16 expected=16 OK`, popup при `stage 1 awaiting=0 total=16`, после Accept `awaiting=0 total=16`, все 16 записей на stage 2; **всего 32 пакета на сервер, 0 отброшено rate limit** (старый драйвер за то же время — сотни). Игрок, пришедший через 25 с — тот же результат (32 пакета, простой без трафика).
- **Wingman 1+3:** `total=4 expected=4 OK`, 8 пакетов, `awaiting=0 total=4`. **Competitive 1+9:** `total=10 OK`, 20 пакетов, `awaiting=0 total=10`. Порядок «Accept раньше fake» и «fake раньше Accept» оба дают `awaiting=0`.
- **Таблица режимов/карт на реальных `game_type` из логов:** `519`→casual `de_dust2`; `520`→competitive `de_dust2`; `4877`→dangerzone `dz_blacksite,dz_sirocco,dz_county`; **`151977990`→deathmatch `cs_agency,cs_office,cs_italy,cs_assault,cs_militia,cs_insertion2` (=mg_hostage)**; синтетические wingman/competitive+mapveto/armsrace/skirmish декодируются по §44.5.

### 45.3 Не проверено live [?]
DM/ArmsRace/Demolition/Skirmish на реальном srcds нужного режима; Competitive/Wingman/DZ после переписывания драйвера и автоопределения AccountID; поведение клиента при `awaiting=127` в начале; хук `WSARecvFrom` на srcds (если не сработает — задать `test_real_account_id`); ScrimComp5v5 (включён по аналогии с Competitive). Cooperative (9) не поддерживается (число игроков не подтверждено).

### §45 STATUS / NEXT STEP
Реализовано и собрано, live-проверка — после установки DLL: отдельные srcds (`-gc_mode`, `-port`) для competitive/wingman/dangerzone + обычные для casual/deathmatch, `test_server_ports` в клиентском config. Дальше (по порядку): подтверждение live → контракт GC↔Java (предложение по JDK/build/framework/структуре/модели/API/панели до реализации).


---

## 46. CHECKPOINT ПЕРЕД JAVA MATCHMAKING BACKEND — полное состояние проекта (Accept flow, режимы, fake roster, AccountID, reservation, конфиг, ограничения)

**Это главный актуальный checkpoint.** Консолидирует §34–§45 и live-логи от 2026-09-19 (`csgo/console.log`, `gc_log.txt`); в случае расхождения с более ранними разделами приоритет у этого раздела, а внутри него — у пунктов с меткой **[C live]**. Старые разделы не изменялись. В этом шаге исходный код, конфиги и Git НЕ менялись. Обозначения: **[C live]** подтверждено живым прогоном (реальный CS:GO 2021 client + srcds), **[C]** подтверждено кодом/RE/source, **[SIM]** проверено только офлайн-симуляцией, **[H]** высокая уверенность, **[?]** не проверено, **[TODO]** решено, не сделано.

### 46.1 Состояние на сейчас
- Установленная в игре DLL = `Build\release\csgo_gc\csgo_gc.dll` от 2026-09-19 03:28 (sha256 `1facbb30…`, идентична билду; сборка 0 errors / 0 warnings в нашем коде). Предыдущие DLL сохранены рядом: `csgo_gc.dll.bak_pre_finalfix`, `csgo_gc.dll.bak_pre_accept`. Java backend НЕ начат (JDK на машине нет).
- Живой клиентский подбор **работает и протестирован** на реальном клиенте и отдельных srcds (192.168.1.150): Competitive (27016), Wingman (27017), Danger Zone (27018), обычные режимы; полный Accept-flow с настоящим штатным popup Panorama (без своего UI, без обхода Accept).

### 46.2 Рабочий pipeline (клиент ↔ GC-DLL ↔ srcds)
```
Panorama UI → client.dll формирует 9101 MatchmakingStart {account_ids, game_type=(eGame&0xF)|(mapMask<<8), client_version=13805, prime_only, …}
  → csgo_gc.dll ClientGC::OnMatchmakingStart (in-process, транспорта наружу нет):
       mode = MM::FindGameMode(eGame); карты = MM::DecodeMapSelection(mode, game_type); порт srcds = config test_server_ports[mode] (иначе test_server_port)
  ├─ БЕЗ Accept (casual/deathmatch/armsrace/demolition/skirmish):
  │    9107 MatchmakingGC2ClientReserve (reservationid=cookie, direct_udp_ip/port, server_address, map) БЕЗ reservation
  │    → client.dll 9107-handler создаёт callback (stage 2, mode 2) → 0x21 stage 2 → srcds отвечает 0x25 awaiting=0 (G-резервация)
  │    → state 4 → «@map» уведомление + QueueConnect (connect отложен +2 c) → `connect ip:port` → карта загружается
  └─ С Accept (competitive/wingman/dangerzone):
       9107 #1 С reservation{game_type=полный game_type} → callback (stage 1, mode 0) → 0x21 stage 1
       srcds (Q-резервация: 1 реальный + N-1 fake) → 0x25 (awaiting,total); все на stage ≥1 → 0x25 awaiting=0 → штатный Match Found/Accept popup (PlaySoundEffect + RaiseReadyUp(true,0,total))
       игрок жмёт Accept (JS SetLocalPlayerReady('accept')) → callback stage→2 → 0x21 stage 2; fake принимают; awaiting→0
       0x25 stage=2 awaiting=0 → хук ws2_32!WSARecvFrom в csgo.exe → GCEvent::ReservationFullyAccepted → 9107 #2 БЕЗ reservation
       + патч callback+92 (client.dll) = eGame → callback (2,2) с accept game_type → QueueConnect БЕЗ второго popup (только popup_accept_match_confirmed) → `connect` → карта
```
Ключевые клиентские адреса (`client.dll`, build 1352, sha1 `53ba71b7ed66a9445cb5eb7783b85c909fbddda9`, IDA-адреса при base 0x10000000): 9107-handler `sub_103F49F0`, reservation-callback `sub_103F7C00`, ctor `sub_103F7980`, Accept `sub_103F7B60`, `ServerReserved` raiser `sub_103FB5C0`, QueueConnect KV `sub_103F7240`, `RaiseReadyUp` `sub_105C2DB0`, accept-required `sub_103EF770` ({8,9,10,11,13}), game_type composer `sub_10288A90`; глобал активного callback `0x152EAD0C`; поля callback: `+128` handle, `+132` mode, `+140` stage, `+92` game_type, `+40` reservationid (подробнее §38.14, §43, §44.5).

**Что проверено вживую (2026-09-19, console.log) [C live]:**
- Competitive `mode=competitive … required_players=10`, `server roster total=10 matches`, `final transition … game_type 0 -> 8`, `Connected to 192.168.1.150:27016`, `Map: de_dust2`, `Players: 1 (2 bots) / 10 humans`.
- Wingman `required_players=4`, `total=4 matches`, `game_type 0 -> 10`, `Connected …:27017`, `Map: de_lake`, `Players: 1 (2 bots) / 4 humans`.
- **Danger Zone 1+15:** `required_players=16`, `total=16 matches`, `game_type 0 -> 13`, `Connected …:27018`, `Map: dz_blacksite`, `Players: 1 (0 bots) / 16 humans` (два успешных подключения). Прежний баг «DZ ≈10» — это была стейл-конфигурация (srcds стартовал как competitive), не ошибка roster (§44.3).
- Второе «Match Found без Accept» устранено (§43.3): после Accept нет `ServerReserved event`, идёт `final transition …`.
- **Автоопределение реального AccountID [C live, по косвенному признаку]:** первая попытка после старта srcds получает `0x25 awaiting=127 total=0` (roster ещё не поставлен), следующая — `awaiting=0 total=16` ⇒ srcds выучил AccountID по первому `0x21` через хук на srcds. Идентичность: лог `[MM] … account=1050166997 (9101 account_ids[0]=1050166997)` — значение из `ISteamUser::GetSteamID()` совпадает с 9101.
- Deathmatch: `game_type=518 (6|0x2<<8)` → `mode=deathmatch … advertised map=de_dust2`, 9107 отправлен, клиент подключился к серверу (на тот момент порт 27016). **Режим DM на самом srcds (game_type 1 / game_mode 2) live не подтверждён.**
- Casual/обычные режимы: рабочие (предыдущие прогоны и последний: 4-я попытка — `0x25 stage 2 awaiting=0 total=0` (`G`-резервация) → QueueConnect → подключение, `Map: de_dust2`). Первые 3 попытки того прогона дали `0x25 awaiting=127` → `state=3`: srcds ещё стартовал (`ServerGC spawned` в логе появился только через ~11 с после первой попытки), `G`-резервация ставится по `k_EMsgGCServerHello`, т.е. это тот же механизм «резервации ещё нет» (46.9 п.1), а не ошибка режима.

**Только симулировалось [SIM] (не live):** rate-limit-безопасность нового fake-драйвера (DZ: 32 пакета, 0 отброшено); декодирование карт для wingman/competitive+mapveto/armsrace/skirmish; стороны srcds-логи `[FAKE-MM]` в этом чекпоинте не проверялись (srcds-консоль недоступна из сессии).

### 46.3 Режимы — актуальный MVP и таблица (`csgo_gc/mm_modes.h/.cpp`, центральная таблица `MM::GameMode`)
**MVP с Accept:** Competitive (game_type 8) — 10; Wingman (10) — 4; Danger Zone (13) — 16.
**MVP без Accept:** Casual (7), Deathmatch (6), Arms Race (4), Demolition (5), Skirmish (12).
**НЕ в MVP:** **ScrimComp5v5 (11) — удалён из MVP**; **Cooperative (9) — не поддерживается** (число игроков не подтверждено, `supported=false`).
**[TODO, расхождение кода с решением]:** в `mm_modes.cpp` строка `scrimcomp5v5` всё ещё `supported=true` (и порт `scrimcomp5v5 27019` присутствует в `csgo_gc/config.txt` и `examples/config.txt`) — убрать при ближайшей правке кода/конфига; до этого режим 11 формально включён, но не тестировался и в MVP не входит.

| eGame | name | accept | required | srcds game_type/game_mode (client.dll QueueConnect KV switch, gamemodes.txt) | gamemodes.txt maxplayers | numeric `+game_type +game_mode` | статус |
|---|---|---|---|---|---|---|---|
| 4 | armsrace | нет | — | gungame / gungameprogressive | 10 | 1 / 0 [H] | MVP |
| 5 | demolition | нет | — | gungame / gungametrbomb | 10 | 1 / 1 [H] | MVP |
| 6 | deathmatch | нет | — | gungame / deathmatch | 16 | 1 / 2 [H] | MVP |
| 7 | casual | нет | — | classic / casual | 20 | 0 / 0 [C live] | MVP |
| 8 | competitive | **да** | **10** | classic / competitive | 10 | 0 / 1 [C live] | MVP |
| 10 | wingman | **да** | **4** | classic / scrimcomp2v2 | 4 | 0 / 2 [C live] | MVP |
| 13 | dangerzone | **да** | **16** | freeforall / survival | 16 | 6 / 0 [C live] | MVP |
| 12 | skirmish | нет | — | skirmish / skirmish | 12 | 5 / 0 [H] | MVP (карта не декодируется) |
| 11 | scrimcomp5v5 | да | 10 | classic / scrimcomp5v5 | 10 | 0 / 3 [H] | **вне MVP** [TODO убрать] |
| 9 | cooperative | да | не подтверждено | cooperative / cooperative | 20 | — | **не поддерживается** |
Клиент нативно требует Accept для {8,9,10,11,13} (client.dll `sub_103EF770`); наша реализация обслуживает {8,10,13}. Четыре разные величины не смешивать: `maxplayers` srcds (свойство сервера) ≠ `required_players` матчмейкинга ≠ размер reservation roster (= required_players) ≠ число принявших Accept (`total − awaiting` в `0x25`).

**Выбор карт [C]:** карты приходят внутри `game_type = (eGame&0xF) | (mapMask<<8)`, `mapMask` — 24-битная маска из KV `game/mapgroupname` (токены `mg_*` через запятую); таблицы бит→карта зависят от режима и общие для 6/7 и 8/11 (полная таблица — §44.5, реализация — `DecodeMapSelection`, проверена на реальных значениях `519, 520, 518, 4877, 151977990`). Competitive: флаг `0x20000` = `mg_lobby_mapveto` (не карта). Skirmish: маска = варианты режима (`1<<(index-1)`), Cooperative: маска = quest id — не карты [?]. В 9107 уходит первая выбранная карта в порядке таблицы (`PickMap`), реальную карту определяет запущенный srcds.

### 46.4 Fake reservation roster (TEST ONLY, srcds, `test_accept.cpp`, класс `AcceptTest::FakeRoster`)
- Размер roster = required players режима: **Competitive 10, Wingman 4, Danger Zone 16** (1 реальный + N−1 fake). Fake AccountID детерминированы: `0xFA4E0000 + i`, i=1..N−1 (выше любого реального Steam account id); SteamID fake = universe 1, type 1, instance 1. Fake — только записи roster + UDP-пакеты, не GC-клиенты и не игровые процессы.
- `Q`-payload: `Q<cookie_hex>,<matchid_hex>,1:[real][fake1]…` (третье поле — `bReserve`, не счётчик); `Unreserve` — `…,0:`.
- Фазы драйвера: `Probe` (каждый fake шлёт **ОДИН** `0x21 stage 1`, один пакет в **150 мс**, подтверждение = `0x25` с `token==fake`, `stage==запрошенный`, `awaiting!=127`; повтор только если за **1.5 с** нет подтверждения, максимум **5** попыток → `TIMEOUT` в логе и перезапуск roster через **30 с**), затем `WaitPopup` (ждёт `0x25 stage 1 awaiting 0` — popup у игрока), затем `Accept` (через `test_fake_accept_delay_ms` = **2500 мс** каждый fake шлёт один `stage 2`, те же пейсинг/подтверждения), затем `Done` (перезарядка чистым roster через **90 с**, если никто не подключился). Первый probe — через **1.5 с** после постановки резервации. Keep-alive резервации — повтор `Q…` каждые **8 с** (cookie тот же ⇒ roster не пересобирается). `OnMatchStarted` (первое NetMessage подключившегося клиента) — пауза; `OnMatchEnded` (последний `ClientSOCacheUnsubscribe`) — `Unreserve`, ожидание **12 с** (пока движок сбросит cookie), новый `Q`.
- Причина текущей схемы: **retail engine.dll** (`sv_max_queries_sec` "10.0", `sv_max_queries_window` "30", `sv_max_queries_sec_global` "500"; source `sv_ipratelimit.cpp`) отбрасывает connectionless-пакеты IP при > 300 за окно 30 с; клиент и fake шлют с одного IP; старый драйвер (пробы каждую секунду, 9–15 pps) терял пакеты → `awaiting=4` навсегда [H, §44.4]. Новая схема: ≈ 2×roster пакетов за матч.
- Семантика `0x25`: `awaiting` = число roster-записей со `stage < stage запроса`; popup появляется при `stage 1 awaiting 0` (все пробили stage 1), а не раньше; `awaiting=127` = аккаунта нет в резервации/cookie не совпал (`total=0`). `total` = размер roster на srcds — он приходит от сервера и должен совпадать с required players режима: расхождение пишется явно (`[FAKE-MM] … MISMATCH` на srcds, `[MM-ACCEPT] server roster total=… <== MISMATCH` на клиенте).
- Лог драйвера: `[FAKE-MM] roster expected=N actual=N`, `player=fake-NN(acct=…) stage=S sent/confirmed (awaiting=…,total=…)`, `server roster total=T expected=N OK|MISMATCH`, `awaiting=0 total=N`.

### 46.5 AccountID
- **Реальный AccountID определяется автоматически:** на клиенте — `ISteamUser::GetSteamID()` (`steam_hook.cpp:156`, `GetUserSteamId`) → `ClientGC::m_steamId`, `AccountId() = steamId & 0xFFFFFFFF`; 9101.`account_ids[0]` совпадает (лог-кросс-проверка) и служит только для сверки. На srcds — из **первого `0x21` (последние 8 байт = SteamID) с нашим cookie от не-fake аккаунта** (хук `ws2_32!WSARecvFrom`, устанавливается на srcds только при `-gc_mode`/`test_accept_mode`; `AcceptTest::SetServerSniff`, событие `GCEvent::TestRealPlayerSeen`, затем `ServerGC::CreateTestRoster`).
- **`test_real_account_id` — только необязательный TEST-fallback/override** (если хук на srcds не сработал); не требуется в обычном запуске и **не должен становиться частью API будущего backend** (backend получает AccountID от GC клиента).
- **[C live] Следствие/известное ограничение:** roster ставится только после первого `0x21`; клиент на `awaiting=127` немедленно получает `state=3` (callback уничтожается, поиск остаётся «висеть» в UI до ручной отмены) ⇒ **первая попытка Accept-режима после старта srcds всегда проваливается, вторая проходит**. В backend-архитектуре не проблема (AccountID известен до резервации).
- Сообщения `0x21` fake ссылаются на fake-AccountID; их фильтр — `IsFakeAccountId()` (`0xFA4E****`).

### 46.6 Server reservation (важное; детали §26–§34, §41–§42)
- **Cookie:** `GameServerCookieId = 0x293A206F6C6C6548` — compile-time константа, общая для клиента и srcds (одна сборка DLL). Идёт в 9107.`reservationid`, в `0x21`, в `Q`/`G` payload, в KV `QueueConnect`.
- **`IVEngineServer::ReserveServerForQueuedGame`** (`VEngineServer023`, vtable slot **149**, offset 0x254, `bool(__thiscall)(void*, const char*)` → `CBaseServer::ReserveServerForQueuedGame`, engine.dll `sub_101B50F0`/`sub_101BFAA0`), **обязательно на main thread** движка: GC-поток кладёт payload в `HostEvent::ReserveServerForQueuedGame` (`SharedGC::PostToHost`), выполнение — в `Hk_SteamAPI_RunCallbacks`/`Hk_SteamGameServer_RunCallbacks` (`steam_hook.cpp`, `DispatchReserveServerForQueuedGame`), fallback `SteamGameServer014→013→012→011→010`.
- **Casual/не-accept srcds:** одноразовая `G<cookie>,<cookie>,1:` при `k_EMsgGCServerHello`. **Accept srcds:** `Q…` c roster (см. 46.4).
- **`A2S_RESERVE_CHECK` 0x21** (33 байта): `FFFFFFFF | 0x21 | hostVersion=13805 u32 | token u32 | stage u32 | cookie u64 | steamid u64`. **`S2A_RESERVE_CHECK_RESPONSE` 0x25** (19 байт): `FFFFFFFF | 0x25 | hostVersion | token | stage | awaiting u8 | total u8`. Обработка — `CBaseServer::ReplyReservationCheckRequest` (`engine.dll sub_101BC1D0`, source `baseserver.cpp:2165`): для `Q` ищет аккаунт в `m_arrReservationPlayers`, **присваивает stage запроса** (в т.ч. меньший), считает awaiting; при `awaiting==0` рассылает `0x25` ВСЕМ участникам по сохранённым `adr/token`. Roster пересобирается **только при смене cookie** (`SetReservationCookie`), сбрасывается при `SetReservationCookie(0)` (`UpdateHibernationState`, `sv_main.cpp`).
- **Stages:** stage 1 — «connection probing» (popup при awaiting=0), stage 2 — Accept; `ReportGCQueuedMatchStart` — стаб `return 0` в retail (server.dll slot 47, `0x10145510`) ⇒ путь server→GC отсутствует, поэтому «все приняли» ловится на клиенте по `0x25 stage 2 awaiting 0` (TEST ONLY триггер).
- Тайминги движка: `sv_mmqueue_reservation_timeout`=21 с, `extended`=21 с, `sv_hibernate_postgame_delay` (5 с по умолчанию).
- **ПОВТОРНЫЙ RESERVATION НАМЕРЕННО НЕ ИСПРАВЛЯЕМ.** Проблема §34 (второй матч против того же srcds в non-accept режиме, cookie/expiry lifecycle) остаётся известной и оставлена до backend-этапа (там cookie и резервации будут создаваться backend'ом на матч). Для Accept-srcds действует только тестовая перезарядка roster (46.4), это не решение lifecycle.

### 46.7 Актуальная конфигурация (структура, которую ожидает код)
`csgo_gc/config.txt` (относительно рабочей папки игры; **один файл читается и клиентом, и каждым srcds; читается один раз при старте процесса — после правки нужно перезапустить процесс**), блок `matchmaking`:
```
"matchmaking"
{
	"test_server_address"	"192.168.1.150"   // машина с test srcds (клиент подключается сюда; fake шлют пакеты сюда)
	"test_server_port"	"27015"            // порт для режимов, которых нет в test_server_ports; srcds без -port берёт его же
	"test_server_ports"                       // клиент: порт srcds по имени режима
	{
		"competitive"	"27016"
		"wingman"	"27017"
		"dangerzone"	"27018"
		"scrimcomp5v5"	"27019"           // [TODO убрать, режим вне MVP]
	}
	"test_accept_mode"	""                    // srcds: держать ПУСТЫМ; режим srcds задаётся -gc_mode <mode>
	"test_fake_accept_delay_ms"	"2500"
	"test_diag"	"1"                       // клиент: [MM-DIAG] диагностика (хуки client.dll + сеть)
	// "test_real_account_id" "…"             // необязательный TEST override, обычно отсутствует
}
```
Командная строка srcds: `-gc_mode <competitive|wingman|dangerzone>` (приоритет над конфигом), `-port N` (порт для fake); пример: `srcds.exe -game csgo -console -port 27018 -gc_mode dangerzone +game_type 6 +game_mode 0 +map dz_blacksite`. Обычные (не-accept) srcds — без `-gc_mode`, порт из `test_server_port`.
**Устаревшая структура (не использовать):** единственный `test_server_port` для всех режимов; `test_accept_mode`/`test_real_account_id` в общем конфиге как обязательные (старый конфиг с `dangerzone` + `1050166997` вызвал ложный «DZ≈10»); комментарии про `TestMM`/«manual Casual test» (`test_mm.*` в проекте есть, но в рабочий путь не подключён). `csgo_gc/config.txt` в папке игры приведён к актуальной структуре; `examples/config.txt` — тоже.

### 46.8 Файлы проекта (`D:\csgo2021_gc\csgo_gc`)
`gc_client.cpp/.h` (`OnMatchmakingStart`, `OnMatchmakingStop`, `OnReservationFullyAccepted`, `m_pendingAccept`), `gc_server.cpp/.h` (`StartAcceptTestRoster`, `CreateTestRoster`, `OnTestRealPlayerSeen`, reservation при `k_EMsgGCServerHello`), `gc_shared.h` (`HostEvent::ReserveServerForQueuedGame`, `GCEvent::{TestRealPlayerSeen,ReservationFullyAccepted}`), `mm_modes.*` (таблица режимов + карты), `test_accept.*` (FakeRoster, хук `WSARecvFrom`: клиент — 0x25, srcds — sniff 0x21), `test_diag.*` (диагностика + патч `callback+92`, хуки `client.dll`), `config.*` (`TestServerPortForMode`, `DedicatedServerPort`, `-gc_mode`), `platform*.cpp` (`CommandLine`, `ResolveModuleInterface`), `steam_hook.cpp` (RunCallbacks-хуки, main-thread dispatch, `GetUserSteamId`), `main.cpp` (установка хуков), `test_mm.*` (не используется). Сборка: `build_local.bat` (MSVC x86/Ninja; `/wd4702` для `*.pb.cc`), результат `Build\release\csgo_gc\csgo_gc.dll`, деплой — только вручную по запросу пользователя.

### 46.9 Известные проблемы / нерешённое
1. **[C live]** Первая попытка Accept после старта srcds проваливается (`awaiting=127` → `state=3`); повтор проходит (46.5).
2. Повторный reservation/cookie lifecycle — оставлено (46.6, §34).
3. Один srcds = одна модель резервации: не-accept клиент против srcds в Accept-режиме (активный `Q`-roster, `total=10`, awaiting>0) ведёт себя как Accept (наблюдалось в §44.3); Accept-клиент против srcds с `G` — Accept не даёт. Клиенту нужен srcds своего режима; в backend серверы будут иметь режим в реестре. Любая попытка до постановки резервации (srcds только запущен) даёт `awaiting=127` → `state=3` (в т.ч. Casual).
4. Deathmatch/ArmsRace/Demolition/Skirmish: серверный режим (`+game_type/+game_mode`) live не подтверждён (DM подключался к srcds на 27016); Skirmish-варианты (`1<<(index-1)`) не разобраны [?].
5. Диагностика (не влияет на flow): `game/mmqueue=(exception)`; хуки `Accept sub_103F7B60` и `ServerReserved sub_103FB5C0` не ставятся («unexpected prologue» — сравнение байт с абсолютными операндами ломается релокацией); в логе последнего прогона добавился `accept` (раньше ставился) — проверять только префикс без адресных операндов.
6. Одиночный реальный игрок на матч; party (`account_ids[]` >1) не обрабатывается; cookie — константа, не per-match.
7. `ScrimComp5v5` в коде/конфиге ещё присутствует (46.3 [TODO]); Cooperative не поддержан.
8. Линукс-ветки (`platform_unix.cpp`, хуки) не собирались/не проверялись.

### NEXT STAGE — JAVA MATCHMAKING BACKEND
**Намерение.** Java backend должен заменить локальную «серверную» matchmaking-логику GC-DLL (выбор сервера, размеры roster, fake players, режимо-специфичные правила, адрес сервера как результат). Архитектура (целевая):
```
CS:GO client → csgo_gc.dll ──HTTP──▶ Java Matchmaking Backend
                                       ├── Active Searches
                                       ├── Game Servers
                                       └── Admin Panel
(в дальнейшем: Active Searches → Matchmaker → Game Server → reservation → GC → 9107/Accept/QueueConnect)
```
**Первый этап Java backend НЕ делает matchmaking assignment.** Только:
1. admin panel (browser) с **обязательной авторизацией login/password**;
2. **CRUD игровых серверов** (адрес, порт);
3. категория/режим сервера;
4. карта сервера;
5. включение/выключение сервера;
6. отображение активных игроков в поиске.

**Что GC передаёт backend при начале поиска** (данные уже есть в ClientGC на момент 9101): **AccountID** (из `ISteamUser`), **game_type** (`eGame`/полное значение), **game_mode** (строка, из `MM::GameMode`: например `competitive`/`scrimcomp2v2`), **категория режима** (accept/no-accept и т. п.; точный набор согласовать в контракте), **выбранные карты/mapgroup** (маска + декодированный список), **время начала поиска**. Backend на этом этапе **только хранит это как `active search`** (и снимает при MatchmakingStop). **Не делать на Java-этапе:** выдачу сервера, reservation, создание матча, QueueConnect, fake/test players, matching; текущая локальная логика GC (46.2–46.6) при этом **остаётся рабочей и не вырезается**; сбой backend не должен ломать текущий клиентский flow.
**Отложено на следующие этапы** (ранее сформулированные пожелания — test bots/симуляция очереди, matcher, реестр/heartbeat srcds, reservation orchestration, structured match-логи, редактируемая конфигурация режимов, миграция логики из GC и удаление `test_*` из GC): не входят в первый этап, но архитектура должна их допускать; `test_real_account_id`, fake generation, `test_server_*` уйдут из GC только после того, как backend реально заменит функциональность и пройдёт тесты.
**Перед реализацией — предложить и согласовать:** версию JDK (на машине JDK/Maven/Gradle нет, устанавливать только после согласования), build system, HTTP/API framework, структуру проекта, контракт GC ↔ Java (JSON поверх HTTP; GC-сторона потребует HTTP-клиента без новых тяжёлых зависимостей, напр. WinHTTP через `platform_*.cpp`, неблокирующего для GC worker thread), модель GameServer / ActiveSearch (и в будущем Match/Reservation), архитектуру browser panel и хранение учётных данных админа. Только после согласования — реализация.

### 46.10 Ограничения проекта (закреплено)
- **Git полностью под контролем пользователя:** никаких commit/push/pull/reset/merge/rebase/checkout и других изменяющих Git-операций.
- Не менять рабочую архитектуру текущего matchmaking без необходимости; не делать новый большой RE-аудит — только точечные вопросы, блокирующие реализацию.
- **`RESEARCH_FINDINGS.md` — главный checkpoint/context-файл проекта:** новые подтверждённые findings добавлять сюда; старые противоречащие findings не удалять — добавлять correction с более высоким статусом.
- DLL в игру копируется только по явному запросу пользователя (с указанием, что копируется); бэкап предыдущей DLL сохраняется рядом.
- Не идти на workaround вида `required=1`/`required=10`, не отключать/обходить Accept, не подменять UI.

### §46 STATUS
Checkpoint записан. Клиентский подбор (Competitive/Wingman/DZ с Accept, обычные режимы) работает и проверен live; fake roster 10/4/16 работает; автоопределение AccountID работает (с ограничением «первая попытка после старта srcds»). Следующий шаг — по согласованию: предложение по стеку/контракту/модели/панели Java backend (первый этап — только панель + CRUD серверов + хранение active searches).


---

## 47. JAVA MATCHMAKING BACKEND — ЭТАП 1 РЕАЛИЗОВАН (active searches + реестр серверов + admin panel)

**Статус:** реализовано, собрано, протестировано (15 интеграционных тестов + живой запуск по HTTP + проверка JS панели). **C++ GC не менялся, DLL в игру не копировалась, Git не трогался.** Интеграция C++ → Java **ещё не сделана** (в `csgo_gc` нет HTTP-клиента). Не отменяет §46: раздел «NEXT STAGE — JAVA MATCHMAKING BACKEND» выполнен в части первого этапа. **[C]** подтверждено, **[H]** высокая уверенность, **[?]** не проверено.

### 47.1 Что сделано
- Проект `D:\csgo2021_gc\java-backend\` (README.md с инструкцией запуска на Windows, curl/PowerShell командами и описанием будущих изменений C++). Запуск: `build.cmd` (сборка + тесты), `run.cmd`; конфиг `config\application.properties` (пример `config\application-example.properties`, файл в `.gitignore`) либо переменные окружения `BACKEND_ADMIN_USERNAME`, `BACKEND_ADMIN_PASSWORD`, `BACKEND_API_KEY`, `BACKEND_DATABASE_PATH`, `BACKEND_STALE_SEARCH_TIMEOUT`, `SERVER_ADDRESS`, `SERVER_PORT`. **Без `backend.admin.*` backend не стартует, учётных данных по умолчанию нет** [C, проверено запуском без них].
- **Тулчейн (поправка к §46 «JDK не устанавливать без согласования»):** после прямого указания пользователя (Java 21 + Spring Boot + Maven/Gradle + SQLite) установлен **портативный** (без системных изменений, без PATH/реестра) **Temurin JDK 21.0.12.1** в `D:\csgo2021_gc\tools\jdk21` и **Maven 3.9.11** в `tools\maven` (папка `tools/` в `.gitignore`; SHA-256/SHA-512 скачанных архивов сверены с опубликованными). В проекте Maven Wrapper (`mvnw.cmd`), `build.cmd`/`run.cmd` берут `JAVA_HOME` или `..\tools\jdk21`.
- Стек [решения]: **Java 21, Spring Boot 3.5.16, Maven, SQLite (`sqlite-jdbc`, `JdbcClient`, один соединение Hikari — единственный писатель), Spring Security (форма входа + session cookie `MMSESSION`, CSRF на `/admin/**`), HTML/CSS/JS без сборки, Jackson snake_case.**
- Функции: страница `/admin` (обязательный вход, logout, блокировка формы после 5 неудач на 5 мин, CSP без inline-скриптов, `X-Frame-Options: DENY`); раздел **Active Searches** (AccountID, `game_type`, `game_mode`, режим/метка Accept, карты, «N s ago», длительность, статус, кнопка End, «show ended», добавление TEST-заявки); раздел **Game Servers** (CRUD, enable/disable, host/порт/категория/карта/дата; `host:port` уникален; **ScrimComp5v5 и Cooperative → 400**); REST API для GC.

### 47.2 Контракт GC ↔ Java (v1) — что и откуда в C++
Все тела JSON, поля snake_case. Защита: заголовок `X-Api-Key` (если `backend.api-key` задан; пусто = открыто + предупреждение в логе); ключ не открывает админку.
- `GET /api/v1/health` (публично).
- `POST /api/v1/matchmaking/search` — `{account_id, game_type, [mode], [game_mode], [maps[]], [request_id]}`. Источники в `ClientGC::OnMatchmakingStart` (`gc_client.cpp`, все данные уже есть на момент 9101): `account_id` = `AccountId()` (`ISteamUser`), `game_type` = `request.game_type()` (сырое `eGame | mapMask<<8`), `mode` = `mode->name`, `game_mode` = `mode->serverGameMode`, `maps` = `selection.maps` (`MM::DecodeMapSelection`, пусто для Skirmish), `request_id` — уникальный id поиска на стороне GC. **Категория выводится из `game_type & 0xF`**, `mode`/`game_mode` необязательны и при наличии проверяются (`mode` обязан совпасть → иначе `400 mode_mismatch`); eGame вне MVP (9, 11, неизвестные) → `400 unsupported_game_type`. Ответ: `result` = `created` / `refreshed` (тот же поиск повторён: тот же `request_id`, либо без него те же режим+карты; обновляется только таймаут) / `replaced` (смена режима/карт или новый `request_id`: запись переписана, время старта сброшено) + `search{…}`.
- `POST /api/v1/matchmaking/cancel` — `{account_id, [request_id]}` → `{cancelled: bool, [reason: no_active_search | request_id_mismatch]}`; идемпотентно; запоздалый cancel со старым `request_id` новый поиск не снимает.
- `GET /api/v1/matchmaking/searches[?include_finished=true]` (ключ GC или сессия админа) — список; его же опрашивает панель раз в 2 с.
- **Дубликатов нет:** уникальный частичный индекс SQLite `(account_id) WHERE status='SEARCHING'` + сериализованная логика сервиса. Статусы: `SEARCHING`, `CANCELLED`, `EXPIRED` (таймаут), `REMOVED` (админ). Timeout: `backend.stale-search-timeout` (по умолчанию 15 мин), отсчёт от последнего `search`; проверка лениво при каждом чтении + фоновый reaper (`backend.reaper-interval` 10 с); завершённые записи хранятся `backend.finished-search-retention` (1 ч). Отдельного heartbeat нет.
- Таблицы `matchmaking_search`, `game_server` (`schema.sql`, идемпотентно при старте). Категории (`ModeCategory`, зеркало `mm_modes.cpp`): competitive(8, 10 игроков), wingman(10, 4), dangerzone(13, 16), casual(7), deathmatch(6), armsrace(4), demolition(5), skirmish(12).

### 47.3 Как проверено
- **15 интеграционных тестов** (`BackendIntegrationTest`, MockMvc + управляемые часы): публичность health, редирект/401 без входа, вход/выход, неверный пароль, CSRF на записи админа, обязательность API-ключа, создание/список, минимальное тело, дедупликация (`created`→`refreshed`→`replaced`, `request_id`), валидация (eGame 9/11/99, `mode_mismatch`, плохие карты, не-JSON), cancel (в т.ч. запоздалый с чужим `request_id`), timeout/продление/очистка (без sleep), тестовая заявка админа и End, CRUD серверов (409 дубликат, 400 на порт/категорию/карту), 405/404/415 без 500. `BUILD SUCCESS`.
- **Живой запуск jar по реальному HTTP** (порт 18080/18081): логин с cookie+CSRF, 403 без CSRF, CRUD серверов, toggle `/enabled`, реальные `game_type` из логов (`4877`→dangerzone, `519`→casual, `518`→deathmatch), cancel по `request_id`, **реальный timeout** (20 с → `EXPIRED`), запуск через `run.cmd` без `JAVA_HOME`.
- **JS панели** проверен в Browser pane на реальном HTML панели и реальных формах JSON API (подменённый `fetch`, страница `data:`): рендер строк/тегов Accept и TEST/времён, «show ended», добавление и End заявки, вкладка серверов (добавить/toggle/edit/delete), CSRF-заголовок на каждой записи. **Пароль в браузерную форму не вводился** (ручную проверку входа в браузере пользователь делает сам) [?].

### 47.4 Находки окружения и исправленные дефекты
1. **[C] JDK 21 на Windows: `Unable to establish loopback connection` / `SocketException: Invalid argument: connect`** при открытии NIO Selector (без него Tomcat не стартует). JDK строит Selector на AF_UNIX-сокете во временной папке; на этой машине это **не работает для `C:\Users\Administrator\AppData\Local\Temp`** (обе формы пути — длинная и 8.3), но работает для `C:\Users\Public`, `C:\Windows\Temp`, `C:\mm_tmp`, `D:\…`. Причина **[?] не установлена** (ACL/содержимое папки в порядке). Обход: `main()` на Windows ставит `jdk.net.unixdomain.tmpdir=<cwd>\data\tmp`, если свойство не задано. Тесты MockMvc эту проблему **не видят** (Tomcat в них не запускается) — поэтому обязателен живой запуск.
2. **[C] `NoDefaultCurrentDirectoryInExePath=1`** в этом окружении: `cmd` не ищет исполняемые/`.cmd` в текущем каталоге → `call mvnw.cmd` не находился; скрипты используют `%~dp0` (полные пути). `.cmd` хранятся с CRLF (`.gitattributes`).
3. **[C] Дефект:** общий `@ExceptionHandler(Exception)` превращал 405/404/415 в 500 — исправлено (статусы Spring MVC сохраняются, security-исключения пробрасываются), есть регрессионный тест.
4. Исправлена ошибка в маппере строки (`wasNull` после других getter'ов) до первого запуска.

### 47.5 Что НЕ сделано (намеренно, следующие этапы)
Подбор игроков, выбор/выдача сервера, reservation, `MatchmakingGC2ClientReserve`, QueueConnect, fake players, Accept, party, рейтинг, проверка доступности srcds, миграция серверной логики из GC. **C++ → Java (когда решим делать):** WinHTTP-клиент в `platform_windows.cpp` с отдельным потоком отправки (не блокировать GC worker thread, таймаут 1–2 с, ошибки только в лог, недоступный backend не ломает клиентский flow); конфиг `matchmaking.backend_url` / `backend_api_key` (пусто = выключено); вызовы в `ClientGC::OnMatchmakingStart` (`POST /search`) и `OnMatchmakingStop` (`POST /cancel`); **открытое решение:** слать ли `cancel` в момент финального 9107 / локального мгновенного матча (пока подбор делает GC, поиск в панели почти не виден) или оставить до таймаута. Локальная matchmaking-логика GC остаётся рабочей. TODO из §46.9 (убрать `scrimcomp5v5` из `mm_modes.cpp` и конфигов, диагностика `test_diag`) не затрагивались.

### §47 STATUS / NEXT STEP
Backend этапа 1 работает автономно (`java-backend\run.cmd` → `http://127.0.0.1:8080/admin`), принимает заявки от любого HTTP-клиента по контракту 47.2. Дальше — по решению пользователя: (1) реализовать C++ HTTP-клиент и вызовы в `OnMatchmakingStart/Stop` (только отправка, без влияния на клиентский flow); (2) затем реестр серверов с heartbeat и Matchmaker (следующие этапы).


---

## 48. C++ GC → JAVA BACKEND: SIDE-CHANNEL ОТПРАВКА СОСТОЯНИЯ ПОИСКА (реализовано, собрано, протестировано офлайн; в игре не запускалось)

**Статус:** код написан, `csgo_gc.dll` собран (0 ошибок / 0 предупреждений в нашем коде), HTTP-клиент проверен харнессом против реального Java backend и сценариями отказов. **DLL в игру НЕ копировалась, Git не трогался, Java backend не менялся** (кроме строки статуса в `java-backend/README.md`). **В самой игре интеграция пока не проверена [?]** — это следующий шаг пользователя. Не отменяет §46/§47. **[C]** подтверждено, **[?]** не проверено.

### 48.1 Что изменено
- **`csgo_gc/backend_client.h/.cpp` (новые)** — весь клиент. API: `BackendClient::SearchStarted(SearchInfo, steamId)` и `BackendClient::SearchCancelled()`. Оба только кладут запрос в очередь и возвращаются (не блокируют, не бросают исключений, `try/catch(...)`).
- **`csgo_gc/gc_client.cpp`** — ровно две точки + `#include`: (1) `ClientGC::OnMatchmakingStart` сразу после строки лога `[MM] mode=…` (блок `BackendClient::SearchStarted`), (2) `ClientGC::OnMatchmakingStop` в начале (`BackendClient::SearchCancelled()`). Всё остальное (9101/9102/9104/9107, reservation, fake roster, Accept, QueueConnect, `m_pendingAccept`, ветки Casual/Accept) **не изменено**.
- **`csgo_gc/config.h/.cpp`** — `GCConfig::BackendUrl()` / `BackendApiKey()`, читаются из `matchmaking.backend_url` / `matchmaking.backend_api_key`.
- **`csgo_gc/CMakeLists.txt`** — `backend_client.cpp` в `csgo_gc`.
- **Конфиги:** игровой `C:\Program Files (x86)\Steam\steamapps\common\csgo legacy\csgo_gc\config.txt` (бэкап рядом: `config.txt.bak_pre_backend`) — `backend_url "http://192.168.1.150:8080"`, `backend_api_key "test-api"` (значение = `backend.api-key` из `java-backend\config\application.properties` на момент правки); `examples/config.txt` — те же ключи с пустыми значениями (интеграция выключена).
- Остальной `matchmaking`-блок конфига не менялся.

### 48.2 Что именно уходит в backend
Только клиент (`ClientGC`); srcds `BackendClient` не вызывает и потоков не создаёт. Поток и очередь создаются лениво при первом запросе.
- **Старт поиска (9101, поддерживаемый режим — те же условия, что у остальной логики `OnMatchmakingStart`; неподдерживаемые eGame 9/11/… не отправляются, backend их всё равно отверг бы):**
  `POST <backend_url>/api/v1/matchmaking/search`, `Content-Type: application/json`, `X-Api-Key: <backend_api_key>`, `Connection: close`, тело:
  `{"account_id":<AccountId()>,"game_type":<request.game_type()>,"mode":"<MM::GameMode::name>","game_mode":"<serverGameMode>","maps":[…декодированные карты, если режим их имеет…],"request_id":"<steamid64>-<unix ms>-<счётчик>"}`.
  Пример: `{"account_id":4000000001,"game_type":520,"mode":"competitive","game_mode":"competitive","maps":["de_dust2","de_mirage"],"request_id":"76561201960265729-1789815560973-1"}`. `account_id` = `ISteamUser::GetSteamID()` & 0xFFFFFFFF (если 0 — берётся `account_ids(0)` из 9101, если и его нет — запрос не отправляется, только строка в логе). `maps` берётся из `MM::DecodeMapSelection` (`selection.maps`, только если `selection.decoded`), без изменения существующей логики выбора карты.
- **Отмена (9102):** `POST <backend_url>/api/v1/matchmaking/cancel`, тело `{"account_id":N,"request_id":"<тот же id>"}`. Отправляется **только если клиент отслеживает ранее отправленный поиск** (повторный 9102 и 9102 без нашего старта ничего не шлют). Новый 9101 без 9102 просто отправляет новый `search` с новым `request_id` (backend отвечает `replaced`).
- Логи (`gc_log.txt` / консоль): `[BACKEND] enabled: … (api key set), side channel only`, `[BACKEND] POST /api/v1/matchmaking/search {json}`, `[BACKEND] search -> HTTP 200 in N ms: {"result":"created",…}`, при ошибках `[BACKEND] search FAILED: <причина> … -- matchmaking is not affected`. Ключ API в лог не пишется.

### 48.3 Устройство клиента (гарантии side channel)
- Свой HTTP/1.1-клиент на сокетах (`#ifdef _WIN32` Winsock / BSD), **без новых зависимостей и без WinHTTP** (в окружении пользователя задан `HTTP_PROXY=127.0.0.1:10809` — сырые сокеты идут напрямую, прокси не участвует). Только `http://`; `https://` → интеграция выключена с сообщением.
- Один **detached worker-поток** + очередь (лимит **16**, при переполнении отбрасывается самый старый запрос с записью в лог; запрос, ждавший в очереди > **30 с**, отбрасывается). Синглтон **намеренно не уничтожается** (нет join в `DllMain`/выгрузке — иначе зависание на loader lock).
- Таймауты: connect **1500 мс**, весь запрос (resolve+connect+send+ответ) **3000 мс**; неблокирующие сокеты + `select`. Ответ читается до Content-Length / chunked-конца / EOF (Tomcat отвечает `chunked`, декодируется), ≤ 16 КБ. Без ретраев: потерянный запрос не повторяется.
- Ошибка/таймаут/`4xx`/`5xx` → только строка `[BACKEND] … FAILED … -- matchmaking is not affected`; результат ни на что не влияет (9107/Accept/reservation/roster не зависят от ответа). Пустой `backend_url`, плохой URL, `https://`, управляющие символы в ключе → интеграция выключена, одна строка `[BACKEND] disabled: …`.

### 48.4 Проверка (харнесс scratchpad `bt/`: реальные `backend_client.cpp` + `keyvalue.cpp` и **реальный формат игрового `config.txt`** (тот же парсер), заглушки Print/GetConfig; backend — отдельный экземпляр Java на 127.0.0.1:18090 с ключом `test-api`)
| Сценарий | Результат |
|---|---|
| backend up, ключ верный: search → cancel | `HTTP 200` `created` → `cancelled:true`; на backend запись `CANCELLED` с теми же режимом/картами/`request_id` [C] |
| вызов из «GC-потока» | `SearchStarted` 1–9 мс (первый вызов, создание потока), `SearchCancelled` 20–50 мкс [C] |
| неверный ключ | `HTTP 401 invalid_api_key` только в логе, процесс жив, запись на backend не создана [C] |
| новый поиск без 9102 → cancel ×2 | `created` → `replaced` (тот же id записи, режим wingman), один `cancel`, второй не отправлен [C] |
| cancel без отслеживаемого поиска | ничего не отправлено [C] |
| backend выключен (порт закрыт) | `connect timed out (backend not running or not reachable?)` за 1,5 с (на Windows отказ loopback-connect приходит позже таймаута) [C] |
| сервер принимает соединение и молчит | обрыв ровно на 3,0 с, `no answer within the timeout` [C] |
| connect заблокирован (заполненная accept-очередь) | `connect timed out` за 1,5 с [C] |
| 25 `SearchStarted` подряд при недоступном backend | все вызовы вернулись, самый медленный 6,8 мс; 9 старых запросов отброшены (лимит очереди) [C] |
| `backend_url` пустой / `https://` / `ftp://` / `http://` / порт 99999 | `[BACKEND] disabled: …`, запросов нет [C] |
| `http://127.0.0.1:18090/` (слеш в конце) | работает [C] |
| нерезолвящееся имя | `cannot resolve host` через **11 с** (блокирующий `getaddrinfo`; блокируется только worker, вызывающий поток не страдает) — для IP-адреса (`192.168.1.150`) DNS нет [C] |
Дополнительно: Tomcat отвечает `Transfer-Encoding: chunked` — сначала в лог попадали размеры чанков, исправлено декодером. Оговорка окружения: на машине разработки работает TUN-прокси, поэтому «недостижимый» адрес `192.0.2.1` принимает TCP-соединение и молчит (~1 с) — реальный blackhole-connect проверен через заполненный backlog.
**Не проверено:** запуск в самой игре (`csgo.exe`), реальный 9101/9102 → `[BACKEND]` в `gc_log.txt`; **backend пользователя на `192.168.1.150:8080` на момент проверки не работал** (connection refused, процессов java нет), поэтому тесты шли на собственном экземпляре; Linux/macOS-ветка сокетов не компилировалась [?].

### 48.5 Сборка и артефакты
- Команда: `cmd //c "D:\\csgo2021_gc\\build_local.bat"` (из Git Bash) / `D:\csgo2021_gc\build_local.bat` (cmd); результат — `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll` (5 754 368 байт, собран 2026-09-19 15:03, SHA-256 `c2f663bfa7f0501c07e00bdfa603d5a41d12d29292d555fd98df0de38ce6a76b`), 0 ошибок, 0 предупреждений в нашем коде (единственное предупреждение — CMake-`FetchContent` в `funchook`, как и раньше).
- **Установленная в игре DLL — прежняя** (`csgo_gc\csgo_gc.dll` от 03:28, до интеграции); копировать новую — только по запросу пользователя, с бэкапом.

### 48.6 Что проверить в игре и известные ограничения
1. Запустить `java-backend\run.cmd` (слушает `192.168.1.150:8080`), запустить игру с новой DLL, начать поиск: в `gc_log.txt` должны появиться `[BACKEND] enabled …`, `POST …/search {…}`, `search -> HTTP 200 …: {"result":"created" …}`; в панели `/admin` — заявка с реальным AccountID, режимом, `game_type`, картами. Отмена поиска в UI → 9102 → `cancel -> HTTP 200 … "cancelled":true`.
2. **Открытый вопрос (не менялось по ТЗ):** `cancel` уходит только по 9102. После успешного подбора/подключения (`QueueConnect`) клиент может не слать 9102 — тогда заявка висит в панели до `backend.stale-search-timeout` (15 мин) или до следующего поиска (`replaced`). Тест покажет, шлёт ли клиент 9102 при коннекте; решение (слать ли `cancel` при финальном 9107 / локальном мгновенном матче) — за пользователем.
3. Потерянный запрос (backend был выключен) не повторяется: поиск не появится в панели, а потерянный `cancel` оставит заявку до таймаута. Ключ API хранится в `config.txt` открытым текстом (ключ доверенной сети). DNS для имён хостов блокирующий (только worker). Соединение без TLS.

### §48 STATUS / NEXT STEP
C++ → Java side-channel готов и собран, DLL лежит в `Build\release`, в игру не установлена. Дальше: пользователь копирует DLL (по запросу — с бэкапом) и проверяет `[BACKEND]`-строки и панель; по результату — решение про `cancel` при подключении, затем следующий этап (реестр серверов с heartbeat, Matchmaker).


---

## 49. ВЫБОР ИГРОВОГО СЕРВЕРА ПЕРЕНЕСЁН С КЛИЕНТА (C++ GC / `config.txt`) НА JAVA BACKEND

**Статус:** реализовано и собрано: Java matchmaker (этапы A и B), C++ клиент получает assignment опросом и запускает существующий 9107/Accept/QueueConnect (этап C), клиентский выбор сервера удалён из кода и `config.txt` (этап D). Проверено: 37 Java-тестов, харнесс с реальным клиентским кодом против реального backend, реальные записи вашего реестра. **DLL этой стадии в самой игре не запускалась [?]** (DLL §48 пользователь уже гонял в игре, см. 49.6). Не отменяет §46–§48. **[C]** подтверждено, **[?]** не проверено.

### 49.1 Аудит перед изменениями
- **`test_*` в C++:** `config.h/.cpp` (`TestServerAddress/Port/PortForMode`, `TestAcceptMode`, `TestRealAccountId`, `TestFakeAcceptDelayMs`, `TestDiag`), `gc_client.cpp` `OnMatchmakingStart` (`ParseIpAddress(TestServerAddress())`, `TestServerPortForMode(mode->name)`), `gc_server.cpp` `StartAcceptTestRoster` (override `TestRealAccountId`) / `CreateTestRoster` (`serverAddress = TestServerAddress()`, `DedicatedServerPort()`), `test_diag.cpp` (`TestDiag`), `main.cpp` (`TestAcceptMode` → хук srcds). Клиентом использовались только первые пять из gc_client; остальное — сторона srcds (fake roster).
- **Путь 9101 → QueueConnect** (не менялся): 9101 → `OnMatchmakingStart` → 9107 #1 (`direct_udp_ip/port`, `reservationid`, `map`; для Accept + `reservation.game_type`) → 0x21 stage 1 → (Accept: 0x25, popup, stage 2) → второй 9107 (`OnReservationFullyAccepted`) → QueueConnect → `connect`. Non-accept: тот же первый 9107 без `reservation` + локальный мост `G`-резервации на host-thread.
- **Контракт Search → MatchAssignment** — см. 49.3. **Решение:** остаётся C++-транспорт (9107 / `G`/`Q`-резервация / Accept / QueueConnect / fake roster на srcds), на Java переезжает *решение* «какой сервер, какая карта, когда матч полный».

### 49.2 Архитектура (итог)
```
CS:GO ─9101─▶ csgo_gc.dll ──POST /search──▶ Java backend ── matcher: свободный сервер (категория, карта ∈ maps[], enabled, AVAILABLE)
   ▲             (worker: register → poll 1 c)   │           classic: 1 сервер = 1 матч;  Accept: сбор required_players
   │                                             ▼
   └─ 9107/Accept/QueueConnect ◀─ assignment {match_id, server_address, server_port, map, accept_required, required_players}
```
Клиент не знает, какие серверы существуют; реестр (адрес, порт, категория, карта, enabled, состояние) — в базе backend, редактируется в админке.

### 49.3 Контракт GC ↔ Java (v2, поверх §47)
- `POST /api/v1/matchmaking/search` (тело как в §47.2) → `{"result": created|refreshed|replaced, "search": {…}}`; matcher срабатывает сразу, ответ уже может быть `READY_TO_CONNECT` с `assignment`.
- **`GET /api/v1/matchmaking/search/{request_id}`** (API-key или админ-сессия) → `{"search": {…}}`; **обновляет `last_seen_at` живой заявки** (опрос = heartbeat, поэтому `stale-search-timeout` теперь означает «GC перестал опрашивать»); 404 — backend не знает поиска.
- `POST /api/v1/matchmaking/cancel` — как в §47. **Новое:** `POST /api/v1/servers/state` `{address, port, map?, state?}` (API-key): сервер сообщает о себе (только зарегистрированные; `BUSY` принимается всегда, `AVAILABLE` — если сервер не RESERVED; `RESERVED` → 400).
- **Статусы заявки:** `SEARCHING`, `MATCHED` (Accept: сервер зарезервирован, игроки собираются, `match.players/required_players`), `WAITING_ACCEPT` (матч полный, есть `assignment`, идёт retail-Accept), `READY_TO_CONNECT` (классика: сервер выдан), `CANCELLED`, `EXPIRED`, `REMOVED` (админ), `COMPLETED` (назначенная заявка старше `assigned-search-timeout`).
- **`assignment`:** `{match_id, server_id, server_address (IPv4; hostname реестра резолвится на backend), server_port, map, accept_required, required_players}`. Cookie резервации в нём **нет** (см. 49.8).
- **Клиент (`backend_client.*`, worker-поток, сессия):** `SearchStarted` → фаза Register (`POST /search`, при сбое повтор каждые 2 с, лог 1-й и каждый 10-й сбой) → Poll (`GET`, раз в 1 с) → Done (assignment / терминальный статус / 4xx-отказ / 15 мин). 404 при опросе → повторная регистрация. Результаты (смена статуса, число игроков, assignment) уходят обработчиком в GC-поток как `GCEvent::BackendSearchResult`. `SearchCancelled` (9102) останавливает опрос и ставит `POST /cancel` в очередь (приоритет над опросами); результаты устаревшего `request_id` игнорируются. Таймауты connect 1,5 с / запрос 3 с; вызовы из GC-потока не блокируют.
- **`ClientGC`:** `OnMatchmakingStart` (та же декодировка режима/карт) → `BackendClient::SearchStarted`; `OnBackendSearchResult`: `IsAssigned()` → `StartServerFlow(assignment)` (**прежний код** 9107 #1 / `ArmClient` / локальная `G`-резервация, адрес-порт-карта из assignment; при неразборчивом адресе — ошибка в лог, без подстановки 127.0.0.1); `MATCHED` → лог прогресса; терминальный статус (админ убрал поиск, timeout) → лог + сброс (в клиентском UI для этого нет сообщения [?]). Без `backend_url` поиск не обслуживается (лог `[MM] matchmaking.backend_url is not configured…`).

### 49.4 Java backend (этап 2)
- **Модель (SQLite, миграция «на месте», проверена на реальной БД пользователя и тестом):** `game_server` + `state` (AVAILABLE/RESERVED/BUSY), `reserved_match_id`, `reserved_at`, `last_assigned_at`, `last_heartbeat_at`, `max_players`; `matchmaking_search` + `match_id`, `matched_at`, уникальный частичный индекс «одна живая заявка на AccountID» теперь по четырём живым статусам; новая `matchmaking_match` (id `m-xxxxxxxx`, категория, сервер, IPv4, порт, карта, `required_players`, `accept_required`, `FORMING`/`READY`/`ENDED`/`CANCELLED`). `SchemaInitializer` (schema.sql + `ALTER TABLE ADD COLUMN`).
- **Matcher** (`SearchService`, все мутации сериализованы одним монитором вместе с `GameServerService`; вызывается при search / poll / cancel / изменении серверов и по таймеру `matcher-interval` 1 с): подходит сервер, если `enabled AND AVAILABLE AND category == заявки AND (maps[] пуст OR server.map ∈ maps[])`; сервер без карты подходит только заявке без карт; порядок — дольше всех не назначавшийся; `reserve` — `UPDATE … WHERE state='AVAILABLE'` (два поиска не получат один сервер). Классика: матч на 1, сразу `READY_TO_CONNECT`. Accept: присоединение к `FORMING`-матчу с подходящей картой либо новый матч на свободном сервере; при `players == required` матч `READY`, все участники `WAITING_ACCEPT`. **`required_players`** — из таблицы режимов (10/4/16); `backend.required-players.<mode>` — переопределение **только для тестовой среды**, где недостающих участников даёт тестовый ростер srcds.
- **Освобождение серверов:** админ Release / Set available / Set busy (завершает матч; формирующийся распадается, игроки возвращаются в `SEARCHING`), удаление сервера (то же), отчёт сервера, TTL `server-reservation-ttl` (10 мин от готовности матча — пока нет heartbeat srcds, «матч закончился» backend не знает), и **правило «новый поиск»**: игрок, начавший новый поиск (не повтор того же `request_id`), покинул сервер прежнего матча — если он был в нём единственным участником и матч `READY`, сервер сразу `AVAILABLE`. Причина: провал Accept/подключения и «искать снова» — обычный сценарий (в т.ч. известная §46.9 п.1 «первая попытка Accept после старта srcds проваливается»), иначе повтор ждал бы TTL. Матч из ≥2 игроков одним уходом не освобождается. Cancel назначенной заявки сервер **не** освобождает (клиент может слать 9102 и при успешном подключении [?]).
- **Админ-панель:** колонка «Server / match» в поисках, статусы, состояние серверов (пиллы, Release / Set busy / Set available, «Last report», начальное состояние при добавлении), вкладка **Matches**. JS проверен на реальных ответах API (страница-снимок) и загрузкой `admin.js` с живого сервера (парсится; CSP `script-src 'self'` работает).
- Прочее: `BackendProperties` + `matcher-interval`, `server-reservation-ttl`, `assigned-search-timeout`, `required-players`; `StartupValidator` проверяет override и пишет предупреждение.

### 49.5 Что удалено у клиента
- **`config.txt` (игровой и `examples/`):** блок `matchmaking` = **только** `backend_url`, `backend_api_key`. Удалены `test_server_address`, `test_server_port`, `test_server_ports`, `test_accept_mode`, `test_fake_accept_delay_ms`, `test_real_account_id`, `test_diag` (бэкап `config.txt.bak_pre_backend_selection`, ранее `config.txt.bak_pre_backend`). Устаревшие комментарии над блоком убраны.
- **Код:** удалены `TestServerAddress/Port/PortForMode`, `TestRealAccountId` и чтение всех этих ключей; `test_accept_mode` остаётся **только** как `-gc_mode` в командной строке srcds; `TestFakeAcceptDelayMs` — константа 2500 мс; `DedicatedServerPort()` — `-port`, иначе 27015 (умолчание srcds); новое `DedicatedServerAddress()` — `-ip`, иначе 127.0.0.1 (адрес, куда fake-участники srcds шлют 0x21; **[?] не проверено против реального движка**: при srcds с `-ip` — этот адрес, без `-ip` — loopback, в симуляции `sim/` работало). `matchmaking.test_diag` читается как необязательный ключ (диагностика включена по умолчанию, в конфиге его нет) — независимый механизм, не выбор сервера. Override `test_real_account_id` на srcds удалён (AccountID определяется из первого 0x21).
- **Не тронуто:** 9101/9102/9104/9107, `G`/`Q`-резервация, fake roster (`test_accept.*`), Accept-флоу, QueueConnect, `mm_modes.*`.

### 49.6 Проверка
- **Java: 37 тестов** (`BackendIntegrationTest` 15, `MatchmakingIntegrationTest` 21, `SchemaMigrationTest` 1): выбор сервера для классики, ожидание и подхват при появлении сервера, режим/карта/disabled/BUSY не подходят, эксклюзивность сервера, два сервера — два поиска (наименее используемый первым), cancel, TTL/COMPLETED, hostname → IPv4, сбор игроков Competitive/Wingman/DZ (несовместимые карты не объединяются, полный матч даёт всем один сервер), распад формирующегося матча, stale-игрок покидает матч, polling продлевает поиск, отчёт сервера, удаление зарезервированного сервера, правка не меняет состояние, правило «новый поиск» (и «общий матч не освобождается»), миграция БД этапа 1.
- **C++ (харнесс `scratchpad/bt`: реальные `backend_client.cpp` + `keyvalue.cpp`, реальный формат игрового `config.txt`, реальный Java-backend):** классика one-player → `READY_TO_CONNECT` на правильном сервере из реестра (не на сервере с другой картой) [C]; DM на карте без сервера, Casual на карте без сервера, только disabled/BUSY-серверы → поиск ждёт, затем cancel останавливает опрос [C]; два поиска на один сервер: второй ждёт, после Release получает сервер [C]; сервер добавлен во время поиска → назначен через опрос [C]; Competitive из двух клиентов: A `MATCHED 1/2`, B → оба `WAITING_ACCEPT` на одном сервере, `accept=1` [C]; Wingman `MATCHED 1/4` [C]; **рестарт backend посреди поиска** — ретраи, «reachable again», поиск пережил рестарт (БД) и получил сервер [C]; **потеря поиска (404)** → повторная регистрация [C]; повторный поиск того же аккаунта → тот же сервер, старый матч `ENDED` [C]; сериализация результата GC-потоку (`roundtrip=ok`) [C]. **Реальные записи реестра** (192.168.1.150:27016 casual de_dust2, :27017 competitive de_dust2, :27018 wingman de_lake, :27019 dangerzone dz_blacksite): Casual → 27016, Competitive → 27017 `WAITING_ACCEPT`, Wingman → 27018, DZ → 27019 [C]. Затем реальным игровым `config.txt` без подмен против backend на 192.168.1.150:8080: поиск (несуществующая карта) → `SEARCHING` → cancel [C].
- **Игровой лог пользователя 15:41 (DLL §48):** реальные `POST /search` → `HTTP 200 created` и `POST /cancel` → `cancelled:true` из `csgo.exe` с `game_type=520`, `maps=[de_dust2]`, AccountID 1050166997 [C] — интеграция §48 работает в игре; в том прогоне srcds не отвечал (0x21 без 0x25), пользователь отменил вручную (9102 через 7,9 и 13,9 с).

### 49.7 Артефакты и состояние окружения
- **DLL:** `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll`, 5 795 840 байт, 2026-09-19 16:12, SHA-256 `2139de910a4079f6621b1a4c4ead2a854d9606652fbe301d3f5227499e9da296`; 0 ошибок / 0 предупреждений в нашем коде. Сборка: `D:\csgo2021_gc\build_local.bat`. **В игру не копировалась** (в игровой папке — DLL §48 от 15:03).
- **Backend:** jar `java-backend\target\matchmaking-backend.jar` (пересобран 16:31), запущен на **192.168.1.150:8080** с `config\application.properties` пользователя (свои логин/пароль/ключ `test-api`), БД `java-backend\data\matchmaking.db` (мигрирована на месте). В реестре четыре сервера выше (все AVAILABLE, enabled), поисков нет. Запуск/остановка: `run.cmd`; лог этого экземпляра — `java-backend\data\backend-out.log`.
- **Изменён `java-backend\config\application.properties` (файл пользователя, в `.gitignore`):** добавлены `backend.matcher-interval`, `server-reservation-ttl`, `assigned-search-timeout` и **`backend.required-players.competitive/wingman/dangerzone=1`** (тестовая среда: недостающих участников даёт ростер srcds; без этого Accept-режимы ждали бы 10/4/16 реальных игроков; строки удалить, когда матчи начнут собираться из реальных игроков).
- Тестовые заявки/матчи и состояния серверов в БД пользователя после проверок очищены (прямым SQL при остановленном backend; серверы возвращены в AVAILABLE).

### 49.8 Известные ограничения / нерешённое
1. **Accept-режимы остаются полутестовыми:** реальные игроки собираются на Java, но сам ростер на srcds по-прежнему строит тестовый `FakeRoster` (1 реальный + fake) и srcds должен быть запущен с `-gc_mode` и на порту из реестра; backend **не передаёт srcds ростер реальных AccountID** и не резервирует сервер сам (нужен канал backend → srcds; следующий этап). Известное ограничение «первая попытка Accept после старта srcds проваливается (`awaiting=127`)» не менялось (§46.9 п.1); правило «новый поиск» делает повтор мгновенным.
2. **Cookie резервации** — по-прежнему константа GC (`GameServerCookieId`), reservation lifecycle §34 не решался; в `assignment` reservation-данных нет. Сервер на backend освобождается по TTL/«новому поиску»/админу — **heartbeat srcds о конце матча не реализован** (API `POST /api/v1/servers/state` готов, вызывающей стороны нет).
3. Пока поиск ждёт сервер, клиентский UI «ищет» без 9104; поведение UI при долгом ожидании и при завершении поиска со стороны backend (admin End / timeout) в игре не проверено [?].
4. Клиент шлёт 9102 при провале Accept/подключения (§ 919 findings); шлёт ли при успешном подключении — не подтверждено [?], поэтому cancel назначенной заявки сервер не освобождает.
5. Skirmish (карты не раскодируются) → `maps[]` пуст → подходит любой сервер категории; Cooperative/ScrimComp5v5 вне MVP (backend отвечает 400).
6. Danger Zone: карта берётся из `maps[]` 9101 (`mapMask`), отдельной механики смены карты нет.
7. Адрес fake-участников srcds (`-ip` или loopback) не проверен на реальном движке [?]. Один srcds = один сервер реестра = один матч; `G`-резервация srcds одноразова (§34).
8. Linux/macOS-ветки сокетов и `platform_unix` не компилировались; `http://` без TLS, ключ API в `config.txt` открытым текстом.

### §49 STATUS / NEXT STEP
Backend выбирает сервер, клиент получает его опросом и ведёт существующий 9107/Accept/QueueConnect; клиентский выбор удалён. Дальше: (1) пользователь копирует DLL и проверяет **Casual end-to-end в игре** (srcds на 192.168.1.150:27016 запущен, backend работает): в `gc_log.txt` ожидаются `[BACKEND] POST …/search`, `search …: READY_TO_CONNECT -> 192.168.1.150:27016`, `[MM] backend assigned match …`, затем обычный 9107/`connect`; (2) затем Accept-режимы через backend (srcds с `-gc_mode`); (3) следующие этапы — канал backend → srcds (реальный ростер и резервация на матч, cookie на матч), heartbeat srcds о конце матча.


---

## 50. АУДИТ ЖИВОГО ТЕСТА §49 (READ-ONLY): Accept-режимы, fake roster, map groups, семантика RESERVED — что подтверждено, что нет, что дальше

**Статус:** только аудит по логам и коду, **ничего не менялось** (исходники, конфиги, Git). Реализация — только после подтверждения пользователя. Раздел — главный persistent checkpoint на случай потери контекста. Метки: **CONFIRMED** (прямое свидетельство в логах/коде), **HIGH CONFIDENCE**, **HYPOTHESIS**, **UNRESOLVED**. Дополняет §49; где §49 утверждал иное (49.8 п.1, 49.4 «RESERVED» для всех режимов), действует §50.

### 50.1 Источники
- `…\csgo legacy\csgo\console.log` — накопительный, содержит GC-строки клиента (`[GC] …`). Сессия §49 = строки ~12147…14149 (первый `[BACKEND] enabled: the backend at … picks the server` — стр. 12147). `gc_log.txt` — только последняя сессия (Casual/DM), `[FAKE-MM]` и вообще вывода srcds **на диске нет** (консоль srcds не сохраняется; запущенных srcds на момент аудита не было).
- `java-backend\data\backend-out.log` (решения matcher'а с 16:31 до 17:09), `csgo\gamemodes.txt` (разбор настоящим парсером KeyValues, скрипт `scratchpad\kv_gamemodes.py`), код (`SearchService.java`, `gc_client.cpp`, `test_accept.cpp`, `gc_server.cpp`, `config.cpp`, `mm_modes.cpp`).
- Оговорка: содержимое консоли srcds недоступно, поэтому всё, что происходит на srcds, выведено из ответов `0x25`, которые видит клиент.

### 50.2 Competitive / Wingman / Danger Zone: «Confirming match», окна Match Found нет
**Наблюдение пользователя (CONFIRMED им):** backend находит сервер, сервер резервируется, клиент доходит до `Confirming match`, официальный Match Found/Accept не появляется, игра висит.

**Цепочка клиента (CONFIRMED по console.log):**
1. `9101` → `[MM] mode=… maps…` → `search … reported to the backend` → `[BACKEND] search …: WAITING_ACCEPT -> 192.168.1.150:27017 map=de_dust2 match=m-5a25e651` → `[MM] backend assigned match …` (стр. 12336–12340). **Assignment доходит до `StartServerFlow`** (в каждой попытке).
2. `[MM-DIAG] GC->client 9107 #1 SENT: reservation=yes, reservation.game_type=268456456 (&0xF=8) reservationid=293a206f6c6c6548 map=de_dust2 direct_udp=192.168.1.150:27017 server_address=192.168.1.150:27017` (serverid=1 задаётся в коде; `game_mode`, `account_ids`, `match_id` в 9107 **не передаются и никогда не передавались**). Wingman: `game_type=2058 (&0xF=10)`, 27018, `de_lake`; DZ: `game_type=4877 (&0xF=13)`, 27019, `dz_blacksite`. **9107 отправляется и принимается:** `client RETRIEVED GC message … (9107)` → `client.dll 9107 handler ENTER (#1)` → `reservation callback CREATED: stage=1 mode=0 (accept / ready-up variant)` → `9107 handler EXIT … game_type=8|10|13`. Структура и значения идентичны рабочему §46 (стр. 5263–5280: `reservation=yes, game_type=520`), различие только в сыром `game_type` (другой mapMask; клиент использует `&0xF`). `sub_103DC4B0` = `ClientJob_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve` (§17/§18 findings), напрямую не хукается; факт выполнения подтверждён цепочкой ENTER/CREATED/EXIT нижестоящего хендлера `sub_103F49F0`.
3. **Accept-ветка выбирается для 8/10/13 и создаёт callback (stage 1, mode 0).** Дальше попап зависит только от `0x25 stage=1 awaiting=0` от srcds (в рабочем §46 через 13 мс после этого — `PlaySoundEffect('popup_accept_match_found')`, стр. 5275–5280).
4. **`ServerReserved` / Match Found popup не происходит ни в одной accept-попытке §49** (нет `popup_accept_match_found`). Оговорка: хук `ServerReserved sub_103FB5C0` не ставится («unexpected prologue»), поэтому судим по звуку — прокси, не прямое наблюдение.
5. Клиентские поля и путь **не отличаются от рабочего §46** → клиент после assignment не сломан; обрыв — ответ srcds.

**Три сигнатуры ответа srcds (все CONFIRMED):**
| Сигнатура | Примеры (console.log) | Что происходит у клиента |
|---|---|---|
| **нет ни одного `0x25`** (srcds не слушает порт / не отвечает) | Competitive a4–a7 на 27017 (9 с и 21 с, стр. 12341–12410); DZ a13 на 27019 (стр. ~12764, `state=3` через 21,6 с) | `0x21 stage 1` раз в секунду, затем таймаут 21 с → `callback ENTER state=3` → callback уничтожается |
| **`awaiting=127 total=0`** (ростер не собран; известное ограничение §46.9 п.1: первая попытка после старта srcds) | Competitive a8, Wingman a12 (27018, стр. 12736+), DZ a14 | немедленно `state=3` → callback уничтожен → UI висит на «Confirming match» до ручной отмены |
| **`awaiting=N-1 total=N` неизменно 20+ с** | Competitive `9/10` (a9, a10, a11: 0x25 раз в секунду до +20 с), DZ `15/16` (a15) | `state=0`, попап не поднимается (нужен `awaiting=0`) |

**Третья сигнатура — главная:** ростер существует (`total` верный: 10/16), реальный игрок на stage 1, но **fake-участники не переходят в stage 1**, поэтому `awaiting` не падает до 0. **Рабочий §46 имел `awaiting=0`** через ~60 мс после 9107 (fake уже были на stage 1 к приходу игрока).

**Что изменилось на стороне srcds между рабочим §46 и §49 (по коду, CONFIRMED как факт изменения):**
- `params.serverAddress` (куда fake-пакеты шлют 0x21): было `matchmaking.test_server_address` (192.168.1.150 в конфиге пользователя) → стало `GCConfig::DedicatedServerAddress()` = значение `-ip` из командной строки srcds, **иначе `127.0.0.1`** (`config.cpp`, `gc_server.cpp CreateTestRoster`). Порт: `-port`, иначе теперь 27015 (раньше `test_server_port`). `TestRealAccountId` override удалён, задержка accept — константа 2500 мс (на fake-пробы stage 1 не влияют).
- **HIGH CONFIDENCE:** смена адреса fake-пакетов — причина регрессии «fake не доходят до stage 1» (единственное изменение fake-пути; поведение до/после различается ровно по признаку из таблицы). **Конкретная причина — HYPOTHESIS:** srcds запущен с `+ip` (а не `-ip`) или привязан к конкретному LAN-адресу, и loopback не доходит; `-ip 0.0.0.0` (sendto на 0.0.0.0 не работает); srcds запущен со старой DLL. Проверка: строки `[FAKE-MM] player=fake-NN … sent / TIMEOUT` в консоли srcds и его командная строка.
- Побочные наблюдения драйвера (`test_accept.cpp Run`): каждый fake отправляет stage 1 не более 5 раз с паузой 1,5 с, потом `TIMEOUT` и через 30 с полный re-arm (Unreserve, 12 с cooldown) — при недоходящих пакетах это объясняет «застрявшее N-1» и периодические сбросы ростера.
- Реестр против реальных srcds: порты §49 (27017 competitive, 27018 wingman, 27019 dangerzone) — из комментариев старого конфига; ответы srcds (total=10 на 27017, total=16 на 27019) соответствуют реестру. Wingman на 27018 в сессии дал только `awaiting=127`.

### 50.3 Fake roster: как сейчас и целевая модель
**Сейчас (CONFIRMED по коду):**
- Roster создаётся **локально на srcds**: `-gc_mode <mode>` → `ServerGC::StartAcceptTestRoster` (размер = таблица `mm_modes.cpp`: 10/4/16) → ждёт первый `0x21` реального игрока (AccountID из `ISteamUser` клиента, хук `WSARecvFrom`) → `CreateTestRoster` → `FakeRoster` шлёт `Q<cookie>,<cookie>,1:[real][fake…]` через host-event и сам гонит UDP-пробы fake (`0xFA4E0000+i`) stage 1 → stage 2.
- **Java Match не знает fake participants:** в `matchmaking_match` нет игроков-fake, в `assignment` нет `account_ids`/roster; из модели известны только сервер, карта, `required_players`. `match_id` до srcds не доходит (в `Q`-payload matchid = cookie). Клиент и srcds связаны только константой cookie.
- **`backend.required-players.competitive/wingman/dangerzone=1` — временный backend override** (тестовая среда; в `application.properties` пользователя): backend собирает матч из 1 реального игрока, недостающих 9/3/15 «даёт» srcds. Это сознательный костыль, не итоговая модель.
- Почему после переноса выбора сервера на backend Accept не даёт `awaiting=0` — 50.2 (регресс адреса, HIGH CONFIDENCE) плюс архитектурные причины: ленивый arm по sniff (первая попытка всегда `awaiting=127`), ростер и таймеры srcds не связаны с жизненным циклом матча в backend, число fake задаётся командной строкой srcds.

**Итоговая модель (целевая, НЕ реализуется сейчас):** различать **реальных игроков матча** и **полный roster сервера** (`roster_size = required_players режима`, `real = участники-заявки`, `fake = roster_size − real`); fake-участники — записи матча в Java (`match_player(match_id, account_id, fake)`, детерминированные id), попадают в reservation roster **именно этого матча**; srcds получает roster от backend (канал srcds → backend), а не через sniff; assignment для accept-режимов отдаётся клиенту после подтверждения srcds «roster собран» (устраняет `awaiting=127` первой попытки). Настоящие GC-клиенты для fake — не нужны.
- **Решение пользователя:** **пока НЕ переносить roster в Java. Сначала восстановить рабочий §46-flow** (fake на stage 1 → `awaiting=0` → попап), затем — модель выше.

### 50.4 Map groups
**Источник истины (CONFIRMED): `csgo\gamemodes.txt`.** `mapgroupsMP` по режимам: casual и deathmatch — `mg_casualsigma, mg_casualdelta, mg_dust247, mg_hostage`; competitive — `mg_lobby_mapveto` + одиночные `mg_de_*`/`mg_cs_*`; scrimcomp2v2 (wingman) — `mg_de_ravine, …, mg_de_lake`; gungameprogressive (Arms Race) — `mg_armsrace`; **gungametrbomb (Demolition) — только `mg_demolition`**; skirmish — `mg_skirmish_armsrace/demolition/flyingscoutsman/retakes`; survival (DZ) — `mg_dz_sirocco`. Состав групп: `mg_casualsigma` = {de_basalt, de_ancient, de_vertigo, de_cbble, de_canals}; `mg_casualdelta` = {de_mirage, de_inferno, de_overpass, de_nuke, de_train, de_cache}; `mg_hostage` = {cs_insertion2, cs_agency, cs_militia, cs_office, cs_italy, cs_assault}; `mg_dust247` = {de_dust2}; **`mg_demolition` = {de_lake, de_stmarc, de_sugarcane, de_bank, de_safehouse, de_shortdust}**; `mg_armsrace` = {de_lake, de_stmarc, de_safehouse, ar_shoots, ar_baggage, ar_lunacy, ar_monastery}.
- **`maps[]` от клиента корректен (CONFIRMED):** все наблюдавшиеся 9101 декодируются ровно в состав соответствующих `mg_*`: Casual/Delta `269530119` (mask `0x1010b4`) → {mirage, inferno, cache, train, overpass, nuke}; DM/Sigma `1677740038` (mask `0x640048`) → {cbble, vertigo, canals, ancient, basalt}; DM/Hostage `151977990` (`0x90f00`); Casual/DM Dust 24/7 (`0x2`) → {de_dust2}; Competitive `268456456` (`0x100052`) → {dust2, inferno, vertigo, overpass}; Wingman `2058` (`0x8`) → {de_lake}; DZ `4877` (`0x13`) → {blacksite, sirocco, county}. Backend получает те же списки (лог backend).
- **Sigma = `mg_casualsigma`, относится к Casual/Deathmatch, не к Demolition. У Demolition — `mg_demolition`.** Комбинации «Demolition + Sigma» в `gamemodes.txt` нет.
- **Matcher корректен (CONFIRMED по коду и логам):** `mapCompatible(maps[], server.map)`: maps пуст → любой сервер; иначе `server.map ∈ maps[]` (несколько карт — достаточно одной); сервер без карты подходит только заявке без карт. Все assignment в логе удовлетворяют правилу. `assignment.map` = метка `server.map` реестра (не запрошенная карта).
- **Почему сейчас всегда `de_dust2` (CONFIRMED):** все успешные подключения — `Map: de_dust2` на 27016 и 27014 (реальные srcds запущены на de_dust2), а метка реестра — ручное поле админки. **Backend не управляет реальной картой srcds:** GC→srcds сообщения с картой нет (`CMsgGCCStrike15_v2_MatchmakingGC2ServerReserve` есть в proto, в C++ не используется), srcds карту от backend не получает и о ней не сообщает (heartbeat не реализован). Клиент карту 9107 **не валидирует**: в старом Wingman-прогоне `9107 map=de_lake` → `Connecting to …:27016` → `Map: de_dust2` (console.log 6064/6097/6103). Итог: registry label ≠ гарантия реальной карты.
- **Отказы подключения при метке ≠ реальной карте (CONFIRMED факт, HYPOTHESIS причина):** Casual/Delta → метка 27016 изменена админом на `de_inferno` (лог backend стр. 138) → assignment → клиент шлёт `0x21 stage 2` 21 с, **ни одного `0x25`** (успешный Casual `de_dust2` получал `0x25 stage=2 awaiting=0` за 20 мс, стр. 12922) → `state=3`, «failed to connect to the match»; DM/Hostage → метка 27014 → `cs_agency` (стр. 153) → то же (стр. 14096+). Совпадает по времени с правками карты в админке. HYPOTHESIS: пользователь менял карту на самом srcds (`changelevel`), сервер молчал во время загрузки; проверка — консоль srcds. Клиентская логика и matcher в этих отказах не виноваты.
- **Demolition / Arms Race (UNRESOLVED, исследовать по новым живым логам):** во всех сессиях `console.log` **нет ни одного 9101 с `eGame=4` или `5`**; backend-лог не содержит `mode=demolition|armsrace`. Из нестандартных режимов клиент присылал только Skirmish (`eGame=12`): `game_type` 131084 / 393228 / 524300, маски `0x200 / 0x600 / 0x800` — не декодируются (`maps=[]`), skirmish-серверов в реестре нет, поиски ждали и были отменены; гипотеза «Skirmish = `1<<(index-1)` по 4 группам `mapgroupsMP`» (§44.5) **не сходится** с наблюдаемыми битами (9, 9+10, 11) [UNRESOLVED]. Assignment для Demolition в логах отсутствует → причину не придумываем: нужны новые живые логи с явным указанием пункта UI (в т.ч. War Games/Skirmish → Demolition) и, при необходимости, RE ветки Skirmish `sub_10288A90`.

### 50.5 Семантика RESERVED (Java)
**Факт (CONFIRMED по коду `SearchService.java`):** `place()` (стр. 248–273) → `createMatch()` (стр. 303–316) безусловно вызывает `servers.reserve(picked.server().id(), id, now)` (`GameServerRepository.reserve`, стр. 88: `UPDATE … SET state='RESERVED' … WHERE state='AVAILABLE'`). **Ветвления по `acceptRequired` нет; RESERVED ставится одинаково для всех режимов, уже во время `POST /search`.** Различие classic/accept есть только в `join()` (стр. 318–336): при `required_players == 1` (классика) матч сразу `READY`/`READY_TO_CONNECT`, для accept — `FORMING` до сбора игроков.

**Почему это неправильно:** для Casual / Deathmatch / Arms Race / Demolition / Skirmish backend лишь **находит подходящий AVAILABLE сервер и отдаёт assignment**, затем C++ делает обычный `connect`; закреплять сервер за матчем из-за поиска не нужно (Casual-сервер вмещает до 20, несколько игроков должны попадать на него). Текущее поведение: пока первый игрок в Casual, сервер RESERVED и второй игрок его не получает; после обычного connect backend ничего не узнаёт — RESERVED держится до TTL 10 мин, нового поиска **того же** аккаунта или ручного Release (в логе: DM-матч `m-62f0cb98` на 27014 освободился только по TTL в 17:09; повторные Casual работали благодаря правилу «новый поиск», §49.4, но не для других игроков).
**Ответы на вопросы аудита:** (1) да, одинаково для всех режимов; (2) в `place()`/`createMatch()` при `POST /search`, до подтверждения чего-либо; (3) различие только в `join()`, не в резервировании; (4) минимальное место разделения — `createMatch()`/`place()`: для `!acceptRequired` не вызывать `reserve` (обновлять `last_assigned_at` для распределения нагрузки), для accept оставить; (5) текущее RESERVED для классики ломает параллельные поиски и блокирует сервер до TTL, повтор одного аккаунта маскируется правилом «новый поиск»; (6) после обычного connect сервер остаётся RESERVED (см. выше), srcds эти состояния не видит — RESERVED в Java не связан с cookie/резервацией на самом srcds.
**Целевая семантика:** reservation — только часть Accept-required flow (Competitive/Wingman/DZ: сервер закреплён за матчем и участниками до Accept/QueueConnect). После non-accept assignment сервер **не** остаётся RESERVED из-за поиска. TTL/правило «новый поиск» — только для accept-матчей.

### 50.6 Единый вывод
- **Проблема A (CONFIRMED, независима):** неверная семантика reservation для non-accept (50.5).
- **Проблема B (CONFIRMED, независима):** Accept ломается на стороне server reservation/roster **до `awaiting=0`** (50.2): нет ответа / `awaiting=127` / `awaiting=N-1`; клиентская цепочка после assignment исправна.
- **Проблема C (отдельная):** map — `registry label` не гарантирует реальную карту srcds, backend ею не управляет (50.4).
- Общий корень B, C и хвоста A — **нет канала backend ↔ srcds** (нет реальной карты, состава roster, конца матча, heartbeat); matcher оперирует ручными метками.

### 50.7 Сводка достоверности
**CONFIRMED:** assignment доходит до `StartServerFlow`; 9107 #1 отправляется с корректными полями и принимается клиентом; accept-ветка создаёт callback (stage 1, mode 0) для 8/10/13; в accept-попытках §49 нет Match Found/`popup_accept_match_found`; srcds отвечает `awaiting=127`, либо `awaiting=N-1` (20+ с), либо не отвечает; при `N-1` roster существует, fake не доходят до stage 1; рабочий §46 имел `awaiting=0` за ~60 мс; fake roster создаётся локально на srcds, Java Match его не знает; `required_players=1` — временный override; `maps[]` корректен и совпадает с `gamemodes.txt`; Sigma = `mg_casualsigma` только у Casual/DM; Demolition = `mg_demolition`; matcher сравнивает `server.map ∈ maps[]` правильно; `de_dust2` — реальная карта запущенных srcds; backend не управляет реальной картой srcds; клиент не валидирует `map` 9107; `createMatch/place()` безусловно ставит RESERVED для всех режимов; после non-accept assignment сервер остаётся RESERVED; в живых логах нет 9101 для Demolition/Arms Race (только skirmish, не декодируется).
**HIGH CONFIDENCE:** изменение адреса fake-пакетов (`test_server_address` → `-ip`/`127.0.0.1`) — причина регрессии между §46 и §49.
**HYPOTHESIS:** конкретный механизм (srcds с `+ip` / привязка к LAN-IP / `-ip 0.0.0.0` / старая DLL); молчание srcds на `0x21 stage 2` при смене метки карты = `changelevel`/загрузка карты; «failed connect» Casual/DM с изменённой меткой — следствие этого молчания.
**UNRESOLVED:** Demolition/Arms Race/War Games (нужны живые логи и RE Skirmish); расшифровка масок skirmish; консоль srcds (`[FAKE-MM]`) и его командная строка.

### 50.8 План (НИЧЕГО не реализовано, ждёт подтверждения пользователя)
1. **Восстановить рабочий §46-flow Accept** (без переноса roster в Java): получить `[FAKE-MM]` и командную строку srcds; вернуть fake-пакетам адрес, реально слушаемый srcds (при `-ip` он же, иначе основной IPv4 машины, не loopback; лог выбранного адреса), проверить `awaiting=0`/попап для Competitive, Wingman, DZ.
2. **Разделить reservation по семантике (Java):** `createMatch/place()` — RESERVED только для `acceptRequired`; классика — AVAILABLE остаётся, assignment без закрепления; TTL и правило «новый поиск» — только для accept; тесты (параллельные Casual-поиски на один сервер, повтор, accept-резервирование).
3. **Только затем** — модель «real players vs полный roster» и roster в Java (`match_player`, fake-флаг), канал srcds → backend (heartbeat/pending-match/ack), после ack — assignment для accept.
4. **Карта:** канал backend → srcds (`changelevel`/отчёт реальной карты), чтобы метка реестра отражала реальность; до этого метка = ручное поле.
5. **Demolition/Arms Race/Skirmish:** новые живые логи (указать пункт UI), при необходимости RE Skirmish-ветки `sub_10288A90`; ничего не хардкодить.

### §50 STATUS
Аудит записан, код не менялся. Следующий шаг — подтверждение пользователя и пункт 1 плана (восстановление §46-flow); для него нужны консоль srcds (`[FAKE-MM]`) и командная строка srcds.



---

## 51. АУДИТ ПЕРЕД ИСПРАВЛЕНИЯМИ (READ-ONLY): точка изменения reservation в Java и сравнение fake roster §46 ↔ §49

**Статус:** только чтение кода/логов, **исходники, конфиги и Git не менялись** (изменён только этот файл). Дополняет §50; не отменяет его. Задача пользователя после §50: две минимальные правки — (1) reservation только для Accept-required, (2) регресс fake roster; roster в Java, `match_player`, heartbeat, backend→srcds, `changelevel`, новый reservation protocol и удаление fake-driver **пока не делаются**.

### 51.1 Проверка
- §50 на месте (`## 50.` — стр. 4310, `### §50 STATUS` — стр. 4389). Состояние исходников: `git status` (только чтение) — изменены `csgo_gc/{CMakeLists,config,gc_client,gc_server,gc_shared}.*`, `examples/config.txt`, новые `backend_client.*` и `java-backend/`; **`test_accept.*`, `mm_modes.*`, `main.cpp` не менялись относительно HEAD**.

### 51.2 Java: где `AVAILABLE → RESERVED` и минимальная правка [CONFIRMED по коду]
- **Единственное место:** `SearchService.createMatch()` (`java-backend/.../search/SearchService.java`, стр. 303–316): `if (!servers.reserve(picked.server().id(), id, now)) return null;` (стр. 306) → `GameServerRepository.reserve` (стр. 88–93): `UPDATE game_server SET state='RESERVED', reserved_match_id=?, reserved_at=?, last_assigned_at=?, … WHERE id=? AND state='AVAILABLE'`. Вызывается из `place()` (стр. 248–273) после `pickServer()` (стр. 279–290, фильтр `findFree`: `enabled AND state='AVAILABLE' AND category`, порядок `COALESCE(last_assigned_at,0), id`, затем `mapCompatible`). `join()` (318–336) различает режимы только по `required_players` (классика = 1 → сразу `READY`/`READY_TO_CONNECT`); ветки по `acceptRequired` при резервировании **нет** (лог стр. 313 всегда пишет «server … is RESERVED»).
- **Минимальное изменение (не реализовано):**
  1. `GameServerRepository`: новый метод `markAssigned(id, now)` — `UPDATE game_server SET last_assigned_at=?, updated_at=? WHERE id=?` (состояние не трогает; нужен для распределения нагрузки, т.к. порядок `pickServer` зависит от `last_assigned_at`).
  2. `createMatch()`: `if (category.acceptRequired()) { if (!servers.reserve(...)) return null; } else { servers.markAssigned(...); }`; сообщение в логе — «RESERVED» только для accept, иначе «assigned (no reservation)».
  3. Javadoc класса (стр. 27), README «Подбор сервера», §49-пояснение — синхронизировать.
- **Остальной код не требует правок (проверено):** `expire()` (TTL матчей — конец `matchmaking_match` READY, состояние сервера меняется только если `ready.id().equals(server.reservedMatchId())` — для классики no-op), `releaseAbandonedMatches` (то же условие), `freeServer`/`dissolve` (то же условие или `reservedMatchId()==null` → return), админ-«Release» на не-RESERVED сервере ничего не ломает. Accept-путь (`join` к FORMING, `dissolve`, TTL, правило «новый поиск») не меняется.
- **Побочные последствия, которые надо знать:** классический сервер перестаёт быть эксклюзивным (несколько поисков получают один и тот же `AVAILABLE` сервер; распределение — по давности `last_assigned_at`). **Вместимость (`max_players`) backend не учитывает** (учёта занятости нет; это следующий этап вместе с heartbeat srcds) — оставить как известное ограничение. `matchmaking_match` для классики по-прежнему создаётся (нужен `match_id` в assignment) и закрывается по TTL.
- **Java-тесты, кодирующие старую семантику (потребуют правки при реализации; `MatchmakingIntegrationTest`):** `classicSearchGetsTheAvailableServerAndItIsReserved` (174: `RESERVED` + `reserved_match_id`), `disabledAndBusyServersAreNotUsed` (235: второй поиск теперь тоже получит сервер), `twoSearchesNeverShareOneServer` (249), `twoServersServeTwoSearches…` (263: третий поиск не `SEARCHING`), `cancelBeforeAServerIsFound…` (273: `RESERVED`), `aPlayerWhoSearchesAgain…` (286, финальный `SEARCHING` стр. 302), `anAssignedServerComesBackAfterTheReservationTtl…` (318, `RESERVED` стр. 324), `aGameServerCanReportItsState…` (428, блок 450–453 — перенести на accept-сервер), `editingAServerNeverChangesItsState` (467, `RESERVED` стр. 474 — перенести на accept). Accept-тесты (306, 341, 371, 383, 400, 457) не меняются. Нужны новые: «классика не резервирует (state остаётся AVAILABLE, `reserved_match_id` пуст)», «два Casual-поиска получают один сервер», «классика распределяется по `last_assigned_at`», «Accept по-прежнему RESERVED и эксклюзивен».

### 51.3 Fake roster: сравнение §46 ↔ §49 [CONFIRMED факты, механизм — HYPOTHESIS]
- **Весь fake-путь на srcds (`test_accept.cpp` — драйвер, sniff `WSARecvFrom`, `BuildQueuedReservationPayload`, тайминги 150 мс / 1,5 с / 5 попыток / 30 с / 12 с; `mm_modes.cpp` — размеры 10/4/16; `StartAcceptTestRoster`/`OnTestRealPlayerSeen`/`CreateTestRoster` в `gc_server.cpp`) не менялся**, кроме двух вещей: (a) удалён override `test_real_account_id` (в конфиге пользователя его и не было — AccountID и в §46 определялся sniff'ом, различия нет); (b) **`params.serverAddress`: `config.TestServerAddress()` → `config.DedicatedServerAddress()`** и `params.serverPort` (при `-port` без изменений).
- **Что было в §46:** `matchmaking.test_server_address` = `192.168.1.150` (`config.txt.bak_pre_backend`, стр. 39) → fake-сокет шлёт 0x21 на `192.168.1.150:<port>`, исходящий IP fake = `192.168.1.150` = тот же IP, что у реального клиента (так и описано в §44.4: «клиент и fake шлют с ОДНОГО IP»).
- **Что стало в §49:** `DedicatedServerAddress()` = значение `-ip` командной строки srcds, иначе `127.0.0.1`. **Во всех задокументированных командах запуска srcds (комментарий старого `config.txt`) `-ip` нет** (`srcds.exe -game csgo -console -port 27017 -gc_mode competitive +game_type 0 +game_mode 1 +map de_dust2` и т.д.), значит при этих командах fake шлют на **`127.0.0.1:<port>`**, а исходящий IP fake — `127.0.0.1` (клиент по-прежнему с `192.168.1.150`). Точная командная строка srcds пользователя не известна [?]; **проверяется одной строкой консоли srcds:** `[FAKE-MM] started: … server=<адрес>:<порт>`.
- **Ключевое:** в findings нет ни одного живого прогона fake-драйвера на loopback против реального `engine.dll` (§42 п.(c) «сервер принимает 0x21 … из loopback/LAN-адреса» помечен [?]); подтверждено (§46, `awaiting=0` за ~60 мс) только с адресом `192.168.1.150`. Совпадает с наблюдаемой сигнатурой §50 (`awaiting=N-1`: ростер есть, реальный игрок на stage 1, fake до stage 1 не доходят; `TIMEOUT` fake → re-arm 30 с).
- **Уровень достоверности:** адрес — единственное различие в fake-пути → **HIGH CONFIDENCE** (как в §50). **Механизм не доказан [HYPOTHESIS]:** почему движок не принимает/не засчитывает loopback-источник (обычно srcds без `-ip` слушает все интерфейсы, и 127.0.0.1 должен доходить) — возможно, engine.dll особо обращается с `127.0.0.1` как отправителем connectionless-пакетов, либо srcds привязан к конкретному адресу (`+ip`), либо иное; без консоли srcds и без эксперимента отличить нельзя. Альтернативные причины N-1 на не-fake стороне (rate limit §44.4) маловероятны: fake теперь шлют с другого IP, чем клиент.

### 51.4 Как восстановить рабочий §46-flow (варианты, НЕ реализовано)
- **Шаг 0, без кода и без пересборки:** перезапустить каждый Accept-srcds с `-ip 192.168.1.150` (`DedicatedServerAddress()` тогда = `192.168.1.150`, ровно как `test_server_address` в §46; реестр backend тоже использует `192.168.1.150`), проверить `awaiting=0`/попап для Competitive, Wingman, DZ. Если помогло — регресс **CONFIRMED** и механизм «loopback-источник fake не засчитывается».
- **Код (после шага 0, по решению пользователя):** `DedicatedServerAddress()` — порядок: `-ip` → (новое) адрес, по которому реальный клиент достучался до srcds: `from` первого sniffed 0x21 реального игрока (в Hk_WSARecvFrom доступен), принимается только если он **локальный адрес этой машины** (проверка `bind()` UDP-сокета на него) → иначе `127.0.0.1` + предупреждение; логировать выбранный адрес в `[MM-ACCEPT]`/`[FAKE-MM] started`. Без нового ключа в `config.txt`, без возврата `test_server_address`. Альтернатива (проще, но требует ручной опции): srcds-only ключ командной строки `-gc_fake_target <ip>`.
- **Не трогать:** тайминги драйвера, payload `Q`, sniff, mm_modes, Accept-код клиента.

### §51 STATUS
Аудит записан, код не менялся. Порядок при подтверждении: (1) Java: reservation только для accept (51.2) + тесты; (2) шаг 0 (`-ip` на srcds) → результат теста Competitive/Wingman/DZ; (3) при подтверждении — правка `DedicatedServerAddress()` (51.4). Не делаем: roster в Java, `match_player`, heartbeat, backend→srcds, `changelevel`, новый reservation protocol, Panorama popup, удаление fake-driver.



---

## 52. ИНСТРУКЦИЯ РУЧНОГО ТЕСТА «`-ip 192.168.1.150` на Accept-srcds» (без изменений кода/конфигов) и уточнения к §50/§51

**Статус:** код, конфиги и Git не менялись; тест выполняет пользователь (он запускает игру и srcds сам), результаты принесёт в чате. Цель — превратить HIGH CONFIDENCE §51.3 (адрес fake-пакетов `127.0.0.1` вместо `192.168.1.150`) в CONFIRMED или опровергнуть. До результата **не** делаются: Java-правка reservation, roster в Java, `match_player`, heartbeat, backend→srcds, `changelevel`, новый reservation protocol, Panorama, удаление fake-driver.

### 52.1 Что уточнено при подготовке [CONFIRMED по логам/коду]
1. **§46-поведение включало `awaiting=127` на ПЕРВОЙ попытке** (ленивое взведение ростера по sniff'у 0x21): console.log 8234–8262 (DZ на 27018: попытка a1 `awaiting=127 total=0`, попытка a2 через ~20 с `stage=1 awaiting=0 total=16` за 57 мс → попап). Поэтому `awaiting=127` на первой попытке после старта srcds — **не** признак регресса; регресс — если fake **не доходят до stage 1 и после этого** (`awaiting=N-1` / `TIMEOUT`). Тест поэтому = две попытки подряд на свежем srcds.
2. **Почему `[FAKE-MM]` нет на диске:** `Platform::Initialize()` (`platform_windows.cpp:15`) делает `DeleteFileA("gc_log.txt")` при старте **каждого** процесса с этим DLL (csgo.exe и каждый srcds) в его рабочей папке; `log_output "2"` (конфиг пользователя) пишет весь `Platform::Print` (включая `[MM-ACCEPT]`/`[FAKE-MM]` srcds) в `gc_log.txt` **рабочей папки процесса** (для srcds из папки игры — `…\csgo legacy\gc_log.txt`, тот же файл, что у клиента). Итог: при запуске клиента после srcds строки srcds стираются; при запуске srcds после клиента стираются старые строки клиента (они остаются в накопительном `csgo\console.log`). Правило теста: **сначала клиент, потом srcds; после каждого режима копировать `gc_log.txt`, до запуска следующего процесса.** Не использовать `-condebug` на srcds (он пишет тот же `csgo\console.log`, что и клиент).
3. **Сохранённых команд запуска srcds на диске нет** (ни .bat, ни ярлыков; запущенных srcds нет). Использованы команды из комментария старого `config.txt` (`config.txt.bak_pre_backend`) + порты реестра backend (27017 competitive / 27018 wingman / 27019 dangerzone; карты реестра de_dust2 / de_lake / dz_blacksite).
4. **Установленный DLL = сборка §49** (SHA-256 `2139DE91…A296` совпадает с `Build\release\csgo_gc\csgo_gc.dll`), значит `DedicatedServerAddress()` = `-ip`, иначе `127.0.0.1`.

### 52.2 Критерии (кратко; полный текст — в ответе пользователю)
- **Подтверждает §51:** в консоли/`gc_log.txt` srcds `[FAKE-MM] started: … server=192.168.1.150:<порт>`; после первой попытки все fake `stage=1 confirmed` и `all N fake players confirmed at stage 1`; во второй попытке у клиента `0x25 stage=1 awaiting=0 total=N` (N = 10 / 4 / 16) за <0,2 с, `popup_accept_match_found` + `RaiseReadyUp … ReadyForMatch`, видимое окно Match Found — **для каждого из трёх режимов**.
- **Опровергает:** адрес `192.168.1.150` в `[FAKE-MM] started`, а fake всё равно `TIMEOUT` / `awaiting=N-1` (причина не в адресе; дальше — привязка srcds к интерфейсу, rate limit, версия/cookie fake-пакетов).
- **Недействительный прогон:** в `[FAKE-MM] started` адрес `127.0.0.1` (`-ip` не подхвачен командной строкой), либо нет `[FAKE-MM]` вообще (ростер не взведён — srcds не видел 0x21 или не тот `-gc_mode`/порт).

### §52 STATUS
Инструкция выдана пользователю; ждём логи по Competitive / Wingman / Danger Zone. После результата — отдельное подтверждение на Java reservation split.



---

## 53. РЕЗУЛЬТАТ РУЧНОГО ТЕСТА §52 (`-ip 192.168.1.150` на Accept-srcds): полный Accept-flow работает во всех трёх режимах

**Статус:** только разбор логов пользователя; исходники, конфиги, Git не менялись. Метки: **CONFIRMED** / **HIGH CONFIDENCE** / **HYPOTHESIS** / **UNRESOLVED**.

### 53.1 Источники
- srcds (это копии общего `gc_log.txt`, в них перемешаны строки srcds и клиента): `…\csgo legacy\competive_srcds.txt` (имя как есть), `wingman_srcds.txt`, `dangerzone_srcds.txt`. Клиент: `csgo\console.log`, метки пользователя `===== …52 competitive/wingman/dangerzone =====` (стр. 15528 — первый прогон competitive, 16228 — второй, 16379, 16505, `END TEST` 16645). Backend: `java-backend\data\backend-out.log` (матчи 18:44–18:51). DLL — сборка §49 (SHA-256 `2139DE91…A296`), без изменений.

### 53.2 Результат по режимам (второй прогон competitive; wingman; dangerzone) [все CONFIRMED по логам]
| | Competitive :27017 | Wingman :27018 | Danger Zone :27019 |
|---|---|---|---|
| `[FAKE-MM] started … server=` | **192.168.1.150:27017** | **192.168.1.150:27018** | **192.168.1.150:27019** (`-ip` подхвачен) |
| roster (real + fake) | 10 = 1 + 9, `expected=actual` | 4 = 1 + 3 | 16 = 1 + 15 |
| попытка 1 (свежий srcds): `0x25 stage=1` | `awaiting=127 total=0` (+0,230 с) → ростер взведён → callback `state=3` уничтожен, попапа нет (ожидаемо, §46.9 п.1) | то же (+0,315 с) | то же (+0,365 с) |
| fake до stage 1 (после попытки 1) | все 9: `confirmed (awaiting=9…1 total=10)`, `roster total=10 expected=10 OK`, `all 9 … confirmed at stage 1` | все 3: `awaiting=3…1 total=4`, `total=4 OK` | все 15: `awaiting=15…1 total=16`, `total=16 OK` |
| ручная отмена попытки 1 | +18,1 с | +20,4 с | +20,5 с |
| попытка 2: `0x25 stage=1` | **`awaiting=0 total=10`** через 1 мс после 0x21 (+0,324 с) | **`awaiting=0 total=4`** (+0,302 с) | **`awaiting=0 total=16`** (+0,304 с) |
| `popup_accept_match_found` / `RaiseReadyUp(1,0,N)` | +0,337 / +0,344 с | +0,311 / +0,314 с | +0,324 / +0,327 с |
| stage 2 (Accept; fake по одному) | клиент 0x21 +2,885 с: `awaiting=8`; +3,896 с: `awaiting=2`; **`awaiting=0 total=10`** +4,157 с | +2,883 с: `awaiting=2`; **`awaiting=0 total=4`** +3,158 с | +2,720 с `15`; +3,711 с `9`; +4,747 с `3`; **`awaiting=0 total=16`** +5,103 с |
| «reservation fully accepted» → второй 9107 | `9107 #2 SENT: reservation=no` +4,159 с | +3,159 с | +5,104 с |
| callback stage=2 mode=2 → `popup_accept_match_confirmed` → `QueueConnectToServer RAISED` | +4,172 / +4,203 / +4,204 с | +3,173 / +3,202 / +3,203 с | +5,124 / +5,168 / +5,169 с |
| connect (console.log) | `Connecting to public(192.168.1.150:27017)`, `Connected`, `Map: de_dust2`, `Players: 1 (2 bots) / 10 humans`, `connected.`, `LOADINGSCREEN -> INGAME` | :27018, `Map: de_lake`, `Players: 1 (2 bots) / 4 humans`, INGAME | :27019, `Map: dz_blacksite`, `Players: 1 (0 bots) / 16 humans`, INGAME |
- Stage 2 начат клиентом (stage-2 0x21 ~2,7–2,9 с после попапа; нажатие Accept пользователем или автоматически — по логу не видно, пользователь знает; `RaiseReadyUp(1,k,N)` считает принявших).
- В srcds-логах на всех трёх нет ни `TIMEOUT`, ни `answered awaiting=127` у fake, ни `MISMATCH`.

### 53.3 Сравнение с рабочим §46 [CONFIRMED]
Последовательность и тайминги совпадают с §46 (console.log 5262–5300: stage 1 `awaiting=0 total=10` за ~60 мс, попап +0,07 с, stage 2 `awaiting=0` +4,2 с, второй 9107 +4,198 с; DZ на 8234–8274: первая попытка `awaiting=127 total=0`, вторая через ~20 с `awaiting=0 total=16` за 57 мс). Поведение **не отличается** от §46, включая `awaiting=127` на первой попытке свежего srcds. Значит: fake-драйвер, roster, sniff, Accept-код клиента и путь backend → `StartServerFlow` → 9107 → Accept → QueueConnect в сборке §49 **работают**, при условии, что fake-пакеты идут на LAN-адрес srcds.

### 53.4 Что это доказывает и что нет
- **CONFIRMED:** `-ip 192.168.1.150` на srcds (⇒ `DedicatedServerAddress()` = `192.168.1.150`, как `test_server_address` в §46) даёт `awaiting=0`, попап, Accept, второй 9107 и реальное подключение для Competitive, Wingman и Danger Zone; адрес в `[FAKE-MM] started` — LAN (§49.8 п.7: LAN подтверждён на реальном движке).
- **HIGH CONFIDENCE (не CONFIRMED):** регресс §49 вызван адресом fake-пакетов `127.0.0.1` (по умолчанию без `-ip`). Не хватает **контрольного прогона**: тот же srcds/DLL без `-ip` — ожидаются `[FAKE-MM] started … server=127.0.0.1:<порт>` и `TIMEOUT`/`awaiting=N-1`. Строк `[FAKE-MM] started` и командных строк srcds из прежних неудачных прогонов §49 на диске нет, поэтому «раньше было именно 127.0.0.1» напрямую не подтверждено (косвенно: в документированных командах запуска `-ip` нет, §51.3).
- **HYPOTHESIS:** механизм (движок не засчитывает fake с loopback-источника) — не проверялся.

### 53.5 Прочие наблюдения
- **[CONFIRMED] Клиент шлёт 9102 (MatchmakingStop) при успешном подключении после Accept** (~2,1 с после `QueueConnectToServer RAISED`: competitive +4,204 → +6,314; wingman +3,203 → +5,316; DZ +5,169 → +7,278), backend получает `cancel` (HTTP 200) уже назначенного поиска. Закрывает §49.8 п.4 для Accept-режимов. Backend после этого сервер не освобождает (по дизайну §49.4); в логе backend матчи закрывались только «new search» (напр. `m-3f5197d1` 18:46:42 → 18:48:49) — реального «конца матча» backend по-прежнему не знает.
- **[UNRESOLVED] Первый прогон competitive (console.log 15528+; backend `m-f26c15d4` 18:44:51, `m-6bd09645` 18:45:10): 0x21 на 192.168.1.150:27017 остались без единого 0x25** (a1 ~18 с, a2 ~11 с), затем клиент перезапущен. srcds-лога этого прогона нет, поэтому причина неизвестна (srcds не запущен/ещё грузился/другая команда). Второй прогон с тем же кодом и адресом работает; на выводы по адресу не влияет.
- **[UNRESOLVED]** `OnSessionFailed: Timed out attempting to connect` в конце каждого srcds-лога (после подключения и выхода клиента) — источник не выяснен, к reservation-flow отношения не видно.
- Одиночный «bots» в `Players:` (2 bots) — стандартные боты карты, к ростеру не относится.

### §53 STATUS
Регресс §49→§46 **практически подтверждён** (работает возврат к LAN-адресу fake), строгое доказательство — контрольный прогон без `-ip` (опционально). Дальше по решению пользователя: Java reservation split (§51.2), затем при желании — закрепление адреса fake в коде (§51.4: `-ip` → локальный адрес, по которому клиент достучался → `127.0.0.1` + предупреждение). Не делаем: roster в Java, `match_player`, heartbeat, backend→srcds, `changelevel`, новый reservation protocol, Panorama, удаление fake-driver.



---

## 54. ТРИ ПРАВКИ ПОСЛЕ §53: (A) reservation только для Accept, (B) map groups / Skirmish (Arms Race, Demolition), (C) fake players переехали на backend

**Статус:** реализовано и проверено (Java: 67 тестов, 0 ошибок; C++ собран; сквозной прогон настоящего C++-кода клиента против настоящего backend). **В игре не запускалось** [?]. Git не трогался, DLL в игру не копировалась, работающий jar пользователя (`java-backend\target\matchmaking-backend.jar`, pid 312) не пересобирался — чтобы применить, остановить backend, `build.cmd`, `run.cmd`. Не отменяет §46–§53; поправки к прежним разделам — в 54.6. Метки: **CONFIRMED** / **HIGH CONFIDENCE** / **HYPOTHESIS** / **UNRESOLVED**.

### 54.1 (A) Reservation split (Java) [CONFIRMED тестами]
- `SearchService.createMatch()`: для `acceptRequired` — `servers.reserve()` (AVAILABLE → RESERVED) как раньше; для Casual / Deathmatch / Arms Race / Demolition / Skirmish — новый `GameServerRepository.markAssigned()` (только `last_assigned_at`, состояние не меняется). Матч-запись и `match_id` в assignment остаются. Классический сервер теперь не эксклюзивен (несколько игроков на один сервер, распределение по `last_assigned_at`), BUSY — единственный способ «закрыть» его; вместимость (`max_players`) не учитывается (нужен heartbeat srcds) — известное ограничение. TTL/«новый поиск»/Release не менялись (для классики они no-op: условие `reservedMatchId == matchId`).
- Тесты, кодировавшие старую семантику, переписаны (9): reserved→«не резервируется», «два поиска не делят сервер»→«сервер не эксклюзивен», cancel/TTL/report/edit — RESERVED-проверки перенесены на Accept-серверы. Accept-тесты не менялись.

### 54.2 (B) Map groups: реальный путь данных и причины [CONFIRMED по коду клиента и данным игры]
**Путь:** UI → lobby KV (`game/mode`, `game/mapgroupname` = токены `mg_*` через запятую) → `sub_10288960` (строка режима → eGame: `gungameprogressive`→4, `gungametrbomb`→5, `deathmatch`→6, `casual`→7, `competitive`→8, `cooperative/coopmission`→9, `scrimcomp2v2`→10, `scrimcomp5v5`→11, **`skirmish`→12**, `survival`→13) → `sub_10288A90` (composer, по eGame) → `game_type = eGame | (mask << 8)` → 9101 → GC (`mm_modes`) → `maps[]` → backend.
1. **Обычные режимы (6/7/8/10/13): декодирование верное.** Сверка (`check_map_groups.py`, композер client.dll × `gamemodes.txt` × таблицы `mm_modes.cpp`): **34 группы, 0 расхождений** — каждая `mapgroupsMP`-группа режима даёт маску, которая раскодируется ровно в карты группы (`mg_casualsigma` 0x640048 = {basalt, ancient, vertigo, cbble, canals}; `mg_casualdelta` 0x1010B4; `mg_hostage` 0x90F00; `mg_dust247` 0x2; wingman/competitive — одиночные карты; DZ; `mg_lobby_mapveto` 0x20000 — флаг, не группа). Matcher сравнивает `server.map ∈ maps[]` и **не смешивает категории**. Значит Sigma «не проходит», потому что в реестре нет сервера этого режима на карте Sigma (метки реестра de_dust2 / de_inferno / …, реальная карта srcds — вручную, §50.4) — не ошибка декодирования/сопоставления. Демонстрация — `MapGroupMatchingTest`.
2. **Arms Race и Demolition — причина найдена (это и есть ответ на «почему нет `eGame=4/5`»).** В актуальном клиенте они доступны **только как War Games (Skirmish)**: eGame=12, а маска — **выбранные режимы**, не карты. Композер, ветка 12: для каждого токена с префиксом `mg_skirmish_` ищет запись по имени в `items_game.txt` `skirmish_modes` и делает `mask |= 1 << (id − 1)`. Ids (items_game): 1 stabstabzap, 2 dm_freeforall, 3 flyingscoutsman, 4 triggerdiscipline, 6 headshots, 7 huntergatherers, 8 heavyassaultsuit, **10 armsrace** (`gamemode gungameprogressive`), **11 demolition** (`gungametrbomb`), **12 retakes** (`casual`). Наблюдавшиеся в логах skirmish `game_type` 131084 / 393228 / 524300 (маски 0x200 / 0x600 / 0x800) = **Arms Race / Arms Race + Demolition / Retakes** (сходится точно; соответствие выбору в UI пользователя — **HIGH CONFIDENCE**, само декодирование — CONFIRMED кодом и данными). Ветки композера eGame 4/5 (`mg_ar_*`, `mg_de_*`, дефолт-маски 8367 / 41005 — тоже сверены, дают ровно `mg_armsrace` / `mg_demolition`) достижимы лишь при `game/mode` = `gungameprogressive`/`gungametrbomb`; ни в одном логе этого не было (**HIGH CONFIDENCE**, что современный UI так не делает).
3. **Почему Demolition/Arms Race не получали сервер (CONFIRMED по коду):** GC слал eGame 12 → категория `skirmish`, `maps[]` пусто (маска не раскодировалась, §50 UNRESOLVED) → matcher смотрел только серверы категории `skirmish`; серверы категорий `armsrace` / `demolition` не могли подойти никогда. Раньше это выдавалось за «Skirmish не разобран».
- **Исправление (общий механизм, без хардкода под Sigma/Demolition):** C++ `MM::ParseSkirmishModes` (читает `items_game.txt` `skirmish_modes` в `ItemSchema`, `Inventory::Schema()`), `MM::SkirmishMapGroupMaps` (карты `mg_skirmish_<mode>` из `csgo/gamemodes.txt`, парсится при первом Skirmish-поиске), `MM::DecodeSkirmishSelection` (маска → варианты, неизвестные биты в лог); `SearchInfo::variants` → JSON `variants:[{name, game_mode, maps[]}]`. Java: `SearchRequest.variants` (только Skirmish, иначе `400 variants_not_allowed`), `SearchVariant`, колонка `matchmaking_search.variants` (миграция), `ModeCategory.forSkirmishGameMode` (`gungameprogressive`→armsrace, `gungametrbomb`→demolition, иначе skirmish), `pickServer` по всем вариантам (категория варианта И `server.map ∈ maps варианта`, побеждает дольше всех не назначавшийся). Skirmish без `variants` (старый GC) — по-прежнему любой сервер категории `skirmish`. Панель: колонка Maps показывает `armsrace [armsrace], demolition [demolition]`.
- **Проверка:** (a) `sk_test.exe` — настоящие `mm_modes.cpp` + `keyvalue.cpp` на настоящих `items_game.txt` / `gamemodes.txt`: 0x200 → armsrace(7 карт), 0x400 → demolition(6), 0x600 → оба, 0x800 → retakes(8), 0x1000000 → неизвестный бит; все 12 живых `game_type` из логов дают прежние `maps` и Skirmish-маски декодируются. (b) Сквозной прогон: настоящий `backend_client.cpp` + настоящее декодирование → настоящий backend (копия текущих исходников, порт 18090, отдельная БД): `game_type=393228` (Arms Race + Demolition) → игрок 1 на armsrace-сервере (`ar_shoots`), игрок 2 на demolition-сервере (`de_bank`), Retakes → skirmish-сервер, Demolition → demolition-сервер; серверы остались AVAILABLE. (c) 12 тестов `MapGroupMatchingTest`.
- **Не решено [UNRESOLVED]:** реальную карту srcds backend по-прежнему не контролирует (§50.4); серверы категорий Arms Race / Demolition в реестре пользователя надо завести самому (карта сервера ∈ `mg_skirmish_armsrace` / `mg_skirmish_demolition`) и запустить srcds в соответствующем режиме — поведение клиента при подключении к ним **в игре не проверялось**.

### 54.3 (C) Fake players переехали на backend: аудит, архитектура, реализация
**Аудит прежнего [CONFIRMED]:** fake-участников строит **srcds** локально (`test_accept.cpp` `FakeRoster`: id `0xFA4E0000 + i`, `Q`-резервация, UDP-пробы stage 1/2), размер по `-gc_mode`; Java Match про них не знал, matcher видел только реальных игроков (отсюда временный `backend.required-players.*=1`). Поведение fake-драйвера с `-ip 192.168.1.150` подтверждено в §53.
**Архитектура миграции (поэтапно, старый драйвер — fallback, не тронут):**
1. **[сделано]** модель и matcher в Java: fake-поиски — виртуальные участники backend, объединяются с реальными поисками матча (`real + fake ≥ required_players` → матч полный, реальные получают `WAITING_ACCEPT`). Инструмент отделён от production: пока нет включённых fake-поисков, `fillFormingMatches()` ничего не делает; глобальный выключатель `backend.fake-players.enabled` (по умолчанию true; false → API `409`, matcher игнорирует).
2. **[сделано]** канал состава матча: `assignment.players[]` (`account_id`, `fake`), вкладка Matches, `GET /api/v1/servers/roster?address=&port=` (X-Api-Key) — состав матча на сервере для srcds. Fake id = `0xFA4E0000 + n` (n — порядок вступления) — та же схема, что у srcds. **Нужен srcds-потребитель — пока нет.**
3. **[следующий этап, не начат]** srcds читает roster с backend вместо sniff + `-gc_mode` (убирает `awaiting=127` первой попытки, если assignment клиенту отдаётся после ack srcds), затем удаление `FakeRoster` из C++.
**Семантика [CONFIRMED тестами]:** fake-поиск сам матч не начинает и сервер не резервирует; присоединяется только к `FORMING` матчу Accept-режима, у которого есть хотя бы один реальный игрок; категория та же, `server.map` ∈ карты fake-поиска (пусто = любая), число игроков «влезает» (остаток мест ≥ игроков; несколько fake-поисков складываются, не влезающий ждёт); классические режимы не поддерживаются (400; ограничение на fake-поиск `required−1` = 9 / 3 / 15); матч распался (реальный ушёл / админ занял сервер) → fake снова `SEARCHING`; матч закончился (новый поиск одиночки, TTL, Release) → `COMPLETED` (Start запускает заново); Stop выходит из формирующегося матча; состав завершённого матча «заморожен» (стоп клиента при подключении не уменьшает `players`). Статусы: `SEARCHING`, `MATCHED`, `STOPPED`, `COMPLETED`; в панели — матч, сервер и `players/required`.
**Админ-панель:** новая вкладка **Fake Players**: таблица (статус, игроки, режим, карты, матч/сервер, Stop search/Start, Edit, Delete) и форма добавления (режим Accept-категорий, игроки, карты — кнопки из карт **ваших** серверов этого режима + ручной ввод, «start searching»); несколько fake-поисков одновременно. Код панели прогнан (node + заглушка DOM) на реальных ответах API: ошибок нет, строки рендерятся; в браузерной панели интерфейс не открывался (вход требует пароля) — визуально не проверен [?].
**Сквозная проверка (настоящий C++ клиент backend → backend):** сервер Competitive de_dust2 + fake-поиск 9 игроков (`de_dust2,de_mirage`) + реальный поиск `game_type 520` → клиент получил `WAITING_ACCEPT players=10/10 accept=1 required=10` (roundtrip ok, новое поле `players` парсер терпит); `GET roster` → `3e9846d5` (реальный) + `fa4e0001…fa4e0009` (fake) — ровно состав `Q`-payload из srcds-логов §53.
**Про `backend.required-players.*=1` (файл пользователя, не менялся):** пока переопределение = 1, матч собирается из одного реального игрока и fake-поиски не участвуют. Чтобы играть с fake-поисками backend, переопределение убрать; **srcds при этом всё равно строит свой ростер размера режима (10/4/16)** — суммы совпадают, потому что Accept-матч тоже 10/4/16.

### 54.4 Тесты (Java, `mvnw test`, **67 из 67**; было 37)
`BackendIntegrationTest` 15, `SchemaMigrationTest` 1 (+ колонка `variants`, таблица `fake_search`), `MatchmakingIntegrationTest` 21 (9 переписаны под 54.1), **новые:** `FakePlayersIntegrationTest` 17 (9+1 → 10/10; Wingman 3, DZ 15; несколько fake-поисков и «не влезает»; ждёт реального матча; не влияет на классику/без матча; STOPPED/disabled не входят; карта сервера должна входить; чужой режим; распад матча; COMPLETED/Start; TTL; админ занял сервер; валидация; сессия+CSRF; правка/удаление; канал srcds roster; размер завершённого матча), `FakePlayersDisabledTest` 1 (выключатель), `MapGroupMatchingTest` 12 (группы Sigma/Delta/Hostage/Dust24-7 на реальных списках карт из `gamemodes.txt`; категории не смешиваются; Skirmish-варианты; Arms Race ≠ Demolition; `variants` только для Skirmish; замена выбора).

### 54.5 Изменённые / новые файлы
Java: `search/{SearchService, SearchRepository, SearchRecord, SearchRequest, SearchView, SearchApiController, MatchRepository}`, новые `search/{SearchVariant, RosterEntry}`, `fake/{FakeStatus, FakeSearch, FakeSearchRequest, FakeSearchView, FakeSearchRepository}`, `mode/ModeCategory`, `server/GameServerRepository`, `admin/AdminApiController`, `config/{BackendProperties, SchemaInitializer, SecurityConfig}`, `schema.sql`, панель (`admin/index.html`, `static/admin/assets/{admin.js, admin.css}`), `README.md`, `config/application-example.properties` (`backend.fake-players.enabled`); тесты — 5 файлов выше + `BackendTestBase`. C++: `mm_modes.{h,cpp}`, `item_schema.{h,cpp}`, `inventory.h`, `backend_client.{h,cpp}`, `gc_client.cpp` (`OnMatchmakingStart`). Не трогались: Panorama, protobuf, reservation-протокол 9107/0x21/0x25, Accept-флоу, `DedicatedServerAddress`, fake-драйвер srcds, `required-players`, реальные player-поиски (Casual/DM работают как раньше), Git.
**DLL:** `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll`, 5 795 840 байт, 2026-09-19 19:35, SHA-256 `691023582965640ed57e2b1d25cd63725dc10850a48d20e514e2b6966dd82632`; 0 предупреждений/ошибок в нашем коде; **в игру не копировалась**.

### 54.6 Поправки к прежним разделам
- §50.4 «Demolition / Arms Race (UNRESOLVED …)» и §49.8 п.5 «Skirmish (карты не раскодируются)» и §44.5 «Skirmish: `1<<(index-1)`, не разобраны»: **закрыто §54.2** — маска Skirmish = `1 << (id−1)` ids из `items_game.txt skirmish_modes`; Arms Race/Demolition приходят только как Skirmish. Гипотеза «не сходится с наблюдаемыми битами» (§50.4) была неверной: ids начинаются не с 1 по порядку `mapgroupsMP`.
- §49.4 / §50.5: «RESERVED для всех режимов» → исправлено, реализована целевая семантика §50.5 (54.1). §51.2 (точка изменения) выполнена.
- §49.8 п.1 / §50.3: fake roster «только на srcds» → теперь есть backend-модель (54.3); srcds-fallback остаётся до этапа 3.

### 54.7 Что проверить пользователю в игре (ничего из этого не запускалось)
1. Остановить backend, `java-backend\build.cmd`, `run.cmd` (миграция БД добавит `variants` и `fake_search`), при желании скопировать новый DLL в игру с бэкапом.
2. Casual/Deathmatch: `state` сервера остаётся Available после подключения (панель → Game Servers).
3. War Games → Arms Race / Demolition: зарегистрировать серверы категорий Arms Race / Demolition (карта из `mg_skirmish_*`), в `gc_log.txt`/консоли — строка `[MM] mode=skirmish … skirmish modes armsrace(7 maps),…`, в панели колонка Maps с `armsrace [armsrace]`.
4. Fake Players: убрать `backend.required-players.*=1`, добавить fake-поиск (Competitive, 9 игроков, карта = карта сервера), запустить обычный поиск → `WAITING_ACCEPT`, в панели — Matched; далее прежний Accept-флоу с srcds `-ip 192.168.1.150 -gc_mode competitive`.

### §54 STATUS
Реализовано (A), (B), (C-этапы 1–2). Дальше по решению пользователя: игровой тест 54.7; этап 3 миграции fake (srcds читает roster с backend, ack, затем удаление `FakeRoster`); управление реальной картой srcds (§50.4).



---

## 55. BACKEND — ИСТОЧНИК ИСТИНЫ ДЛЯ РОСТЕРА ТЕСТОВОГО МАТЧА: srcds получает состав матча (`assignment.players[]`) с backend и взводит по нему резервацию

**Статус:** реализовано и проверено офлайн (Java 82 теста; C++ собран; decision table srcds-логики; сквозной прогон настоящих классов DLL против настоящего backend). **Против настоящего `engine.dll` и в игре не запускалось [?]** — нужен игровой тест (55.7). Старый srcds-локальный fake-драйвер не удалён, остаётся fallback. Git не трогался, DLL в игру не копировалась. Не затронуты: Panorama, protobuf, reservation-протокол (9107 / 0x21 / 0x25), Accept-флоу клиента, `DedicatedServerAddress`, значение `required-players` в конфиге пользователя, Java reservation split. Метки: **CONFIRMED** (проверено запуском/тестом/кодом) / **HIGH CONFIDENCE** / **HYPOTHESIS** / **UNRESOLVED**.

### 55.1 Аудит и выбор канала (`assignment.players[]` → srcds)
- **Что было [CONFIRMED]:** backend уже строит `assignment.players[]` (§54), но srcds его не читал: `ServerGC` брал реального игрока из sniff первого 0x21 и сам строил `FakeRoster` (`[real][fake1..N-1]`), размер — по `-gc_mode`. Из-за sniff первая попытка после старта srcds всегда `awaiting=127`.
- **Каналы, которые рассматривались:** (1) клиент → srcds: в 9107 нет поля ростера, 0x21/0x25 — пакеты фиксированного формата, protobuf/reservation-протокол не трогаем → отпадает; (2) backend → srcds push: у srcds нет слушающего порта под это → отпадает; (3) **srcds → backend HTTP** — **выбран**: `matchmaking.backend_url` / `backend_api_key` уже читаются srcds из общего `config.txt`, HTTP-клиент `BackendClient` уже есть (только `http://`, таймауты 1,5 / 3 с), адрес и порт своего сервера srcds знает (`-ip` / `-port`, тот же `DedicatedServerAddress()`/`DedicatedServerPort()`, что использует fake-драйвер).
- **Итоговая схема:** Java Backend `match.players[]` → `GET /api/v1/servers/roster?address=&port=` (srcds, раз в секунду) → `RosterFeed::Poller` (поток) → GC-поток `ServerGC` → `RosterFeed::Controller` (решение) → `AcceptTest::FakeRoster` с явным списком участников → `Q`-резервация (`ReserveServerForQueuedGame`) → Accept-флоу как прежде.

### 55.2 Протокол и семантика backend [CONFIRMED тестами]
- **`GET /api/v1/servers/roster`** (X-Api-Key): ближайший `FORMING`/`READY` матч зарегистрированного сервера: `match_id, mode, status, map, accept_required, required_players, real_players, fake_players, players[{account_id, fake}]`; порядок — реальные игроки, затем fake (`0xFA4E0000 + n`); 404 — сервер не зарегистрирован / матча нет. Каждый вызов = «этот сервер читает ростер с backend» (`backend.roster-poll-window` PT10S).
- **`POST /api/v1/servers/roster/ready`** `{address, port, match_id}` (X-Api-Key): «ростер взведён»; 404 — сервер не зарегистрирован / матч не этого сервера / не активен; повтор безвреден. Ответ `{match_id, promoted}`.
- **Удержание assignment:** Accept-матч, ставший полным на сервере, который читает ростер, остаётся у игроков в `MATCHED` (`match.status=READY`, `match.awaiting_server=true`, `assignment` нет) до подтверждения; дальше `WAITING_ACCEPT` + `assignment`. Отпускается также по `backend.roster-ack-timeout` (PT25S) и если сервер перестал спрашивать ростер. Сервер, который ростер не спрашивает (старый srcds), и классические режимы не удерживаются вообще.
- **`required_players` ≠ fake:** `required_players` — размер матча (10 / 4 / 16), `real_players + fake_players = required_players`; в assignment добавлены `real_players`, `fake_players`; fake-поиск не «уменьшает» `required_players`. Пример из задачи: fake 9 × Competitive `[de_dust2]` + real Competitive `[de_dust2]` → **один** матч 10 = 1 + 9, `assignment.players` = ровно эти 10.
- Панель: у матча и поиска признак «ждёт srcds» (`awaiting_server`).

### 55.3 srcds (C++) [CONFIRMED сборкой и офлайн-прогонами]
- **`RosterFeed::Poller`** (`server_roster.{h,cpp}`, отдельный поток, не блокирует игру/GC): раз в секунду `GET roster`, шлёт `GCEvent::BackendRoster` только при изменении ответа (лог `[MM-ACCEPT] backend roster: …`); ставит в очередь и повторяет `POST ready`. Стартует вместе с `-gc_mode` (`StartAcceptTestRoster`) и только если `backend_url` настроен.
- **`RosterFeed::Controller`** (чистая state-машина, GC-поток; отдельный класс ради офлайн-теста): (1) матч `READY` + размер ростера = размер режима srcds (10/4/16) + есть реальный игрок + `accept_required` + режим совпал → `arm` **ровно этим списком** (если уже взведён другой ростер — с `unreserveFirst`: Unreserve → 12 с cooldown → Q); (2) тот же список уже взведён (тот же игрок ищет снова, или legacy-ростер совпал) → не пересобирать, при готовности fake — подтвердить новый матч; (3) когда **все fake на stage 1** (`onFakesReady(true)`) → `POST ready` один раз на матч; (4) ростер, который не подходит (1 участник при `required-players=1`, чужой режим, нет реального, не-Accept) → **не взводится, игроков не удерживают** (`confirm` сразу), работает legacy-ростер (sniff) — поведение как до §55; (5) смена ростера откладывается, пока на сервере игрок (`m_connectedClients`), и применяется, когда он вышел; (6) backend недоступен → остаётся то, что взведено, ничего не подтверждается.
- **`AcceptTest::FakeRoster`:** `Params` расширен (`participants`, `source`, `unreserveFirst`, `onFakesReady`), payload строится из явного списка (`BuildQueuedReservationPayload(cookie, participants)`), fake определяются по `IsFakeAccountId`, токен ответа → индекс по списку. Тайминги, пейсинг, keep-alive, re-arm **не менялись**; без `participants` строится прежний legacy-ростер `[real][fake 1..N-1]` (проверено: тот же payload, тот же ход). `[FAKE-MM] started` теперь пишет `source=` (`legacy…` / `backend match m-…`).
- **`ServerGC`:** `OnBackendRoster`, `ArmRoster`; legacy sniff (`OnTestRealPlayerSeen` → `CreateTestRoster`) остаётся: если backend-ростер «в работе» (`UsingBackendRoster`), sniffed-игрок ему принадлежит, legacy не создаётся; иначе — как раньше. Новые события `GCEvent::BackendRoster`, `GCEvent::TestFakesReady`. `backend_client.{h,cpp}`: `FetchServerRoster`, `ConfirmServerRoster` (блокирующие, только из рабочего потока).

### 55.4 Backend-тесты (Java, 82 из 82; было 67) — `BackendDrivenRosterTest` (15)
Явные: 1 real + 9 fake = 10 (пример из задачи, один матч, `players` = эти 10); 1 + 3 = 4 (Wingman); 1 + 15 = 16 (DZ); fake-поиск несовместимого режима не матчится; несовместимой карты не матчится; несколько fake-поисков складываются (4 + 3 + 2 = 9); выключенный fake-поиск не участвует (и вступает после включения); fake не создают матч и не занимают сервер без подходящего real-поиска (другой режим / другая карта / после ухода единственного real матч `CANCELLED`, сервер `AVAILABLE`). Handshake: сервер, читающий ростер, удерживает assignment до подтверждения (с ростером из GET и `awaiting_server`), не читающий — нет; таймаут удержания; конец удержания при молчании сервера; классика не удерживается; подтверждение обязано называть матч именно этого сервера (404 для чужого/неизвестного/незарегистрированного, 401 без ключа, 400 без `match_id`); ростер второго матча на том же сервере.

### 55.5 Офлайн-проверки C++ (`D:\csgo2021_gc\offline_tests\roster\`, `build.bat`; README там же)
- **`controller_test.exe` — ALL PASSED** (0 failed): no match/backend down; FORMING не взводится; полный ростер взводится точно списком backend (реальный id + `fa4e0001…09`) и подтверждается только после «fake на stage 1»; повторные опросы ничего не меняют; тот же игрок снова — без re-arm, новый матч подтверждается сразу; другой ростер → re-arm с unreserve; смена откладывается при игроке на сервере; 1 участник из 10 / чужой режим / только fake / не-Accept — не взводится, не удерживается, подтверждается один раз; legacy-ростер и backend совпали (adopt); backend пропал после взвода; сериализация снимка между потоками.
- **`roster_e2e.exe` — сквозной прогон настоящих классов** (`BackendClient` клиента и srcds, `Poller`, `Controller`, `FakeRoster`) против настоящего backend (копия текущих исходников, порт 18090, отдельная БД) и UDP-заглушки движка (реализует 0x21 → 0x25 для `Q`-ростера): **S1** сервер Competitive + fake 9 + реальный поиск: клиент сначала `MATCHED 10/10` (удержан), srcds видит матч `READY` (1 real + 9 fake), взводит `Q…[3e9846d5][fa4e0001]…[fa4e0009]`, fake доходят до stage 1 (`awaiting 9→1, total=10`), `POST ready` → `promoted:1` → клиент получает `WAITING_ACCEPT` через ~0,8 с (его опрос раз в секунду) → **первая же проверка клиента `stage 1 → awaiting=0 total=10`** (без прежней первой неудачной попытки), fake принимают, `stage 2 → awaiting=0 total=10`. **S2** backend с `required-players.competitive=1`: ростер из 1 участника не подходит srcds на 10 → не взводится, игрок получает сервер через ~0,5 с без удержания, первая проверка `awaiting=127` — то есть поведение «как сейчас» (legacy sniff в заглушке не моделировался). **S3 (legacy)** старый драйвер без списка: payload `[3e9846d5][fa4e0001..03]`, все fake на stage 1, `awaiting=0`, `stage 2 awaiting=0` — путь не сломан.
- **Не проверено [?]:** настоящий `engine.dll` (заглушка повторяет только протокол 0x21/0x25 по §46/§53), реальный srcds с `-gc_mode` и клиент; поведение при смене ростера (Unreserve + 12 с) на реальном движке.

### 55.6 Соответствие `game_type → skirmish id → server category → maps[]` (сохранено, чтобы не реверсить заново)
`game_type = eGame | (mask << 8)`, `eGame = game_type & 0xF`, `mask = (game_type >> 8) & 0xFFFFFF`. Категория сервера в backend = `eGame` → `ModeCategory`; для Skirmish — по `game_mode` варианта.

| eGame | режим (категория) | что в `mask` | `maps[]` |
|---|---|---|---|
| 6 / 7 | Deathmatch / Casual | биты карт (composer `sub_10288A90` case 6/7): `mg_dust247`=0x2, `mg_casualdelta`=0x1010B4, `mg_casualsigma`=0x640048, `mg_hostage`=0x90F00 и одиночные `mg_de_*`/`mg_cs_*` | битовая таблица `s_casualDeathmatch` (`mm_modes.cpp`); Sigma {basalt, ancient, vertigo, cbble, canals}; Delta {mirage, inferno, overpass, nuke, train, cache}; Hostage {insertion2, agency, militia, office, italy, assault}; Dust 24/7 {dust2}. Живые: 519 → dust2; 269530119 → Delta; 1677740038 (DM) → Sigma; 151977990 (DM) → Hostage |
| 8 | Competitive | те же биты + флаг `0x20000` (`mg_lobby_mapveto`, не карта) | `s_casualDeathmatch` без флага (268456456 → dust2, inferno, vertigo, overpass; 520 → dust2) |
| 10 | Wingman | биты карт case 0xA (`s_wingman`) | 2058 → de_lake |
| 13 | Danger Zone | `dz_blacksite`=0x1, `dz_sirocco`=0x2, `dz_county`=0x10 | 4877 → все три |
| 4 / 5 | Arms Race / Demolition (в современном UI **не приходят**) | биты карт (`mg_ar_*`, `mg_de_*`); неизвестный токен ⇒ дефолт 8367 / 41005 = все карты группы | `s_armsRace` / `s_demolition` |
| **12** | **Skirmish (War Games)** | **`mask` = выбранные РЕЖИМЫ**: бит `1 << (id − 1)`, id из `items_game.txt skirmish_modes` (composer, case 0xC: токены `mg_skirmish_<name>`) | карты **режима** — группа `mg_skirmish_<name>` из `gamemodes.txt` (ниже) |

Skirmish подробно (id → бит → имя → `gamemode` из items_game → **категория сервера в backend** → `maps[]` из `gamemodes.txt`):

| id | бит | name | gamemode | категория | maps[] |
|---|---|---|---|---|---|
| 1 | 0x1 | stabstabzap | casual | skirmish | de_safehouse, de_lake, gd_rialto, de_austria |
| 2 | 0x2 | dm_freeforall | deathmatch | skirmish | de_dust2, de_inferno, de_mirage, de_cbble, de_overpass, de_nuke, de_vertigo, cs_militia, cs_assault, cs_office, cs_italy, de_lake, de_stmarc, de_ancient |
| 3 | 0x4 | flyingscoutsman | casual | skirmish | de_lake, de_safehouse, ar_dizzy, ar_lunacy, ar_shoots |
| 4 | 0x8 | triggerdiscipline | casual | skirmish | de_austria, de_inferno, de_thrill, de_mirage, de_dust2, de_lite |
| 6 | 0x20 | headshots | deathmatch | skirmish | cs_agency, de_inferno, de_blackgold, de_cache, de_cbble, de_nuke |
| 7 | 0x40 | huntergatherers | deathmatch | skirmish | de_nuke, de_dust2, cs_insertion, de_thrill, de_canals, de_cbble, de_train |
| 8 | 0x80 | heavyassaultsuit | casual | skirmish | de_dust2, de_mirage, de_overpass, de_shipped, de_austria |
| **10** | **0x200** | **armsrace** | **gungameprogressive** | **armsrace** | de_lake, ar_baggage, de_safehouse, de_stmarc, ar_shoots, ar_lunacy, ar_monastery |
| **11** | **0x400** | **demolition** | **gungametrbomb** | **demolition** | de_lake, de_safehouse, de_sugarcane, de_bank, de_stmarc, de_shortdust |
| 12 | 0x800 | retakes | casual | skirmish | de_inferno, de_mirage, de_dust2, de_nuke, de_overpass, de_train, de_vertigo, de_ancient |
(ids 5 и 9 в `skirmish_modes` нет). Живые/проверенные `game_type`: 131084 = 12 \| 0x200<<8 → armsrace; **393228 = 12 \| 0x600<<8 → armsrace + demolition**; 524300 = 12 \| 0x800<<8 → retakes; 262156 = 12 \| 0x400<<8 → demolition. Правило категории (Java `ModeCategory.forSkirmishGameMode`): `gungameprogressive` → `armsrace`, `gungametrbomb` → `demolition`, всё остальное → `skirmish`. Сервер подходит варианту, если `категория сервера == категория варианта` И `server.map ∈ maps[] варианта`; из подходящих берётся дольше всех не назначавшийся. Сверка композера с `gamemodes.txt`: 34 группы Casual/DM/Competitive/Wingman/DZ/Arms Race/Demolition, 0 расхождений (§54.2).

### 55.7 Игровой тест (следующий шаг, ничего из этого не запускалось)
1. Остановить backend, `java-backend\build.cmd`, `run.cmd` (миграции не нужны: изменения только кода/конфига). **Убрать `backend.required-players.competitive/wingman/dangerzone=1`** из `config\application.properties`, иначе матч соберётся из 1 игрока и srcds включит legacy-ростер (это штатный fallback, но не то, что проверяем).
2. Новый `csgo_gc.dll` (`Build\release\csgo_gc\csgo_gc.dll`, SHA-256 ниже) — в игру и **во все папки, откуда стартуют srcds** (та же папка `csgo legacy`), с бэкапом старого.
3. Панель → Fake Players: Competitive, 9 игроков, карта = карта сервера. srcds: `-ip 192.168.1.150 -port 27017 -gc_mode competitive …` (адрес и порт **должны совпадать** с записью в Game Servers). Клиент: обычный поиск Competitive.
4. Что должно быть в логе srcds (`gc_log.txt`, запускать клиент до srcds): `[MM-ACCEPT] backend roster: asking the backend…`; после поиска `backend roster: match m-… READY competitive: 10 participants (1 real + 9 fake) of 10`; `arming the roster of backend match m-…`; `[FAKE-MM] started … source=backend match m-…`; `Q…[<accountid игрока>][fa4e0001]…[fa4e0009]`; fake `stage=1 confirmed`; `roster of match m-… is armed and every fake participant is at stage 1: telling the backend`; `backend roster: match … armed -> confirmed ({"…","promoted":1})`. В логе клиента: `MATCHED … players=10/10`, затем `WAITING_ACCEPT -> …`, **первая же** `0x25 stage=1 awaiting=0 total=10`, `popup_accept_match_found` — без прежней первой неудачной попытки. Аналогично Wingman (3 fake) и Danger Zone (15 fake).
5. Сравнить с прежним поведением (`awaiting=127` на первой попытке). Только после успеха — решать про удаление legacy `FakeRoster` (отдельным шагом).

### 55.8 Известные ограничения / нерешённое
1. **[UNRESOLVED]** поведение против настоящего `engine.dll` (особенно смена ростера: Unreserve + 12 с, а удержание у backend ограничено `roster-ack-timeout` PT25S) и в игре.
2. Адрес/порт srcds (`-ip` / `-port`) должны совпасть с записью реестра (host `192.168.1.150`); при расхождении backend отвечает 404 «нет матча», srcds работает по legacy-ростеру и игрока backend не держит (сервер не спрашивал ростер — для backend это «не читающий»). Имя хоста в реестре при `-ip` = IP не сопоставится.
3. Ростер, размер которого не равен размеру режима srcds, намеренно не используется (fallback на legacy) — включая `required-players=1`.
4. Матч из ≥2 реальных игроков поддержан ростером (все реальные id в `Q`), в игре не проверялся; fake-драйвер отвечает только за fake.
5. Backend пока не знает о конце матча на srcds (heartbeat не реализован); ростер `READY`-матча остаётся «активным» до TTL / нового поиска, srcds его повторно не пересобирает (тот же список — без re-arm).
6. Старый srcds-локальный fake-драйвер и sniff остаются (legacy/fallback); удалять — только после успешного игрового теста 55.7.
7. В момент написания порт 8080 (backend пользователя) не слушался — не запущен (мной не останавливался); тесты шли на своих копиях (порт 18090).

### 55.9 Артефакты
**DLL:** `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll`, 5 903 872 байт, 2026-09-19 20:12, SHA-256 `98c794c0e13e13960f58d035473d3c45652a57c9a38ef167e11096f50e44f073`; 0 ошибок/предупреждений в нашем коде; в игру не копировалась. **Новые/изменённые файлы:** Java — `search/{SearchService, SearchApiController, SearchView, MatchRepository}`, `config/{BackendProperties, SecurityConfig}`, `README.md`, `config/application-example.properties`; тесты `BackendDrivenRosterTest` (+ свойства в `BackendTestBase`). C++ — новые `server_roster.{h,cpp}` (+ `CMakeLists.txt`), `backend_client.{h,cpp}`, `test_accept.{h,cpp}`, `gc_server.{h,cpp}`, `gc_shared.h`; офлайн-тесты `offline_tests\roster\{build.bat, README.md, controller_test.cpp, roster_e2e.cpp, stdafx.h, config.h, funchook.h}` (каталог `build\` — результат сборки).

### §55 STATUS
Реализованы: (1) канал srcds → backend (`GET roster` / `POST ready`); (2) srcds взводит резервацию списком backend (настоящие AccountID + fake-id), подтверждает готовность; (3) backend удерживает assignment до подтверждения, `required_players = real + fake`; (4) legacy-драйвер сохранён как fallback; (5) 15 новых backend-тестов; (6) таблица `game_type → skirmish id → категория → maps[]` записана (55.6). Дальше — игровой тест 55.7; при успехе — отдельным шагом убрать legacy `FakeRoster`/sniff и `required-players`-обходы.


---

## 56. ПОЛНЫЙ АКТУАЛЬНЫЙ CHECKPOINT ПРОЕКТА (после §55) — ЧИТАТЬ ПЕРВЫМ

**Назначение.** Единая точка восстановления контекста: прочитав этот раздел, можно продолжать работу без объяснений. Он **заменяет §46 как главный checkpoint** (архитектура §46 устарела: выбор сервера, fake roster и reservation-семантика с тех пор переехали). Старые разделы §1–§55 **не удалялись** и остаются историей и источником деталей (номера ссылок ниже — на них). Раздел написан как документация: код, конфиги и Git не менялись. Метки: **CONFIRMED** (прямое свидетельство: лог реальной игры, запуск, тест, код клиента) / **HIGH CONFIDENCE** / **HYPOTHESIS** / **UNRESOLVED**. Если ниже вывод помечен «актуально», а в старом разделе сказано иначе — верить этому разделу (таблица поправок — 56.13).

### 56.1 Проект и правила работы
- **Проект:** `D:\csgo2021_gc` — эмулятор Game Coordinator для CS:GO 2021 (build 1352, `PatchVersion=1.38.0.5`, `ClientVersion=13805`) в виде `csgo_gc.dll` (один DLL и для клиента `csgo.exe`, и для `srcds.exe`: `ClientGC` / `ServerGC`) + Java matchmaking backend (`java-backend\`). Ветка git `experimental-native-mm`.
- **Правила пользователя:** Git полностью под контролем пользователя (никаких commit/push/pull/reset/checkout/merge/rebase); `RESEARCH_FINDINGS.md` — append-only checkpoint (старое не удалять, исправления — новой записью с пометкой «актуально»); перед новым большим RE — перечитывать findings; DLL в игру **автоматически не копировать**; пользователь сам запускает игру/srcds и приносит логи; я не могу запускать его игру.
- **Стиль результатов:** сначала аудит/план → реализация → тесты → запись в findings; для выводов — метки достоверности.

### 56.2 Состояние на диске (на момент этого checkpoint) [CONFIRMED чтением файлов]
- **Исходники C++ (`csgo_gc\`)** — изменены/новые относительно HEAD (git status, только чтение): `CMakeLists.txt, config.{h,cpp}, gc_client.{h,cpp}, gc_server.{h,cpp}, gc_shared.h, inventory.h, item_schema.{h,cpp}, mm_modes.{h,cpp}, test_accept.{h,cpp}`, новые `backend_client.{h,cpp}`, `server_roster.{h,cpp}`; `examples\config.txt`. Новые каталоги (не в git): `java-backend\`, `offline_tests\roster\`.
- **Собранный DLL:** `D:\csgo2021_gc\Build\release\csgo_gc\csgo_gc.dll`, 5 903 872 байт, 2026-09-19 20:12, SHA-256 `98c794c0e13e13960f58d035473d3c45652a57c9a38ef167e11096f50e44f073` — сборка §55 (roster с backend, skirmish variants). **В игру НЕ установлена.**
- **DLL, установленный в игре** (`…\csgo legacy\csgo_gc\csgo_gc.dll`): SHA-256 `2139de91…a296` = сборка **§49** (backend-выбор сервера, без skirmish/roster). Значит все игровые логи пользователя (§49–§53) сняты **старым** DLL.
- **Backend jar пользователя** `java-backend\target\matchmaking-backend.jar` — от 16:31 (**до §54**, без reservation split / variants / fake players / roster). **На момент checkpoint ни backend (порт 8080), ни srcds, ни csgo не запущены** (мной не останавливались; тесты шли на моих копиях порта 18090, уже остановленных).
- **БД пользователя** `java-backend\data\matchmaking.db`: миграции нового кода **ещё не применялись** (нет колонки `matchmaking_search.variants`, таблицы `fake_search`); они добавятся сами при первом старте нового jar (`SchemaInitializer`, идемпотентно). Реестр серверов (всё `AVAILABLE`, enabled): `192.168.1.150:27014 deathmatch cs_agency`; `:27016 casual de_inferno` (метка изменена пользователем; реальный srcds на de_dust2); `:27017 competitive de_dust2`; `:27018 wingman de_lake`; `:27019 dangerzone dz_blacksite`. Серверов категорий Arms Race / Demolition / Skirmish в реестре нет.
- **Конфиг backend пользователя** (`java-backend\config\application.properties`, git-ignored): `server.address=192.168.1.150`, `server.port=8080`, admin `admin`/(пароль), `backend.api-key` (в `config.txt` клиента — `test-api`), `stale-search-timeout PT15M`, `reaper-interval PT10S`, `matcher-interval PT1S`, `server-reservation-ttl PT10M`, `assigned-search-timeout PT2M` и **`backend.required-players.competitive/wingman/dangerzone=1`** (тестовый override, поставлен в §49; **надо убрать** для игрового теста §55.7).
- **Конфиг игры** (`…\csgo legacy\csgo_gc\config.txt`, общий для клиента и srcds): в `matchmaking` **только** `backend_url "http://192.168.1.150:8080"`, `backend_api_key "test-api"`; `log_output "2"`; бэкапы `config.txt.bak_pre_backend`, `…_pre_backend_selection`. В клиентском конфиге больше нет `test_server_*`, `test_accept_mode`, `test_real_account_id`, `test_fake_accept_delay_ms`.

### 56.3 Текущая архитектура C++ GC ↔ Java backend ↔ srcds [CONFIRMED кодом; работа в игре — по 56.11]
```
csgo.exe (клиент)                          Java backend (Spring Boot, SQLite)                 srcds.exe (Accept-режим: -gc_mode …)
 ClientGC ─9101 MatchmakingStart──▶ BackendClient ──POST /api/v1/matchmaking/search──▶ SearchService (matcher)
                                     worker-поток   ◀─GET  /matchmaking/search/{id} раз/с──   реестр серверов, матчи,
 ClientGC ◀─BackendSearchResult──────┘  (assignment: server, map, players[])                    fake-поиски (виртуальные участники)
 StartServerFlow(assignment)
   ├─ 9107 #1 (Accept: reservation{game_type}) ─▶ клиентский handler ─▶ UDP 0x21 (раз/с) ─────────────────────────▶ движок srcds
   ◀──────────────── UDP 0x25 (awaiting/total) ◀───────────────────────────────────────────────────────────────── (ServerGC: Q-резервация)
   ├─ Accept popup → stage 2 → 0x25 awaiting=0 → 9107 #2 (без reservation) → QueueConnect → connect
   └─ 9102 MatchmakingStop ─▶ POST /matchmaking/cancel
                                      ServerGC ── RosterFeed::Poller ──GET /servers/roster раз/с──▶ backend (состав матча его сервера)
                                               ── RosterFeed::Controller (решение) ── FakeRoster (fake-участники stage 1/2, UDP 0x21 на свой -ip:-port)
                                               ── POST /servers/roster/ready (когда fake на stage 1) ──▶ backend отпускает assignment игроку
```
- **Принятые архитектурные решения:** (1) выбор сервера, состав матча, fake-участники — **на Java backend**; C++ клиент только сообщает поиск и исполняет полученный assignment (9107/Accept/QueueConnect — прежний, рабочий код). (2) Клиентский конфиг знает только `backend_url`/`backend_api_key`. (3) Транспорт резервации остаётся в C++/srcds (`G`/`Q`-резервация через `IVEngineServer::ReserveServerForQueuedGame`, UDP 0x21/0x25, cookie-константа `GameServerCookieId = 0x293A206F6C6C6548`); **reservation protocol, protobuf, Panorama и Accept-флоу клиента не менялись** после §46. (4) Резервирование сервера в backend — только для Accept-режимов (§54.1). (5) srcds → backend (HTTP-опрос), а не push (§55.1). (6) Старый srcds-локальный fake-драйвер и sniff — fallback (legacy), удалить только после успешного игрового теста. (7) Разделять `serverMaxPlayers` (свойство srcds) ≠ `required_players` (размер матча) ≠ размер reservation-roster (= required_players) ≠ число принявших (`total − awaiting`).

### 56.4 Matchmaking: клиентская часть (C++)
- **`ClientGC::OnMatchmakingStart` (9101)** [CONFIRMED]: `eGame = game_type & 0xF`; `MM::FindGameMode` (`mm_modes.cpp`, режимы 4,5,6,7,8,9,10,11,12,13; поддерживаются все, кроме 9 Cooperative; ScrimComp5v5 в backend 400); `DecodeMapSelection` (биты карт → `maps[]`); для eGame 12 — `DecodeSkirmishSelection` (режимы из `items_game.txt skirmish_modes` + карты из `csgo/gamemodes.txt`, читается при первом Skirmish-поиске) → `SearchInfo{accountId, gameType, mode, gameMode, maps[], variants[]}` → `BackendClient::SearchStarted`. AccountID = `ISteamUser::GetSteamID() & 0xFFFFFFFF` (1050166997 = `0x3E9846D5` у пользователя).
- **`BackendClient` (`backend_client.cpp`, свой HTTP/1.1 на сокетах, только `http://`)** [CONFIRMED]: рабочий поток; сессия Register (`POST search`, повтор каждые 2 с) → Poll (`GET search/{request_id}` раз в секунду; это и heartbeat) → Done (assignment / терминальный статус / 4xx / 15 мин); 404 при опросе → повторная регистрация; таймауты connect 1,5 с / запрос 3 с; вызовы из GC-потока не блокируют; результат — `GCEvent::BackendSearchResult` в поток `ClientGC`. Ошибки backend не ломают игру (side-channel). Без `backend_url` поиск не обслуживается (лог в консоли).
- **`ClientGC::OnBackendSearchResult` → `StartServerFlow(assignment)`** [CONFIRMED live §53]: прежний код: 9107 #1 (`serverid`, `direct_udp_ip/port`, `reservationid`=cookie, `map`, `server_address`; для Accept — `reservation{game_type}`; `game_mode`/`account_ids`/`match_id` в 9107 **не передаются**); `ArmClient` (хук `WSARecvFrom` ловит 0x25); non-Accept — 9107 без reservation + локальный мост `G`-резервации.
- **Клиент шлёт 9102 (`MatchmakingStop`) ~2 с после `QueueConnectToServer`** при подключении в Accept-режимах (CONFIRMED §53.5): backend получает `cancel` уже назначенного поиска — это **не** освобождает сервер и не считается концом матча. Для классических режимов — не проверялось [?].

### 56.5 Server selection и matchmaking на backend (Java) [CONFIRMED тестами; live — по 56.11]
- **Категории** (`ModeCategory`): competitive(8, 10 игроков), wingman(10, 4), dangerzone(13, 16), casual(7), deathmatch(6), armsrace(4), demolition(5), skirmish(12); `acceptRequired` ⇔ `requiredPlayers > 0`. Категория поиска = `game_type & 0xF` (для Skirmish с `variants` — по `game_mode` варианта, 56.6).
- **Сервер подходит:** `enabled AND state==AVAILABLE AND category == категория AND (maps[] пуст ИЛИ server.map ∈ maps[])`; сервер без карты подходит только поиску без карт; из подходящих — дольше всех не назначавшийся (`last_assigned_at`); хост из реестра резолвится в IPv4 (нужен для `direct_udp_ip`). **Реальную карту srcds backend не контролирует** — `server.map` в реестре это ручная метка (CONFIRMED, §50.4).
- **Reservation split (актуально, §54.1):** Accept-режимы: `AVAILABLE → RESERVED` за матчем (эксклюзивно, `reserved_match_id`); классика (Casual/DM/Arms Race/Demolition/Skirmish): сервер **не резервируется**, остаётся AVAILABLE, только двигается `last_assigned_at` (несколько игроков на один сервер); BUSY — единственный способ «закрыть» (админ или отчёт сервера). Освобождение RESERVED: Release/Set available в панели, отчёт сервера `AVAILABLE` (не снимает свежий RESERVED), TTL `server-reservation-ttl`, а также **правило «новый поиск»**: игрок, оставшийся один в READY-матче, начал новый поиск → сервер AVAILABLE, матч ENDED (провал Accept/«искать снова»).
- **Accept-матч:** первый поиск резервирует сервер и открывает матч `FORMING`; совместимые реальные поиски (категория, `server.map` ∈ их карты) присоединяются (`MATCHED`, `players/required_players`); `real + fake ≥ required_players` → матч `READY`, реальные получают `WAITING_ACCEPT` и **один** `assignment`. Распад: единственный реальный игрок ушёл / админ занял сервер (BUSY/Release/удаление) → формирующийся матч `CANCELLED`, игроки и fake снова `SEARCHING`.
- **Статусы:** поиска — `SEARCHING, MATCHED, WAITING_ACCEPT, READY_TO_CONNECT, CANCELLED, EXPIRED, REMOVED, COMPLETED` (назначенный поиск → `COMPLETED` через `assigned-search-timeout`; неопрошенный → `EXPIRED` через `stale-search-timeout`); матча — `FORMING, READY, ENDED, CANCELLED`; сервера — `AVAILABLE, RESERVED, BUSY`; fake-поиска — `SEARCHING, MATCHED, STOPPED, COMPLETED`.
- **`assignment`** (в ответе поиска, когда матч полный и отпущен): `{match_id, server_id, server_address (IPv4), server_port, map, accept_required, required_players, players:[{account_id, fake}], real_players, fake_players}`. `map` = метка реестра, не запрошенная карта. Cookie резервации в assignment **нет** (константа GC).

### 56.6 Map groups / variants [CONFIRMED клиентским кодом и данными игры; подробно §44.5, §54.2, §55.6]
- **Путь данных:** UI → lobby KV (`game/mode`, `game/mapgroupname` = токены `mg_*`) → `sub_10288960` (строка режима → eGame) → composer `sub_10288A90` → `game_type = eGame | (mask << 8)` → 9101 → `mm_modes` → `maps[]`/`variants[]` → backend.
- **Обычные режимы (6/7/8/10/13):** биты карт; декодирование верное (сверка композер × `gamemodes.txt` × `mm_modes.cpp`: 34 группы, 0 расхождений); группы Casual/DM: Sigma `0x640048`, Delta `0x1010B4`, Hostage `0x90F00`, Dust 24/7 `0x2`. Sigma «не подбиралась» из-за отсутствия сервера с картой Sigma в реестре, **не** из-за ошибки декодирования/matcher.
- **Arms Race и Demolition в актуальном клиенте существуют только в War Games (Skirmish, eGame 12)** — клиент никогда не шлёт eGame 4/5 (CONFIRMED кодом композера; соответствие логам 131084/393228/524300 = Arms Race / Arms Race+Demolition / Retakes — HIGH CONFIDENCE для выбора в UI пользователя). Для eGame 12 `mask` = выбранные **режимы**: бит `1 << (id−1)`, id из `items_game.txt skirmish_modes` (armsrace 10 → `0x200`, demolition 11 → `0x400`, retakes 12 → `0x800`, …). GC шлёт `variants[{name, game_mode, maps[]}]`; backend обслуживает каждый вариант **своей категорией**: `gungameprogressive` → `armsrace`, `gungametrbomb` → `demolition`, остальное → `skirmish`; условие — категория И `server.map ∈ maps варианта`. Skirmish без `variants` (старый GC) — любой сервер категории `skirmish`. `variants` у не-Skirmish поиска → 400. Полная таблица соответствия `game_type → skirmish id → категория → maps[]` — **§55.6**.
- Причина «Demolition/Arms Race не получают сервер» (CONFIRMED по коду): раньше GC слал eGame 12 → категория `skirmish` с пустыми картами, серверы категорий armsrace/demolition не могли подойти.

### 56.7 Reservation flow (srcds) [CONFIRMED live для Accept-режимов, §46/§53]
- **`ServerGC`** на `k_EMsgGCServerHello`: `SendServerWelcome` (cookie `GameServerCookieId`) + `ReserveServerForOurCookie`: классика — plain `G<cookie>,<cookie>,1:` (одноразово; жизненный цикл §34 не решён, классические серверы при этом работают); Accept (`-gc_mode competitive|wingman|dangerzone`) — `StartAcceptTestRoster`: sniff (legacy) + `Poller` + `Controller` (backend-ростер). Резервация исполняется на host-потоке (`HostEvent::ReserveServerForQueuedGame`).
- **`Q`-резервация:** `Q<cookie>,<cookie>,1:[<acct hex>]…` (третье поле — `bReserve`, не счётчик; `…,0:` — Unreserve). Движок держит ростер на cookie; повторный `Q` с тем же cookie ростер не пересобирает (keep-alive раз в 8 с), смена ростера = Unreserve → **12 с** (движок роняет cookie после `sv_hibernate_postgame_delay`) → новый `Q`.
- **UDP (connectionless), без патчей движка:** `0x21` A2S_RESERVE_CHECK (33 байта: `FFFFFFFF`, opcode, `hostVersion=13805`, token, stage, cookie u64, steamid u64); `0x25` S2A_RESERVE_CHECK_RESPONSE (19 байт: … token, stage, `awaiting` u8, `total` u8). `awaiting` = число участников ростера со стадией меньше запрошенной; **`awaiting=127` = ростер не взведён / аккаунта в нём нет**; движок отбрасывает пакеты IP при >300/30 с (`sv_max_queries_sec=10`, окно 30) — поэтому fake-драйвер шлёт по одному пакету на fake за стадию, пейсинг 150 мс.
- **Fake-участники** (`AcceptTest::FakeRoster`, `test_accept.cpp`): id `0xFA4E0000 + n`; фазы Probe (stage 1, повтор через 1,5 с, ≤5 попыток → `TIMEOUT` → re-arm через 30 с) → WaitPopup → Accept (через 2500 мс после попапа, stage 2) → Done (re-arm через 90 с, если никто не подключился); `OnMatchStarted`/`OnMatchEnded` (подключение/выход последнего клиента) ставят паузу / Unreserve+перевзвод. **Fake-пакеты обязаны идти на LAN-адрес srcds, который он слушает** (§53: с `-ip 192.168.1.150` работает во всех трёх режимах; loopback `127.0.0.1` по умолчанию без `-ip` — вероятная причина регресса §49, механизм не доказан: HIGH CONFIDENCE, нет контрольного прогона).
- Клиент **без patch движка** видит `awaiting` из `0x25`; хук `ServerReserved` (`sub_103FB5C0`) установить нельзя («unexpected prologue») — признак попапа судим по `PlaySoundEffect('popup_accept_match_found')` / `RaiseReadyUp` (прокси).

### 56.8 Accept flow [CONFIRMED live §46/§53 старым DLL с legacy-ростером]
Последовательность (Competitive/Wingman/DZ; Accept-режимы клиента {8,9,10,11,13}, GC реализует {8,10,13}): assignment → 9107 #1 (reservation) → клиентский handler `sub_103F49F0` создаёт callback (stage 1, mode 0) → 0x21 stage 1 раз/с → **`0x25 stage=1 awaiting=0` → state 4 → popup Match Found** (`popup_accept_match_found`, `RaiseReadyUp(1,0,N)`) → Accept → 0x21 stage 2 (`RaiseReadyUp(1,k,N)`, `awaiting` падает по мере принятия fake) → `0x25 stage=2 awaiting=0` → GC шлёт **9107 #2** (без reservation; callback stage 2 mode 2 → `popup_accept_match_confirmed`) → `QueueConnectToServer` → `connect` → карта/режим. `awaiting=127` на первой проверке → callback `state=3` → уничтожается → UI зависает на «Confirming match» (наблюдалось; известное ограничение §46.9 п.1). Тайминги live (§53): stage 1 → попап за <0,4 с, Accept → второй 9107 через 3–5 с, connect ещё +2 с.

### 56.9 Fake players и roster (backend-driven) [Java CONFIRMED тестами; srcds-часть CONFIRMED офлайн/заглушками, live нет]
- **Модель (Java, §54.3/§55):** таблица `fake_search` (режим Accept-категории, `players` 1…required−1, `maps[]` пусто=любая, `enabled`, статус, `match_id`). Fake-поиск — **виртуальные matchmaking-участники backend**, не GC-клиенты: сам матч не открывает и сервер не занимает; присоединяется к `FORMING` матчу с реальным игроком, если категория совпала, `server.map` ∈ его карты и он «влезает» в остаток мест; несколько складываются; STOPPED/disabled не участвуют; после конца матча `COMPLETED` (Start ставит заново); глобальный выключатель `backend.fake-players.enabled` (false → API 409, matcher игнорирует). Пример: fake 9 × Competitive `[de_dust2]` + real Competitive `[de_dust2]` → один матч 10 = 1 + 9.
- **`required_players` — размер матча (10/4/16), не число fake**; `real_players + fake_players = required_players`; `backend.required-players.*` — только тестовый override (пользовательские `=1` **отключают** fake-поиски: матч полон с 1 игроком).
- **`assignment.players[]`:** реальные первыми, затем fake `0xFA4E0000 + n` (n — порядок вступления, та же схема, что у srcds-драйвера; live `Q…[3e9846d5][fa4e0001]…`).
- **Канал srcds (§55):** `GET /api/v1/servers/roster?address=&port=` (раз/с, `RosterFeed::Poller`) → `RosterFeed::Controller`: матч `READY` + ровно столько участников, сколько у режима srcds + есть реальный + `accept_required` + режим совпал → взвести **точно этим списком** (`FakeRoster` с `participants`; если уже взведён другой — Unreserve+12 с); тот же список уже взведён → не пересобирать; все fake на stage 1 → `POST /servers/roster/ready` (один раз на матч); смена ростера откладывается, пока на сервере игрок.
- **Удержание assignment (backend):** Accept-матч на сервере, недавно (`roster-poll-window` PT10S) спрашивавшем ростер, остаётся у игрока `MATCHED` (`match.awaiting_server=true`) до `ready`, `roster-ack-timeout` (PT25S) или молчания сервера → первая проверка клиента должна дать `awaiting=0` (устраняет «первая попытка `awaiting=127`», **только на заглушке проверено**). Сервер, который ростер не спрашивает, и классика не удерживаются.
- **Legacy fallback [CONFIRMED live для старого пути]:** srcds-локальный ростер (`StartAcceptTestRoster` → sniff первого 0x21 через хук `WSARecvFrom` → `CreateTestRoster`: `[real][fake 1..N−1]`, размер по режиму) остаётся; включается, если нет backend_url / нет матча на сервере / backend недоступен / ростер не того размера (в т.ч. override `required-players=1`) / адрес srcds не совпал с реестром. Тогда поведение прежнее (первая попытка `awaiting=127`, вторая работает).

### 56.10 API (актуальные эндпоинты) [CONFIRMED кодом и тестами]
GC/srcds-facing (`/api/v1`, заголовок `X-Api-Key`; пустой ключ в конфиге — search/cancel открыты):
| Эндпоинт | Кто | Назначение |
|---|---|---|
| `GET /health` | публично | живость |
| `POST /matchmaking/search` | клиент GC | поиск (`account_id, game_type, [mode, game_mode, maps[], request_id, variants[]]`); идемпотентен на аккаунт; ответ `{result: created\|refreshed\|replaced, search}` |
| `GET /matchmaking/search/{request_id}` | клиент GC (раз/с) или админ | состояние поиска + `assignment`; обновляет `last_seen_at` (heartbeat); 404 — backend поиска не знает |
| `POST /matchmaking/cancel` | клиент GC (9102) | отмена; запоздалый cancel старого `request_id` не снимает новый поиск |
| `GET /matchmaking/searches[?include_finished]` | админ-панель / ключ | список заявок, запускает таймеры |
| `POST /servers/state` | сервер/скрипт | отчёт `{address, port, map?, state? AVAILABLE\|BUSY}`; только зарегистрированные; `AVAILABLE` не снимает RESERVED (вызывающей стороны пока нет) |
| `GET /servers/roster` | srcds (раз/с) | состав ближайшего FORMING/READY матча сервера (`players[]` с `fake`); 404 нет матча/не зарегистрирован; вызов = «читаю ростер» |
| `POST /servers/roster/ready` | srcds | `{address, port, match_id}` «ростер взведён» → игроки получают assignment |
Админ (`/admin/**`, сессия + CSRF, форма входа, блокировка по IP после неудач): страницы `/admin`, `/admin/login`; JSON `/admin/api/`: `categories`, `servers` (CRUD, `/{id}/enabled`, `/{id}/state` — Set busy/available/Release), `matches`, `searches` (тестовая заявка `POST`, `DELETE /{id}`), **`fake-searches`** (`GET {enabled, fake_searches}`, `POST`, `PUT /{id}`, `POST /{id}/enabled` = Stop/Start, `DELETE /{id}`). Панель: вкладки Active Searches, Game Servers, Matches, **Fake Players**. Доступ: всё вне перечисленного — `denyAll` (при добавлении нового пути его нужно явно разрешить в `SecurityConfig` — уже ловилось тестом).

**Конфигурационные параметры backend (`application.properties` / переменные окружения `BACKEND_…`, значения по умолчанию)** [CONFIRMED кодом `BackendProperties`]: `server.address` (127.0.0.1) / `server.port` (8080); `backend.admin.username` / `password` (обязательны); `backend.api-key` (пусто = search/cancel открыты); `backend.database-path` (`./data/matchmaking.db`); `stale-search-timeout` PT15M; `reaper-interval` PT10S; `finished-search-retention` PT1H; `matcher-interval` PT1S; `server-reservation-ttl` PT10M; `assigned-search-timeout` PT2M; `required-players.<competitive\|wingman\|dangerzone>` (по умолчанию 10/4/16; тестовый override); **`roster-ack-timeout` PT25S**; **`roster-poll-window` PT10S**; **`fake-players.enabled` true**; `login.max-failures/failure-window/lock-duration` (5 / PT5M / PT5M); `server.servlet.session.timeout` 30m, `…cookie.secure` false. **Параметры srcds / клиента (в общем `csgo_gc\config.txt` и командной строке):** `matchmaking.backend_url`, `matchmaking.backend_api_key`, `log_output` (2 = консоль + `gc_log.txt`), `matchmaking.test_diag` (необязательный, по умолчанию 1); командная строка srcds: `-gc_mode competitive\|wingman\|dangerzone` (включает Accept-ростер), `-ip <адрес>`, `-port <порт>`. **Константы драйвера (в коде, не в конфиге):** `PacketGap` 150 мс, `RetryTimeout` 1500 мс, `MaxAttempts` 5, `FirstProbeDelay` 1500 мс, keep-alive `Q` 8 с, `GaveUpRearmDelay` 30 с, `DoneRearmDelay` 90 с, `UnreserveCooldown` 12 с, задержка Accept fake 2500 мс; опрос ростера 1 с, таймауты HTTP 1,5 / 3 с, `MaxConfirmAttempts` 8.

### 56.11 Что проверено и чем
**(A) Проверено в настоящей игре (логи пользователя):** [CONFIRMED]
- Accept-flow Competitive/Wingman/DZ с legacy fake-драйвером до `connect` (§46) и после переноса выбора сервера на backend **при `-ip 192.168.1.150` на srcds** (§53: fake на stage 1, `awaiting=0`, попап, Accept, второй 9107, `QueueConnect`, `Connected`, INGAME на 27017/27018/27019; первая попытка — `awaiting=127`, вторая — успех).
- Casual и Deathmatch: `9101 → backend → assignment → обычный connect` (§49/§50), DLL §49 + jar до-§54.
- Клиентская цепочка после assignment идентична рабочему §46 (9107 поля, callback, 0x21).
- Без `-ip` (адрес fake = `127.0.0.1`) в §49 наблюдались `awaiting=N-1` / `127` / отсутствие 0x25; связь с адресом — HIGH CONFIDENCE (контроль не делался).
**(B) Проверено только тестами/заглушками/офлайн:**
- **Java: 82 теста, все проходят** (`mvnw test`; классы: `BackendIntegrationTest` 15, `SchemaMigrationTest` 1, `MatchmakingIntegrationTest` 21, `FakePlayersIntegrationTest` 17, `FakePlayersDisabledTest` 1, `MapGroupMatchingTest` 12, `BackendDrivenRosterTest` 15). Покрыто: авторизация/CSRF/API-key, поиски и таймауты, выбор сервера (категория/карта/disabled/BUSY), reservation split, fake-поиски, roster/ready/удержание, skirmish variants и группы карт на реальных списках `gamemodes.txt`, миграция БД.
- **C++ офлайн** (`offline_tests\roster`, `build.bat`): `controller_test.exe` — decision table `RosterFeed::Controller` ALL PASSED; `roster_e2e.exe` — настоящие `BackendClient`+`Poller`+`Controller`+`FakeRoster` против настоящего backend и UDP-заглушки движка: S1 (real+9 fake: удержание → взвод → confirm → первая проверка `awaiting=0`), S2 (override `required-players=1` → fallback без удержания), S3 (legacy без списка — прежний payload и ход).
- `sk_test.exe` (настоящие `mm_modes.cpp`+`keyvalue.cpp` на настоящих `items_game.txt`/`gamemodes.txt`): маски Skirmish 0x200/0x400/0x600/0x800 → armsrace/demolition/оба/retakes; `check_map_groups.py` (34 группы, 0 расхождений); сквозной прогон настоящего клиентского кода со Skirmish `game_type` 393228/524300/262156 против backend (Arms Race → `ar_shoots`, Demolition → `de_bank`, Retakes → skirmish-сервер).
- Код панели прогнан в node на реальных ответах API (ошибок нет). **Визуально в браузере вкладки Fake Players не смотрели** [?] (вход требует пароля).
**(C) НЕ проверено в настоящей игре / на настоящем движке** [UNRESOLVED до теста]: всё, что появилось с §54 и §55: DLL 98c794… и новый jar вообще не запускались пользователем; reservation split вживую (сервер Casual остаётся Available; несколько игроков); Skirmish/War Games и серверы Arms Race/Demolition; Fake Players через панель; **srcds, читающий ростер с backend, и удержание assignment против настоящего `engine.dll`** (смена ростера: Unreserve+12 с против `roster-ack-timeout` 25 с); матч с ≥2 реальными игроками; поведение клиента при долгом «MATCHED».

### 56.12 Известные баги, ограничения, открытые вопросы
1. **Первая попытка после старта srcds — `awaiting=127`, UI висит на «Confirming match»** (legacy-ростер строится по первому 0x21). [CONFIRMED; архитектурный фикс §55 реализован, live не проверен] Поэтому legacy остаётся.
2. **`-ip` на Accept-srcds обязателен** для fake-пакетов (иначе `127.0.0.1`): без него в §49 fake не доходили до stage 1. [CONFIRMED что с `-ip` работает; причина без `-ip` — HIGH CONFIDENCE; механизм — HYPOTHESIS; контрольный прогон без `-ip` не делался]. Адрес/порт srcds должны совпасть с записью реестра, чтобы backend-ростер заработал.
3. **Метка карты сервера в реестре ≠ реальная карта srcds** (backend не управляет `changelevel`, heartbeat нет). [CONFIRMED] Отсюда «failed connect» при ручной правке метки (Casual `de_inferno`, DM `cs_agency`, §50.4; причина silence-на-`changelevel` — HYPOTHESIS) и то, что клиент карту 9107 не валидирует.
4. **Backend не знает о конце матча** (нет heartbeat srcds): освобождение RESERVED — TTL 10 мин / «новый поиск одиночки» / админ; cancel назначенной заявки (9102 при connect) сервер не освобождает; классические матч-записи закрываются по TTL. **Вместимость `max_players` не учитывается** — классический сервер может получить сколько угодно игроков. [CONFIRMED]
5. **Смена ростера на srcds стоит ≥12 с** (Unreserve + cooldown) при том, что backend ждёт ack ≤25 с; на настоящем движке не проверялась. [HYPOTHESIS о достаточности запаса]
6. **Скрытый риск override:** пока в `application.properties` `required-players.*=1`, fake-поиски не участвуют, srcds работает по legacy. [CONFIRMED]
7. **Хук `ServerReserved` не устанавливается** («unexpected prologue») — попап судим по звуку/`RaiseReadyUp`. [CONFIRMED]
8. **`gc_log.txt`** — общий для всех процессов в одной папке и **удаляется при старте каждого процесса** с этим DLL (`Platform::Initialize`): запускать клиент до srcds, копировать лог сразу после теста; `console.log` клиента накопительный. Консоль srcds на диске не сохраняется. [CONFIRMED]
9. **[UNRESOLVED]** Первый прогон Competitive в §53: 0x21 на 192.168.1.150:27017 без единого 0x25 (srcds-лога прогона нет); `OnSessionFailed: Timed out attempting to connect` в конце srcds-логов (происхождение неизвестно, к резервации отношения не видно).
10. **[UNRESOLVED]** Серверный режим srcds для Arms Race/Demolition/Skirmish (`+game_type/+game_mode`, skirmish-конфиги `server_exec` из `items_game.txt`) live не подтверждён; клиент к таким серверам не подключался.
11. Не поддерживаются: ScrimComp5v5, Cooperative (backend 400). `http://` без TLS, API-ключ открытым текстом в `config.txt`; ветки Linux/macOS не компилировались.
12. Cookie резервации — константа сборки (`GameServerCookieId`), одинаковая DLL нужна и клиенту, и srcds; жизненный цикл `G`-резервации (§34) не решён; один srcds = один сервер реестра = один Accept-матч и один режим (`-gc_mode`).
13. Мелочи: пустой каталог `java-backend\src\main\java\dev\csgogc\mm\match\` (остаток, безвреден); каталог `offline_tests\roster\build\` — результат сборки; RE-артефакты (IDA-базы `client.dll.i64`, `q45_gametype_composer.txt`, скрипты `check_map_groups.py`, `kv_gamemodes.py`) лежат во **временном** scratchpad сессии, не в репозитории: выводы из них записаны в §44.5/§54/§55, при потере воспроизводятся (`sub_10288A90`/`sub_10288960` в client.dll build 1352, sha1 `53ba71b7…ddda9`).

### 56.13 Поправки к более ранним выводам (старые записи не удалены; актуально — то, что здесь)
| Старое утверждение | Где | Актуально |
|---|---|---|
| Главный checkpoint = §46 (архитектура: выбор сервера/ростер в C++/config) | §46 | **§56**; выбор сервера — Java (§49), ростер — backend (§55) |
| Матчер резервирует сервер для всех режимов (`RESERVED`) | §49.4, §50.5 | RESERVED только для Accept-режимов, классика не резервирует (§54.1) [CONFIRMED тестами] |
| Skirmish: маска не разобрана / `1<<(index−1)` по 4 группам не сходится | §44.5, §49.8 п.5, §50.4 | маска = выбранные режимы, `1 << (id−1)`, id из `items_game.txt skirmish_modes`; Arms Race/Demolition приходят только как Skirmish (§54.2, таблица §55.6) [CONFIRMED кодом, привязка к UI — HIGH CONFIDENCE] |
| Причина «Demolition не получает сервер» — UNRESOLVED / нет логов | §50.4 | GC слал категорию `skirmish` без карт; исправлено variants (§54.2) [CONFIRMED кодом] |
| Sigma/Demolition — возможный баг matcher/декодирования | запрос пользователя после §50 | декодирование и сопоставление верны (34/34 групп); причина — нет серверов нужной категории/карты (§54.2) [CONFIRMED] |
| Fake roster строится только на srcds и Java его не знает | §49.8 п.1, §50.3 | backend хранит fake-поиски и состав матча, srcds читает его и взводит по нему; legacy — fallback (§55) [CONFIRMED тестами/заглушкой] |
| Регресс §49 — HIGH CONFIDENCE (адрес fake) | §50.2, §51.3 | практически подтверждён: с `-ip 192.168.1.150` работает во всех режимах (§53); строгое доказательство — контрольный прогон без `-ip` (не сделан) |
| §49.8 п.4: шлёт ли клиент 9102 при подключении — не подтверждено | §49.8 | шлёт (Accept-режимы, ~2 с после QueueConnect) (§53.5) [CONFIRMED] |
| §49.8 п.7: адрес fake `-ip`/loopback не проверен на движке | §49.8 | LAN-адрес (`-ip`) проверен; loopback live не проверялся |
| `awaiting=127` в §50 — «сигнатура отказа» | §50.2 | на **первой** попытке свежего srcds это норма legacy-ростера (как в §46); отказ — это N−1/TIMEOUT после неё (§52.1, §53.3) |
| `test_real_account_id`, `test_server_*`, `test_accept_mode` в конфиге клиента | §26–§46 | удалены (§49); остался `-gc_mode` в командной строке srcds |
| §50.2 «srcds-вывода на диске нет» | §50.1 | он пишется в `gc_log.txt` рабочей папки процесса, но стирается стартом следующего процесса (§52.1) |

### 56.14 Точный следующий этап (игровой тест §55.7; реализацию не начинать без подтверждения)
1. **Подготовка (пользователь):** остановить/не запущенный backend → `java-backend\build.cmd` → `run.cmd` (миграция БД сама добавит `variants` и `fake_search`); из `config\application.properties` **удалить три строки `backend.required-players.*=1`**; скопировать `Build\release\csgo_gc\csgo_gc.dll` (98c794…) в игру **и** в папку srcds (это одна папка `csgo legacy`) с бэкапом текущего; запуск: сначала клиент, потом srcds (`gc_log.txt`).
2. **Команды srcds** (из старого конфига, точных .bat нет): Competitive `srcds.exe -game csgo -console -ip 192.168.1.150 -port 27017 -gc_mode competitive +game_type 0 +game_mode 1 +map de_dust2`; Wingman `… -port 27018 -gc_mode wingman +game_type 0 +game_mode 2 +map de_lake`; DZ `… -port 27019 -gc_mode dangerzone +game_type 6 +game_mode 0 +map dz_blacksite`; классика без `-gc_mode`: Casual `-port 27016 +game_type 0 +game_mode 0`, DM `-port 27014 +game_type 1 +game_mode 2`.
3. **Тест 1 — backend-ростер (главный):** панель → Fake Players: Competitive, 9 игроков, карта = карта сервера; обычный поиск. Ожидать в логах srcds строки из §55.7 (`backend roster … READY … 10 participants (1 real + 9 fake)`, `arming the roster of backend match`, `[FAKE-MM] started … source=backend match`, `roster … armed … telling the backend`, `armed -> confirmed`), у клиента `MATCHED 10/10` → `WAITING_ACCEPT` → **первая** `0x25 stage=1 awaiting=0 total=10` → попап → Accept → `awaiting=0` → connect. Затем Wingman (3 fake) и DZ (15 fake). Сохранить `gc_log.txt` каждого srcds и `console.log`.
4. **Тест 2 — регрессия классики и fallback:** Casual/Deathmatch (в панели сервер остаётся Available, второй поиск получает тот же сервер); Accept без fake-поисков и с override `=1` (legacy: первая попытка `awaiting=127`, вторая успех).
5. **Тест 3 — Skirmish:** завести серверы категорий Arms Race (карта ∈ `mg_skirmish_armsrace`) и Demolition (∈ `mg_skirmish_demolition`), реальные srcds в соответствующих режимах; War Games → Arms Race / Demolition; в консоли `[MM] mode=skirmish … skirmish modes armsrace(7 maps),…`, в панели Maps `armsrace [armsrace]`.
6. **После результатов:** разбор логов → §57; при успехе теста 1 — отдельным решением удалить legacy `FakeRoster`/sniff и override; затем (по приоритету, каждый — отдельный план): heartbeat srcds и конец матча в backend; управление реальной картой srcds (`changelevel` / отчёт реальной карты); учёт вместимости классических серверов; контрольный прогон без `-ip` (закрыть механизм регресса); ≥2 реальных игрока в одном матче.

### 56.15 Важные файлы и точки интеграции
- **C++ (`csgo_gc\`):** `gc_client.cpp` (`OnMatchmakingStart`, `OnBackendSearchResult`, `StartServerFlow`, декодирование Skirmish, `GameModesFile`); `backend_client.{h,cpp}` (клиент HTTP: поиск, `FetchServerRoster`, `ConfirmServerRoster`); `mm_modes.{h,cpp}` (таблица режимов, битовые таблицы карт, `DecodeMapSelection`, `ParseSkirmishModes`, `DecodeSkirmishSelection`); `item_schema.{h,cpp}` + `inventory.h` (`Schema()`, `SkirmishModes()`); `gc_server.{h,cpp}` (`ServerGC`: резервация, sniff, `OnBackendRoster`, `ArmRoster`); `server_roster.{h,cpp}` (`RosterFeed::Poller`, `Controller`, `Snapshot`); `test_accept.{h,cpp}` (`FakeRoster`, payload `Q`, хук `WSARecvFrom` — sniff 0x21 на srcds / наблюдатель 0x25 на клиенте); `test_diag.*` (`[MM-DIAG]` клиента); `gc_shared.h` (`GCEvent::BackendSearchResult, BackendRoster, TestFakesReady`); `config.{h,cpp}` (`BackendUrl/BackendApiKey`, `DedicatedServerAddress()` = `-ip` иначе `127.0.0.1`, `DedicatedServerPort()` = `-port` иначе 27015, `TestAcceptMode` = `-gc_mode`, `TestFakeAcceptDelayMs()` = 2500); `platform_windows.cpp` (`gc_log.txt`).
- **Java (`java-backend\src\main\java\dev\csgogc\mm\`):** `search/SearchService` (matcher, reservation split, fake join, roster, удержание/ack), `search/{SearchRepository, MatchRepository, SearchApiController, SearchReaper, SearchRequest, SearchVariant, SearchView, RosterEntry}`; `fake/*` (модель fake-поисков); `server/*` (реестр и состояния); `mode/ModeCategory`; `admin/*` + `resources/admin/index.html`, `static/admin/assets/{admin.js, admin.css}`; `config/*` (`BackendProperties`, `SecurityConfig`, `SchemaInitializer`, `ApiKeyFilter`); `resources/schema.sql`; `README.md`, `config/application-example.properties`.
- **Офлайн-тесты:** `offline_tests\roster\{build.bat, README.md, controller_test.cpp, roster_e2e.cpp, stdafx.h, config.h, funchook.h}`; Java — `src\test\java\dev\csgogc\mm\*`.
- **Данные игры (источники истины):** `csgo\gamemodes.txt` (группы карт, режимы), `csgo\scripts\items\items_game.txt` (`skirmish_modes`), `client.dll` build 1352 (`sub_10288960` режим→eGame, `sub_10288A90` composer, `sub_103F49F0` handler 9107, `sub_103DC4B0` job 9107, `sub_103EF770` Accept-режимы).
- **Логи:** клиент `…\csgo legacy\csgo\console.log` (накопительный, `[GC]…`, `[MM-DIAG]`), `…\csgo legacy\gc_log.txt` (общий, стирается стартом процесса), backend `java-backend\data\backend-out.log` (решения matcher'а).

### 56.16 Окружение и приёмы (чтобы не терять время)
- Портативные инструменты в `D:\csgo2021_gc\tools\` (msvc, cmake, ninja, vcpkg, jdk21, maven). Сборка DLL: `D:\csgo2021_gc\build_local.bat` (результат в `Build\release\csgo_gc\`); Java: `java-backend\build.cmd` (jar) / `.\mvnw.cmd -B test` (тесты; **не** `package`, если backend запущен — jar занят); запуск backend: `java-backend\run.cmd`; офлайн C++: `offline_tests\roster\build.bat`.
- В окружении пользователя `HTTP_PROXY=127.0.0.1:10809` (TUN-прокси): для `curl` к локальному backend использовать `--noproxy '*'`; наш C++ HTTP-клиент — сырые сокеты, прокси не участвует. `NoDefaultCurrentDirectoryInExePath=1` (скрипты вызывать как `.\name.cmd`). PowerShell-скрипты блокирует execution policy при запуске через `powershell -File`; инструмент PowerShell запускает их через `&`. Heredoc в Bash-инструменте с кавычками/обратными слэшами ломается — файлы писать инструментом Write, правки — python-скриптом из файла. `Remove-Item` с маской блокируется.
- Приватные экземпляры backend для тестов — порт 18090, отдельная БД, ключ `test-api`, не трогать порт 8080 пользователя.

### §56 STATUS
Актуальный checkpoint записан. Реализовано и подтверждено тестами (Java 82/82, C++ офлайн): backend-выбор сервера, reservation split, Skirmish variants, fake players на backend, `assignment.players[]`, канал ростера srcds ↔ backend с удержанием assignment, legacy fallback. **Подтверждено в настоящей игре:** Accept-flow трёх режимов (legacy-ростер, `-ip 192.168.1.150`), Casual/DM через backend (старые DLL/jar). **Не проверено в игре:** всё, что новее §53 (DLL 98c794…, jar после §54). Следующий шаг — игровой тест 56.14, пункт 3.
