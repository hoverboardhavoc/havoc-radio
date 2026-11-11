// The radio web page's functions under Node, from the page's <script id="lib">, with the English strings from the
// prototype's table: the RDS rendering on a state as GET /api/state sends it (radio/test/rds/state-99.5.json, the
// fields decoded from the 99.5 MHz bench capture), the Radio and Settings views by the location hash, the header
// clock, the Time card and its controls posting /api/time, and the frequency guide: the Radio view's table, the
// Settings > Guide card's update against a fake manifest and table, and From file; the event stream's reopening.
//
//   node test_page.js ROOT
"use strict";
process.env.TZ = "Europe/Oslo";  // the browser's zone for the local times
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const root = process.argv[2];
const page = fs.readFileSync(path.join(root, "radio/components/web/page/index.html"), "utf8");
const lib = page.match(/<script id="lib">([\s\S]*?)<\/script>/);
assert(lib, 'no <script id="lib"> in the page');
const ctx = vm.createContext({});
vm.runInContext(lib[1] + "\n;Object.assign(globalThis, { rdsHtml, showView, clockHtml, timeHtml, ageText, bindTime, " +
  "daysText, guideRows, renderGuide, guideInfoText, sha256, updateGuide, sendGuide, GUIDE_BASE, openEvents });", ctx);
// The page's other script must at least compile.
for (const m of page.matchAll(/<script>([\s\S]*?)<\/script>/g)) new vm.Script(m[1]);

const proto = fs.readFileSync(path.join(root, "radio/ui/prototype.html"), "utf8");
const en = JSON.parse(proto.match(/const STRINGS = (\{[\s\S]*?\n\});\n/)[1]).en;
const t = k => en[k] || k;

const state = JSON.parse(fs.readFileSync(path.join(root, "radio/test/rds/state-99.5.json"), "utf8"));
let checks = 0;
function eq(got, want, what) {
  checks++;
  assert.strictEqual(got, want, what);
}

const html = ctx.rdsHtml(state.rds, state.hz, t);
eq(html, '<div class="ps">NYHETER</div><div class="rt">METRO gir deg nasjonale nyheter</div>' +
  '<div class="meta"><span class="tag">News</span><span class="tag">TP</span><span class="mono">PI F395</span></div>' +
  '<div class="meta af">Also on <a href="#" data-hz="88400000">88.4</a> <a href="#" data-hz="90900000">90.9</a> ' +
  '<a href="#" data-hz="99000000">99.0</a> <a href="#" data-hz="102900000">102.9</a> ' +
  '<a href="#" data-hz="104600000">104.6</a> <a href="#" data-hz="105800000">105.8</a> ' +
  '<a href="#" data-hz="106600000">106.6</a> <a href="#" data-hz="107000000">107.0</a> ' +
  '<a href="#" data-hz="107900000">107.9</a></div>', "Metro: PS, RT, chips and the AF list without 99.5");

const r = state.rds;
eq(ctx.rdsHtml({ ...r, ta: true }, state.hz, t).includes('<span class="tag on">TP</span>'), true, "TP lit during TA");
eq(ctx.rdsHtml({ ...r, tp: false }, state.hz, t).includes(">TP<"), false, "no TP chip without TP");
eq(ctx.rdsHtml({ ...r, pty: 0, pi: null, tp: false, af: [state.hz] }, state.hz, t),
  '<div class="ps">NYHETER</div><div class="rt">METRO gir deg nasjonale nyheter</div>', "no chips, no other frequencies");
eq(ctx.rdsHtml({ ...r, sync: false }, state.hz, t), '<div class="nosig">RDS: searching</div>', "searching");
eq(ctx.rdsHtml(null, 6070000, t), "", "outside FM");
eq(ctx.rdsHtml({ ...r, ps: "A&B <i>", rt: '"€"', af: [531000, 99500000] }, state.hz, t),
  '<div class="ps">A&amp;B &lt;i&gt;</div><div class="rt">&quot;€&quot;</div>' +
  '<div class="meta"><span class="tag">News</span><span class="tag">TP</span><span class="mono">PI F395</span></div>' +
  '<div class="meta af">Also on <span>531k</span></div>', "text escaped; MW frequency as text");
