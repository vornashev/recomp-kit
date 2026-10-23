// node --test web/player/tests/*.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createContext, runInContext } from "node:vm";

const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
const script = html.split("<script>")[1].split("</script>")[0];
// Exercise the shipped page's handlers without downloading or starting a game.
const handlers = script.slice(0, script.lastIndexOf("\nstart().catch("));

class Element {
  hidden = true;
  children = [];
  attributes = new Map();
  listeners = new Map();
  constructor(tag) { this.tagName = tag; }
  set textContent(text) { this.text = text; this.children = []; }
  get textContent() { return this.text || ""; }
  setAttribute(name, value) { this.attributes.set(name, value); }
  getAttribute(name) { return this.attributes.get(name); }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; this.text = ""; }
  hasChildNodes() { return this.children.length > 0 || !!this.text; }
  addEventListener(name, handler) { this.listeners.set(name, handler); }
  click() { return this.listeners.get("click")?.(); }
  focus() {}
}

function page({ external = true, savedHidden = "0" } = {}) {
  const elements = Object.fromEntries(["canvas", "status", "perf", "saves-button", "saves"]
    .map(id => [id, new Element(id)]));
  const stored = new Map([["recomp.perf.hidden", savedHidden]]);
  const intervals = [];
  const storage = { persist: async () => true, persisted: async () => true,
    getDirectory: async () => { throw new Error("No saves yet"); } };
  const context = createContext({
    document: { getElementById: id => elements[id], createElement: tag => new Element(tag) },
    navigator: { language: "en", storage },
    performance: { now: () => 0 }, console,
    localStorage: { getItem: key => stored.get(key), setItem: (key, value) => stored.set(key, value) },
    setInterval: fn => intervals.push(fn),
    recompUI: external ? { onPacing() {} } : {},
  });
  context.window = context;
  runInContext(handlers, context);
  let mode = 0, hidden = null;
  const module = {
    _recomp_web_overlay_mode: () => mode,
    _recomp_web_overlay_hidden: value => { hidden = value; },
    _recomp_web_pacing() {},
  };
  context.setupPerfButton(module);
  context.setupSaves("test", { savesDir: "saves", saveExt: ".sav" });
  return { elements, storage, player: context.recompPlayer, stored, intervals,
    setMode(value) { mode = value; }, nativeHidden: () => hidden };
}

test("themed FPS remains available with native mode off and remembers visibility", () => {
  const p = page({ savedHidden: "1" });
  assert.equal(p.elements.perf.hidden, false);
  assert.equal(p.player.overlayHidden, true);
  p.elements.perf.click();
  assert.equal(p.player.overlayHidden, false);
  assert.equal(p.nativeHidden(), 0);
  assert.equal(p.elements.perf.getAttribute("aria-expanded"), "true");
  assert.equal(p.stored.get("recomp.perf.hidden"), "0");
});

test("FPS, saves and theme panels are mutually exclusive", async () => {
  const p = page();
  let chest = false;
  p.player.registerPanel("chest", open => { chest = open; });
  assert.equal(p.player.overlayHidden, false);
  await p.elements["saves-button"].click();
  assert.equal(p.elements.saves.hidden, false);
  assert.equal(p.player.overlayHidden, true);
  assert.equal(p.elements.perf.getAttribute("aria-pressed"), "false");
  p.player.togglePanel("chest");
  assert.equal(chest, true);
  assert.equal(p.elements.saves.hidden, true);
  assert.equal(p.elements["saves-button"].getAttribute("aria-expanded"), "false");
  p.elements.perf.click();
  assert.equal(chest, false);
  assert.equal(p.player.overlayHidden, false);
  p.elements.perf.click();
  assert.equal(p.player.overlayHidden, true);
});

test("a pending save read cannot reopen its panel after a second click or a tab switch", async () => {
  const p = page();
  let finish;
  p.storage.persist = () => new Promise(resolve => { finish = resolve; });
  const reading = p.elements["saves-button"].click();
  assert.equal(p.elements.saves.hidden, false);
  await p.elements["saves-button"].click();
  assert.equal(p.elements.saves.hidden, true);
  p.elements.perf.click();
  finish(true);
  await reading;
  assert.equal(p.elements.saves.hidden, true);
  assert.equal(p.player.overlayHidden, false);
  assert.equal(p.elements.saves.children.filter(e => e.tagName === "h2").length, 1);
});

test("native-only FPS still follows the engine setting", () => {
  const p = page({ external: false });
  assert.equal(p.elements.perf.hidden, true);
  p.setMode(1);
  p.intervals[0]();
  assert.equal(p.elements.perf.hidden, false);
  p.elements.perf.click();
  assert.equal(p.nativeHidden(), 0);
  p.setMode(0);
  p.intervals[0]();
  assert.equal(p.elements.perf.hidden, true);
  assert.equal(p.nativeHidden(), 1);
});
