// The radio web page's functions under Node, from the page's <script id="lib">, with the English strings from the
// prototype's table: the RDS rendering on a state as GET /api/state sends it (radio/test/rds/state-99.5.json, the
// fields decoded from the 99.5 MHz bench capture), the Radio and Settings views by the location hash, the header
// clock, the Time card and its controls posting /api/time.
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
vm.runInContext(lib[1] + "\n;Object.assign(globalThis, { rdsHtml, showView, clockHtml, timeHtml, ageText, bindTime });", ctx);
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
console.log(`page: ${checks} checks, 0 failures`);
