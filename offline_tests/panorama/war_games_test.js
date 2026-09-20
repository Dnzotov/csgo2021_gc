// Offline test of RESEARCH_FINDINGS.md #62: War Games ("skirmish") is gone from the Play menu, nothing else changed.
//
//   node war_games_test.js <patched code.pbin> [<original code.pbin>]
//
// It reads scripts\mainmenu_play.js straight out of the pbin(s), takes the real functions that decide which game mode
// radio buttons exist (_IsGameModeAvailable, _ValidateSessionSettings, ...) and runs them in a vm with stubbed Panorama
// APIs. With the original pbin it also proves the harness sees the old behaviour (War Games available), and that the
// patched pbin differs from it in mainmenu_play.js only.
"use strict";
const fs = require("fs");
const vm = require("vm");

let checks = 0, failed = 0;
function check(cond, what) {
    checks++;
    if (!cond) { failed++; console.log("  FAILED: " + what); }
}

// ---- pbin: "PAN\x02" + 512 byte RSA block + stored "PK\3\4" entries + tail --------------------------------------------
function parsePbin(path) {
    const d = fs.readFileSync(path);
    if (d.toString("latin1", 0, 4) !== "PAN\x02") throw new Error(path + ": not a pbin");
    let pos = 516;
    const entries = [];
    while (d.toString("latin1", pos, pos + 4) === "PK\x03\x04") {
        const csize = d.readUInt32LE(pos + 18);
        const nlen = d.readUInt16LE(pos + 26);
        const elen = d.readUInt16LE(pos + 28);
        const name = d.toString("utf8", pos + 30, pos + 30 + nlen);
        const start = pos + 30 + nlen + elen;
        entries.push({ name, header: d.subarray(pos, pos + 30), body: d.subarray(start, start + csize) });
        pos = start + csize;
    }
    return { size: d.length, entries, tail: d.subarray(pos) };
}

function entryText(pbin, name) {
    const e = pbin.entries.find(x => x.name === name);
    if (!e) throw new Error("missing " + name);
    return e.body.toString("latin1");
}

// ---- pull the real functions out of the script ----------------------------------------------------------------------
function extractFunction(src, name) {
    const at = src.search(new RegExp("function\\s+" + name + "\\s*\\("));
    if (at < 0) throw new Error("function " + name + " not found");
    const open = src.indexOf("{", at);
    let depth = 0, i = open;
    for (; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0) break;
    }
    return src.slice(at, i + 1);
}

const NEEDED = [
    "_SetGameModeRadioButtonVisible", "_SetGameModeRadioButtonAvailableTooltip", "_IsValveOfficialServer",
    "_IsGameModeAvailable", "_IsSingleSkirmishString", "_GetSingleSkirmishIdFromSingleSkirmishString",
    "_GetSingleSkirmishMapGroupFromId", "_GetSingleSkirmishMapGroupFromSingleSkirmishString",
    "_LoadGameModeFlagsFromSettings", "_ValidateSessionSettings",
];

const RADIO_IDS = ["competitive", "scrimcomp2v2", "casual", "deathmatch", "survival", "skirmish", "cooperative", "coopmission"];

function makeMenu(source, opts) {
    const radios = {};
    for (const id of RADIO_IDS) radios[id] = { id, visible: true, SetPanelEvent() {} };
    const saved = Object.assign({}, opts.saved || {});
    const ctx = {
        radios,
        m_isWorkshop: false,
        m_serverSetting: opts.server,
        m_gameModeSetting: opts.mode,
        m_singleSkirmishMapGroup: null,
        m_gameModeFlags: {},
        $: (sel) => sel === "#GameModeSelectionRadios"
            ? { FindChildInLayoutFile: (id) => radios[id] || null }
            : null,
        LobbyAPI: { BIsHost: () => true, GetSessionSettings: () => ({ game: { mode: opts.mode } }) },
        MyPersonaAPI: { HasPrestige: () => !opts.newUser, GetCurrentLevel: () => (opts.newUser ? 1 : 40) },
        GameInterfaceAPI: { GetSettingString: (key) => (key in saved ? saved[key] : "") },
        GameModeFlags: { DoesModeUseFlags: () => false, AreFlagsValid: () => true },
        UiToolkitAPI: { ShowCustomLayoutParametersTooltip() {}, HideCustomLayoutTooltip() {} },
        GetMatchmakingQuestId: () => 0,
        MissionsAPI: { GetQuestDefinitionField: () => "" },
        _setAndSaveGameModeFlags() {},
        parseInt,
    };
    vm.createContext(ctx);
    vm.runInContext(NEEDED.map((n) => extractFunction(source, n)).join("\n\n"), ctx);
    return ctx;
}

const isAvailable = (src, server, mode, extra) => {
    const c = makeMenu(src, Object.assign({ server, mode }, extra));
    return { result: c._IsGameModeAvailable(server, mode), radio: c.radios[mode] };
};

function fallbackMode(src, server, startMode, saved, extra) {
    const c = makeMenu(src, Object.assign({ server, mode: startMode, saved }, extra));
    c._ValidateSessionSettings();
    return { mode: c.m_gameModeSetting, group: c.m_singleSkirmishMapGroup, ctx: c };
}

// ---- run -----------------------------------------------------------------------------------------------------------
const patchedPath = process.argv[2];
const originalPath = process.argv[3];
if (!patchedPath) { console.log("usage: node war_games_test.js <patched code.pbin> [<original code.pbin>]"); process.exit(2); }

