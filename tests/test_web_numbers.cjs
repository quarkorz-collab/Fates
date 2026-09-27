"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "../frontend/static/app.js"), "utf8");
function declaration(name) {
  const start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0, `Missing function ${name}`);
  const end = source.indexOf("\nfunction ", start + 1);
  assert.ok(end > start);
  return source.slice(start, end);
}

function element(fragment = false) {
  return {
    fragment,
    children: [],
    append(...nodes) { this.children.push(...nodes); },
    replaceChildren(...nodes) {
      this.children = nodes.flatMap((node) => node.fragment ? node.children : [node]);
    },
  };
}

const context = vm.createContext({
  document: { createElement: () => element(), createDocumentFragment: () => element(true) },
  createCell: (textContent) => ({ textContent }),
  createExpressionCell: (textContent) => ({ textContent }),
});
vm.runInContext(`${declaration("formatNumber")}\n${declaration("renderSearchRows")}`, context);

const root = 777777.0000000017;
assert.equal(context.formatNumber(root, true), root.toString());
assert.equal(context.formatNumber(root), "777777"); // Other number columns retain compact formatting.
assert.equal(context.formatNumber(null, true), "—");
for (const value of [root, -root, 77776.9999999558, 1e-12, 1e20]) {
  assert.equal(Number(context.formatNumber(value, true)), value);
}

for (const mode of ["equations", "constants"]) {
  const head = element();
  const body = element();
  const table = { querySelector: (selector) => selector === "thead" ? head : body };
  context.renderSearchRows(table, {
    search_mode: mode,
    results: [{ [mode === "equations" ? "equation" : "expression"]: "x = pi", cost: 3,
      estimated_root: root, value: root, signed_error: root - 777777,
      absolute_error: root - 777777, residual_at_target: 1e-9, relative_error: 1e-15 }],
  });
  assert.equal(body.children.length, 1);
  const cells = body.children[0].children;
  assert.equal(cells[3].textContent, mode === "equations" ? root.toString() : "777777");
  assert.notEqual(cells[5].textContent, "0");
}

console.log("Web numeric formatting and result/live-table shared renderer passed.");
