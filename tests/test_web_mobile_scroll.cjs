"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "../frontend/static/app.js"), "utf8");
const bindings = source.match(/^const mobileViewButtons = .*;\nconst mobileScrollPositions = .*;$/m);
assert.ok(bindings, "Missing mobile view bindings");
const viewStart = source.indexOf("function setMobileView(");
const viewEnd = source.indexOf("\nlet toastTimer", viewStart);
assert.ok(viewStart >= 0 && viewEnd > viewStart, "Missing setMobileView implementation");
const listenersStart = source.indexOf("mobileViewButtons.forEach((button) => {");
const listenersEnd = source.indexOf("\n});", listenersStart);
assert.ok(listenersStart >= 0 && listenersEnd > listenersStart, "Missing mobile view handlers");

function target(view, parent = null) {
  const listeners = [];
  return {
    parent, dataset: { mobileView: view }, listeners, attributes: {},
    classList: { toggle() {} },
    addEventListener(type, listener) {
      assert.equal(type, "click");
      listeners.push(listener);
    },
    setAttribute(name, value) { this.attributes[name] = value; },
    click() {
      for (let current = this; current; current = current.parent) {
        for (const listener of current.listeners) listener();
      }
    },
  };
}

const workspace = target("config");
const nav = target(null);
nav.offsetTop = 496;
const header = { offsetHeight: 74 };
const config = target("config", nav);
const results = target("results", nav);
const checkbox = target(null, workspace);
const outputTab = target(null, workspace);
const scrollCalls = [];
const window = {
  scrollY: 980,
  matchMedia: () => ({ matches: true }),
  scrollTo(x, y) {
    assert.equal(x, 0);
    scrollCalls.push(y);
    this.scrollY = y;
  },
};
const document = {
  querySelectorAll(selector) {
    if (selector === ".mobile-view-switch button[data-mobile-view]") return [config, results];
    if (selector === "[data-mobile-view]") return [config, results, workspace];
    throw new Error(`Unexpected selector: ${selector}`);
  },
  querySelector(selector) {
    if (selector === ".workspace") return workspace;
    if (selector === ".mobile-view-switch") return nav;
    if (selector === ".app-header") return header;
    throw new Error(`Unexpected selector: ${selector}`);
  },
};

const context = vm.createContext({ document, window });
vm.runInContext([
  bindings[0],
  source.slice(viewStart, viewEnd),
  source.slice(listenersStart, listenersEnd + 4),
].join("\n"), context);

checkbox.click();
outputTab.click();
assert.equal(window.scrollY, 980, "clicks inside the workspace must not scroll the page");
assert.equal(scrollCalls.length, 0);
assert.equal(workspace.listeners.length, 0, "workspace must not receive a view switch handler");

results.click();
assert.equal(window.scrollY, header.offsetHeight, "first visit should start at the new view, below the header");
assert.equal(workspace.dataset.mobileView, "results");
assert.equal(results.attributes["aria-pressed"], "true");

window.scrollY = 250;
outputTab.click();
assert.equal(window.scrollY, 250, "switching output tabs must preserve the scroll position");
config.click();
assert.equal(window.scrollY, 980, "returning to config must restore its scroll position");
results.click();
assert.equal(window.scrollY, 250, "returning to results must restore its scroll position");
results.click();
assert.equal(window.scrollY, 250, "reselecting the current view must not scroll");

console.log("Mobile WebUI: workspace clicks preserve scroll; main views restore their own positions.");
