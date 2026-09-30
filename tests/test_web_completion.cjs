"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../frontend/static/app.js"), "utf8");
const html = fs.readFileSync(path.join(__dirname, "../frontend/static/index.html"), "utf8");
const fields = new Map();
for (const id of ["target", "utilityAction", "includeDefaults", "argsFile", "equations", "searchMode",
  "inverseDepth", "deepRounds", "noBidirectional", "maxCost", "sideCost", "noStop",
  "customConstants", "symbolCounts", "constantCounts", "symbolOrder"]) {
  fields.set(id, { id, value: "", defaultValue: "", checked: false, type: "text", dataset: {} });
}
for (const [id, option, value] of [["completionMode", "--completion-mode", "auto"],
  ["completionBudget", "--completion-budget", "0"]]) {
  assert.match(html, new RegExp(`id="${id}"[^>]*data-option="${option}"[^>]*data-default="${value}"`));
  fields.set(id, { id, value, defaultValue: value, checked: false, type: "text", dataset: { option, default: value } });
}
const form = {
  querySelectorAll: selector => [...fields.values()].filter(field => selector !== "[data-option]" || field.dataset.option),
  checkValidity: () => true,
  reset() { for (const field of fields.values()) { field.value = field.defaultValue; field.checked = false; } },
};
const context = vm.createContext({ element: id => fields.get(id), form, advancedPolicies: {},
  geneticFieldIds: [], geneticIntent: () => false, advancedIntent: () => false,
  commandPreview: { value: "" }, validationMessage: {}, runButton: {}, runtime: { running: false },
  syncSymbolPickers() {}, clearPresetSelection() {}, showToast() {}, document: { querySelectorAll: () => [] } });
vm.runInContext("let commandShell = 'posix'; let commandDirty = false;", context);
for (const name of ["lines", "quotePowerShell", "quotePosix", "formatCommand", "tokenizePosix", "tokenizePowerShell",
  "tokenizeCommand", "isExecutableToken", "takeOptionValue", "isRangeEndpoint", "buildArguments",
  "validateConfiguration", "importCommand", "refreshCommand"]) {
  const start = source.indexOf(`function ${name}(`);
  const end = source.indexOf("\nfunction ", start + 1);
  assert.ok(start >= 0 && end > start, name);
  vm.runInContext(source.slice(start, end), context);
}
fields.get("target").value = "0.731";
assert.deepEqual(Array.from(context.buildArguments()), ["0.731"]);
for (const shell of ["posix", "powershell"]) {
  vm.runInContext(`commandShell = '${shell}'`, context);
  for (const mode of ["off", "auto", "full"]) {
    fields.get("target").value = "0.731";
    fields.get("completionMode").value = mode;
    fields.get("completionBudget").value = mode === "auto" ? "12345" : "0";
    context.refreshCommand();
    assert.equal(context.runButton.disabled, false);
    const saved = Array.from(context.buildArguments());
    context.importCommand();
    assert.deepEqual(Array.from(context.buildArguments()), saved);
    assert.equal(fields.get("completionMode").value, mode);
  }
}
fields.get("completionMode").value = "full";
fields.get("completionBudget").value = "1";
assert.equal(context.validateConfiguration([]).valid, false);
fields.get("completionMode").value = "auto";
assert.equal(context.validateConfiguration([]).valid, true);
console.log("Web completion policy: defaults, validation and POSIX/PowerShell import round-trips passed.");
