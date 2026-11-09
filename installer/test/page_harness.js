// Runs the page's <script id="lib"> under Node for test_page.py.
//
//   node page_harness.js PAGE REQUEST_JSON
//
// REQUEST_JSON: {"sha256": [hex, ...], "split": [path, ...], "csi": [[frame, ...], ...]}.
// Prints {"sha256": [digest, ...], "split": [[{target, length, erase, sha256}, ...], ...],
// "csi": [text, ...]}: each csi case is console frames fed through stripCsi()
// with the unfinished rest carried over, as the page does.
"use strict";
const fs = require("fs");
const vm = require("vm");

const [pagePath, requestPath] = process.argv.slice(2);
const page = fs.readFileSync(pagePath, "utf8");
const lib = page.match(/<script id="lib">([\s\S]*?)<\/script>/);
if (!lib) throw new Error("no <script id=\"lib\"> in the page");
const ctx = vm.createContext({});
vm.runInContext(lib[1] + "\n;globalThis.sha256 = sha256; globalThis.splitRegions = splitRegions; globalThis.stripCsi = stripCsi;", ctx);

// The page's other scripts must at least compile.
for (const m of page.matchAll(/<script>([\s\S]*?)<\/script>/g)) new vm.Script(m[1]);

const req = JSON.parse(fs.readFileSync(requestPath, "utf8"));
const out = {
  sha256: req.sha256.map(h => ctx.sha256(new Uint8Array(Buffer.from(h, "hex")))),
  split: req.split.map(p => ctx.splitRegions(0, new Uint8Array(fs.readFileSync(p)))
    .map(r => ({target: r.target, length: r.length, erase: r.erase, sha256: r.sha256}))),
  csi: req.csi.map(frames => {
    let text = "", rest = "";
    for (const f of frames) {
      const [t, r] = ctx.stripCsi(rest + f);
      text += t;
      rest = r;
    }
    return text;
  }),
};
process.stdout.write(JSON.stringify(out));