// The two views, switched by the location hash.
function el(view) {
  const cls = new Set();
  return { hidden: false, dataset: { view }, classList: { toggle: (c, on) => (on ? cls.add(c) : cls.delete(c)), has: c => cls.has(c) } };
}
const views = { radio: el(), settings: el(), links: [el("radio"), el("settings")] };
eq(ctx.showView("#settings", views), "settings", "#settings shows Settings");
eq(views.radio.hidden && !views.settings.hidden, true, "only the Settings view shown");
eq(views.links[1].classList.has("on") && !views.links[0].classList.has("on"), true, "the Settings link marked");
for (const h of ["", "#", "#radio", "#other"]) {
  eq(ctx.showView(h, views), "radio", `hash '${h}' shows Radio`);
  eq(!views.radio.hidden && views.settings.hidden, true, `only the Radio view shown for '${h}'`);
}
eq(views.links[0].classList.has("on") && !views.links[1].classList.has("on"), true, "the Radio link marked");

// The header: the radio's UTC in the browser's zone (Oslo, UTC+1 in November) and UTC, or `time not set`.
const utc = Date.UTC(2026, 10, 10, 13, 5, 42);
eq(ctx.clockHtml(utc, t), "14:05<small>13:05 UTC</small>", "header with utc_ms");
eq(ctx.clockHtml(null, t), '<span class="nosig">time not set</span>', "header without utc_ms");

// The Time card: UTC, the browser's own clock with its zone, the last sync with its server.
const time = { utc_ms: utc, last_sync_ms: utc - 12 * 60000 - 30000, sync: true, server: "pool.ntp.org" };
eq(ctx.timeHtml(time, utc, utc + 2000, "Europe/Oslo", t),
  '<dl><dt>UTC</dt><dd><span class="mono">13:05:42</span></dd>' +
  '<dt>This browser</dt><dd><span class="mono">14:05:44</span> (Europe/Oslo)</dd>' +
  "<dt>Last sync</dt><dd>12 min ago from pool.ntp.org</dd></dl>", "Time card, synced 12 min ago");
eq(ctx.timeHtml({ ...time, last_sync_ms: utc - 3 * 3600000 - 59 * 60000 }, utc, utc, "Europe/Oslo", t)
  .includes("<dd>3 h ago from pool.ntp.org</dd>"), true, "age in whole hours from an hour");
eq(ctx.timeHtml({ ...time, utc_ms: null, last_sync_ms: null }, null, utc, "Europe/Oslo", t),
  '<dl><dt>UTC</dt><dd>time not set</dd><dt>This browser</dt><dd><span class="mono">14:05:42</span> (Europe/Oslo)</dd>' +
  "<dt>Last sync</dt><dd>never</dd></dl>", "Time card, clock not set");

// The switch and the server form post /api/time.
const posts = [];
const form = { sync: { checked: true }, server: { value: " ntp.example.net " }, save: {}, def: {} };
ctx.bindTime(form, (url, body) => posts.push([url, JSON.stringify(body)]));
form.sync.checked = false;
form.sync.onchange();
form.save.onclick();
form.server.onkeydown({ key: "Enter" });
form.server.onkeydown({ key: "a" });
form.def.onclick();
eq(JSON.stringify(posts), JSON.stringify([
  ["/api/time", '{"sync":false}'],
  ["/api/time", '{"server":"ntp.example.net"}'],
  ["/api/time", '{"server":"ntp.example.net"}'],
  ["/api/time", '{"server":""}'],
]), "switch, Save, Enter and Default post /api/time");
// ---- the frequency guide ----
for (const [bits, text] of [[0x1F, "Mo-Fr"], [0x0A, "Tu,Th"], [0x7D, "We-Mo"], [0x60, "Sa,Su"], [0x41, "Mo,Su"],
  [0x61, "Sa-Mo"], [0x55, "Mo,We,Fr,Su"], [0, ""]]) eq(ctx.daysText(bits), text, `days 0x${bits.toString(16)}`);
