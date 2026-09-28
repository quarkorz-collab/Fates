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
  assert.ok(end > start, `Missing end of ${name}`);
  return source.slice(start, end);
}
function control(id, value = "", option = undefined) {
  return { id, value, defaultValue: value, checked: false, type: "text", tagName: "TEXTAREA",
    dataset: option ? { option, default: "123456789", emptyEquals: "true" } : {} };
}
const fields = new Map([
  ["target", control("target")],
  ["digits", control("digits", "123456789", "--digits")],
  ["customConstants", control("customConstants")],
  ["symbolCounts", control("symbolCounts")],
  ["constantCounts", control("constantCounts")],
  ["symbolOrder", control("symbolOrder")],
  ["utilityAction", control("utilityAction")],
  ["includeDefaults", control("includeDefaults")],
]);
const allControls = [...fields.values()];
const form = {
  querySelectorAll(selector) {
    if (selector === "[data-option]") return allControls.filter((field) => field.dataset.option);
    if (selector === "input, select, textarea") return allControls;
    return [];
  },
  reset() {
    for (const field of allControls) {
      field.value = field.defaultValue;
      field.checked = false;
    }
  },
};
const commandPreview = { value: "" };
const validationMessage = { textContent: "", className: "" };
const runButton = { disabled: false };
const context = vm.createContext({
  element: (id) => fields.get(id), form, commandPreview, validationMessage, runButton,
  runtime: { running: false }, advancedPolicies: {},
  geneticFieldIds: [], advancedIntent: () => false, geneticIntent: () => false,
  validateConfiguration: () => ({ valid: true, message: "ready" }),
  syncSymbolPickers() {}, clearPresetSelection() {}, showToast() {},
  document: { querySelectorAll: () => [] },
});
vm.runInContext("let commandShell = 'posix'; let commandDirty = false;", context);
for (const name of ["lines", "quotePowerShell", "quotePosix", "formatCommand", "tokenizePosix",
  "tokenizePowerShell", "tokenizeCommand", "isExecutableToken", "takeOptionValue",
  "isRangeEndpoint", "buildArguments", "importCommand", "refreshCommand"]) {
  vm.runInContext(declaration(name), context);
}

fields.get("target").value = "5.859874482048838";
fields.get("digits").value = "";
fields.get("symbolCounts").value = "pi=1";
fields.get("constantCounts").value = "pi,e=2:4\ne,phi=0:1\n";
for (const shell of ["posix", "powershell"]) {
  vm.runInContext(`commandShell = '${shell}'`, context);
  context.refreshCommand();
  const command = commandPreview.value;
  assert.match(command, /--constant-count/);
  assert.match(command, /pi,e=2:4/);
  assert.match(command, /e,phi=0:1/);
  // Use the real import path so both repeated and inline options round-trip.
  fields.get("constantCounts").value = "";
  commandPreview.value = command.replace(/--constant-count\s+'?e,phi=0:1'?/, "--constant-count=e,phi=0:1");
  context.importCommand();
  assert.equal(fields.get("constantCounts").value, "pi,e=2:4\ne,phi=0:1");
  assert.equal(fields.get("symbolCounts").value, "pi=1");
  assert.equal(fields.get("digits").value, "");
  assert.deepEqual(Array.from(context.buildArguments()).filter((arg) => arg === "--constant-count"),
    ["--constant-count", "--constant-count"]);
  assert.equal(runButton.disabled, false);
  const before = fields.get("constantCounts").value;
  commandPreview.value = `${shell === "posix" ? "./fates" : ".\\fates.exe"} 1 --constant-count pi,e=2 --unexpected 1`;
  context.importCommand();
  assert.equal(fields.get("constantCounts").value, before, "failed import must restore group field");
  assert.equal(runButton.disabled, true);
}

console.log("Web grouped constant counts: form/command import round-trip passed on POSIX and PowerShell.");
