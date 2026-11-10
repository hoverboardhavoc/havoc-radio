// The radio web page's RDS rendering under Node: rdsHtml() from the page's <script id="lib"> on a state as
// GET /api/state sends it (radio/test/rds/state-99.5.json, the fields decoded from the 99.5 MHz bench capture),
// with the English strings from the prototype's table.
//
//   node test_page.js ROOT
"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const root = process.argv[2];
const page = fs.readFileSync(path.join(root, "radio/components/web/page/index.html"), "utf8");
const lib = page.match(/<script id="lib">([\s\S]*?)<\/script>/);
assert(lib, 'no <script id="lib"> in the page');
const ctx = vm.createContext({});
vm.runInContext(lib[1] + "\n;globalThis.rdsHtml = rdsHtml;", ctx);
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
console.log(`page: ${checks} checks, 0 failures`);