// The Radio view's table from the state's `guide`, as GET /api/state sends it; text set with textContent only.
const entries = [
  { name: "Channel 292", start: 0, end: 1440, days: 127, irregular: false, language: "D,E", target: "Eu", on_air: true },
  { name: "<b>China</b> & Co", start: 1020, end: 1080, days: 31, irregular: false, language: "R", target: "CAs", on_air: false },
  { name: "SM Radio Dessau", start: 420, end: 840, days: 0, irregular: true, language: "D", target: "Eu", on_air: false },
];
eq(JSON.stringify(ctx.guideRows(entries, t)), JSON.stringify([
  { on: true, cells: ["0000-2400", "daily", "Channel 292", "D,E", "Eu"] },
  { on: false, cells: ["1700-1800", "Mo-Fr", "<b>China</b> & Co", "R", "CAs"] },
  { on: false, cells: ["0700-1400", "irr", "SM Radio Dessau", "D", "Eu"] },
]), "guide rows: time, days, name, language, target");
eq(ctx.guideRows(null, t).length, 0, "no rows in FM");
// A DOM node as far as the table needs: setting textContent replaces the children, as in a browser.
function node(tag) {
  let text = "";
  const n = { tag, children: [], className: "", appendChild(c) { this.children.push(c); } };
  Object.defineProperty(n, "textContent", { get: () => text, set(v) { text = v; n.children = []; } });
  Object.defineProperty(n, "innerHTML", { set() { throw new Error("innerHTML used for guide text"); } });
  return n;
}
const doc = { createElement: node };
const tbody = node("tbody");
tbody.children.push("a stale row");
ctx.renderGuide(tbody, entries, t, doc);
eq(tbody.children.length, 3, "three rows");
eq(tbody.children[0].className + "|" + tbody.children[1].className, "on|", "the on-air row marked");
eq(tbody.children[1].children.map(c => c.textContent).join("|"), "1700-1800|Mo-Fr|<b>China</b> & Co|R|CAs",
  "cells as text, unescaped and not parsed");
eq(ctx.guideInfoText({ season: "B26", entries: 8780, length: 137325, sha256: "ab", source_modified: "2026-10-31T12:00:00Z" }, t),
  "EiBi B26 · 2026-10-31 · 8780 entries", "Settings > Guide line");
eq(ctx.guideInfoText({ entries: 0 }, t), "No guide", "Settings > Guide without a table");