const patchedPbin = parsePbin(patchedPath);
const src = entryText(patchedPbin, "panorama\\scripts\\mainmenu_play.js");

console.log("14. War Games is not offered (Online = official, Offline = listen, new player included)");
for (const server of ["official", "listen"]) {
    for (const extra of [{}, { newUser: true }]) {
        const r = isAvailable(src, server, "skirmish", extra);
        check(r.result === false, `${server}${extra.newUser ? "/new user" : ""}: _IsGameModeAvailable(skirmish) === false`);
        check(r.radio.visible === false, `${server}${extra.newUser ? "/new user" : ""}: the skirmish radio button is hidden`);
    }
}

console.log("    the other modes are as before");
for (const mode of ["competitive", "scrimcomp2v2", "casual", "deathmatch"]) {
    for (const server of ["official", "listen"]) {
        const r = isAvailable(src, server, mode, {});
        check(r.result === true, `${server}: ${mode} available`);
        check(r.radio.visible === true, `${server}: ${mode} radio button stays visible`);
    }
}
check(isAvailable(src, "official", "survival", {}).result === true, "official: survival (Danger Zone) available");
check(isAvailable(src, "listen", "survival", {}).result === false, "listen: survival still online-only");
check(isAvailable(src, "official", "competitive", { newUser: true }).result === false, "new player (level < 2): competitive still locked, as before");
check(isAvailable(src, "official", "casual", { newUser: true }).result === true, "new player: casual available");
check(isAvailable(src, "official", "deathmatch", { newUser: true }).result === true, "new player: deathmatch available");

console.log("15. a saved War Games selection falls back to another mode");
for (const server of ["official", "listen"]) {
    // current setting skirmish, saved setting skirmish -> the first available mode of the list (deathmatch)
    let r = fallbackMode(src, server, "skirmish", { ["ui_playsettings_mode_" + server]: "skirmish" });
    check(r.mode === "deathmatch", `${server}: skirmish + saved skirmish -> deathmatch (got ${r.mode})`);
    check(r.group === null, `${server}: no single skirmish map group is left behind`);

    // saved setting is a single skirmish button ("skirmish_armsrace"): it is skirmish as well -> fallback
    r = fallbackMode(src, server, "skirmish", { ["ui_playsettings_mode_" + server]: "skirmish_armsrace" });
    check(r.mode === "deathmatch", `${server}: skirmish + saved skirmish_armsrace -> deathmatch (got ${r.mode})`);
    check(r.group === null, `${server}: single skirmish map group cleared`);

    // saved setting is a fine mode: it wins
    r = fallbackMode(src, server, "skirmish", { ["ui_playsettings_mode_" + server]: "casual" });
    check(r.mode === "casual", `${server}: skirmish + saved casual -> casual (got ${r.mode})`);
    r = fallbackMode(src, server, "skirmish", { ["ui_playsettings_mode_" + server]: "competitive" });
    check(r.mode === "competitive", `${server}: skirmish + saved competitive -> competitive (got ${r.mode})`);

    // an available current mode is not touched
    r = fallbackMode(src, server, "casual", { ["ui_playsettings_mode_" + server]: "skirmish" });
    check(r.mode === "casual", `${server}: casual stays casual (got ${r.mode})`);
}
// new player: the fallback list must not end up with nothing
{
    const r = fallbackMode(src, "official", "skirmish", { ui_playsettings_mode_official: "skirmish" }, { newUser: true });
    check(r.mode === "deathmatch", `new player: skirmish -> deathmatch (got ${r.mode})`);
}

// ---- the container ---------------------------------------------------------------------------------------------------
console.log("    package: same size, same entries, only mainmenu_play.js differs from the original");
if (originalPath) {
    const orig = parsePbin(originalPath);
    check(orig.size === patchedPbin.size, `size ${orig.size} == ${patchedPbin.size}`);
    check(orig.entries.length === patchedPbin.entries.length, "entry count");
    const differing = [];
    orig.entries.forEach((e, i) => {
        const p = patchedPbin.entries[i];
        if (e.name !== p.name) differing.push("ORDER:" + e.name);
        else if (!e.body.equals(p.body)) differing.push(e.name);
    });
    check(differing.length === 1 && differing[0] === "panorama\\scripts\\mainmenu_play.js", "only mainmenu_play.js differs: " + JSON.stringify(differing));
    check(orig.tail.equals(patchedPbin.tail), "tail identical");

    // harness sensitivity: the ORIGINAL script does offer War Games
    const osrc = entryText(orig, "panorama\\scripts\\mainmenu_play.js");
    const o = isAvailable(osrc, "official", "skirmish", {});
    check(o.result === true, "harness: the original script offers War Games (official)");
    check(isAvailable(osrc, "listen", "skirmish", {}).result === true, "harness: the original script offers War Games (listen)");
    const of = fallbackMode(osrc, "official", "skirmish", { ui_playsettings_mode_official: "skirmish" });
    check(of.mode === "skirmish", "harness: the original script keeps a saved skirmish");

    // the layout is untouched (the button itself is hidden by the script, not removed)
    check(entryText(orig, "panorama\\layout\\mainmenu_play.xml") === entryText(patchedPbin, "panorama\\layout\\mainmenu_play.xml"), "mainmenu_play.xml identical");
}
check(/id='skirmish'/.test(entryText(patchedPbin, "panorama\\layout\\mainmenu_play.xml")), "the radio button is still declared in the layout (hidden by script)");

console.log(`\n${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
