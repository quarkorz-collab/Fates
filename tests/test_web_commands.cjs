"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { spawnSync } = require("node:child_process");

const source = fs.readFileSync(path.join(__dirname, "../frontend/static/app.js"), "utf8");
function declaration(name) {
  const start = source.indexOf(`function ${name}(`);
  assert.ok(start >= 0, `Missing function ${name}`);
  const end = source.indexOf("\nfunction ", start + 1);
  assert.ok(end > start);
  return source.slice(start, end);
}
const context = vm.createContext({});
for (const name of ["quotePowerShell", "quotePosix", "formatCommand", "tokenizePosix",
  "tokenizePowerShell", "tokenizeCommand", "isExecutableToken"]) {
  vm.runInContext(declaration(name), context);
}
const tokens = (command, shell) => Array.from(context.tokenizeCommand(command, shell));
const args = ["777777", "--digits=", "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,cos,tan",
  "--error-range", "(0,inf)", "--args-file", "a folder/it's π.args", "", "@args.txt",
  "$HOME", "$(do-not-run)", "`no-execution`", "a\\b", "#comment", "a;b", "a\nb", 'a"b'];

for (const shell of ["posix", "powershell"]) {
  const command = context.formatCommand(args, shell);
  const parsed = tokens(command, shell);
  assert.equal(parsed.shift(), shell === "posix" ? "./fates" : ".\\fates.exe");
  assert.deepEqual(parsed, args, `${shell} round trip`);
}

assert.deepEqual(tokens("./fates 777777 \\\n --digits='' \\\r\n --ops '+,-,*,/' # comment\n --json", "posix"),
  ["./fates", "777777", "--digits=", "--ops", "+,-,*,/", "--json"]);
assert.deepEqual(tokens('./fates --args-file "a\\q\\$b\\\"c\\\\d"', "posix"),
  ["./fates", "--args-file", 'a\\q$b"c\\d']);
assert.deepEqual(tokens("./fates --args-file a\\ b.args --digits ''", "posix"),
  ["./fates", "--args-file", "a b.args", "--digits", ""]);
assert.deepEqual(tokens(".\\fates.exe 777777 `\r\n --digits='' --constant 'a''b=1'", "powershell"),
  [".\\fates.exe", "777777", "--digits=", "--constant", "a'b=1"]);
for (const command of ["./fates 'unclosed", './fates "unclosed', "./fates \\",
  "./fates 1; echo nope", "./fates 1 | cat", "./fates 1 > out", "./fates $TARGET", './fates "$(cmd)"']) {
  assert.throws(() => tokens(command, "posix"), undefined, command);
}
for (const executable of ["./fates", "/tmp/a b/fates", ".\\fates.exe", "C:\\a b\\fates.exe", "pries"]) {
  assert.equal(context.isExecutableToken(executable), true);
}

if (process.platform !== "win32") {
  // Independent oracle: the real shell must recover exactly the same arguments.
  const nodeScript = "process.stdout.write(JSON.stringify(process.argv.slice(1)))";
  const command = [process.execPath, "-e", nodeScript, "--", ...args].map(context.quotePosix).join(" ");
  const result = spawnSync("/bin/sh", ["-c", command], { encoding: "utf8", timeout: 10000 });
  assert.equal(result.status, 0, result.stderr || result.error?.message);
  assert.deepEqual(JSON.parse(result.stdout), args);
}

console.log("Web command quoting/import: POSIX and PowerShell passed.");