// SHA-256 against Node's own, on the converter's table.
const crypto = require("crypto");
const tableBytes = new Uint8Array(fs.readFileSync(path.join(root, "radio/build/host/guide/b26.bin")));
const tableSha = crypto.createHash("sha256").update(tableBytes).digest("hex");
eq(ctx.sha256(tableBytes), tableSha, "SHA-256 of the B26 table");
eq(ctx.sha256(new Uint8Array(0)), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
for (const n of [55, 56, 63, 64, 65]) {
  const b = new Uint8Array(n).fill(97);
  eq(ctx.sha256(b), crypto.createHash("sha256").update(b).digest("hex"), `SHA-256 of ${n} bytes`);
}

// The event stream: a dropped connection the EventSource retries itself shows offline and opens nothing new; one it
// closed for good (an error answer, such as 503 with every stream taken) is opened again 5 s later.
{
  const sources = [], timers = [], states = [];
  let offline = 0;
  class FakeEventSource {
    constructor(url) { this.url = url; this.readyState = FakeEventSource.CONNECTING; this.on = {}; sources.push(this); }
    addEventListener(type, fn) { this.on[type] = fn; }
  }
  Object.assign(FakeEventSource, { CONNECTING: 0, OPEN: 1, CLOSED: 2 });
  ctx.EventSource = FakeEventSource;
  ctx.setTimeout = (fn, ms) => timers.push([fn, ms]);
  ctx.openEvents(s => states.push(s.hz), () => offline++);
  eq(sources.length + "|" + sources[0].url, "1|/api/events", "one stream opened");
  sources[0].on.state({ data: '{"hz":99500000}' });
  eq(states.join(), "99500000", "a state event rendered");
  sources[0].readyState = FakeEventSource.CONNECTING;
  sources[0].onerror();
  eq(offline + "|" + timers.length + "|" + sources.length, "1|0|1", "a dropped connection: offline, the EventSource retries");
  sources[0].readyState = FakeEventSource.CLOSED;
  sources[0].onerror();
  eq(offline + "|" + timers.length + "|" + timers[0][1], "2|1|5000", "closed for good: offline, a new one in 5 s");
  eq(sources.length, 1, "nothing opened before the 5 s");
  timers.shift()[0]();
  eq(sources.length + "|" + sources[1].url, "2|/api/events", "a new stream after 5 s");
  sources[1].on.state({ data: '{"hz":6070000}' });
  eq(states.join(), "99500000,6070000", "the new stream's state rendered");
  sources[1].readyState = FakeEventSource.CLOSED;
  sources[1].onerror();
  timers.shift()[0]();
  eq(sources.length, 3, "and again after the next refusal");
}

// The update flow against a fake data repository and radio.
const manifest = { format: 1, season: "B26", entries: 8780, length: tableBytes.length, sha256: tableSha,
  source_modified: "2026-10-31T12:00:00Z", csv: "eibi/sked-b26.csv" };
function repo({ m = manifest, bytes = tableBytes, offline = false, stream = true } = {}) {
  return async url => {
    if (offline) throw new TypeError("Failed to fetch");
    if (url === ctx.GUIDE_BASE + "manifest.json") return { ok: true, json: async () => m };
    if (url === ctx.GUIDE_BASE + "stations.bin") {
      const chunks = [];
      for (let i = 0; i < bytes.length; i += 40000) chunks.push(bytes.slice(i, i + 40000));
      return { ok: true, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length),
        body: stream ? { getReader: () => ({ read: async () => chunks.length ? { done: false, value: chunks.shift() } : { done: true } }) } : null };
    }
    return { ok: false, status: 404 };
  };
}
async function flow(radio, opts, postOk = true) {
  const log = [], posts = [];
  const io = { fetch: repo(opts), status: s => log.push(s),
    post: async (bytes, sha, progress) => { posts.push([bytes.length, sha]); progress(bytes.length); return postOk ? { ok: true, text: "" } : { ok: false, text: "CRC does not match" }; } };
  const result = await ctx.updateGuide(radio, io, t);
  return { result, log, posts };
}
(async () => {
  let r = await flow({ entries: 8780, sha256: tableSha });
  eq(r.result + "|" + r.log.join("|") + "|" + r.posts.length, "uptodate|Up to date|0", "up to date: nothing fetched or posted");
  r = await flow({ entries: 0 });
  eq(r.result, "updated", "an update");
  eq(JSON.stringify(r.posts), JSON.stringify([[tableBytes.length, tableSha]]), "the table posted with its SHA-256");
  eq(r.log.some(s => s.startsWith("Downloading ")), true, "download progress shown");
  eq(r.log[r.log.length - 1], "Guide updated", "result shown");
  r = await flow({ entries: 0 }, { stream: false });
  eq(r.result, "updated", "an update without a body stream");
  const damaged = tableBytes.slice();
  damaged[100] ^= 1;
  r = await flow({ entries: 0 }, { bytes: damaged });
  eq(r.result + "|" + r.posts.length + "|" + r.log[r.log.length - 1], "mismatch|0|The download does not match its manifest.",
    "a hash mismatch: nothing posted");
  r = await flow({ entries: 0 }, { bytes: tableBytes.slice(0, 1000) });
  eq(r.result + "|" + r.posts.length, "mismatch|0", "a length mismatch: nothing posted");
  r = await flow({ entries: 0 }, { offline: true });
  eq(r.result + "|" + r.log.join("|"), "offline|No internet connection. Use From file.", "no internet: points to From file");
  r = await flow({ entries: 0 }, { m: { ...manifest, format: 2 } });
  eq(r.result + "|" + r.posts.length, "failed|0", "a manifest of another format");
  r = await flow({ entries: 0 }, {}, false);
  eq(r.result + "|" + r.log[r.log.length - 1], "failed|Failed: CRC does not match", "the radio refuses the table");
  // From file: the bytes posted without a SHA-256.
  const log = [], posts = [];
  const io = { status: s => log.push(s), post: async (b, sha) => { posts.push([b.length, sha]); return { ok: true, text: "" }; } };
  eq(await ctx.sendGuide(tableBytes, null, io, t), "updated", "From file");
  eq(JSON.stringify(posts), JSON.stringify([[tableBytes.length, null]]), "From file posts the file as it is");
  console.log(`page: ${checks} checks, 0 failures`);
})().catch(e => { console.error(e); process.exit(1); });
