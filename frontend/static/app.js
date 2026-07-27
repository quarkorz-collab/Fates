"use strict";

const form = document.querySelector("#configForm");
const runButton = document.querySelector("#runButton");
const cancelButton = document.querySelector("#cancelButton");
const commandPreview = document.querySelector("#commandPreview");
const validationMessage = document.querySelector("#validationMessage");
const runStatus = document.querySelector("#runStatus");
const runMetrics = document.querySelector("#runMetrics");
const runDot = document.querySelector("#runDot");
const resultSummary = document.querySelector("#resultSummary");
const stdoutOutput = document.querySelector("#stdoutOutput");
const stderrOutput = document.querySelector("#stderrOutput");
const liveOutput = document.querySelector("#liveOutput");
const liveEmpty = document.querySelector("#liveEmpty");
const liveTitle = document.querySelector("#liveTitle");
const liveMeta = document.querySelector("#liveMeta");
const liveTableWrap = document.querySelector("#liveTableWrap");
const liveResultTable = document.querySelector("#liveResultTable");
const fallbackOutput = document.querySelector("#fallbackOutput");
const resultTableWrap = document.querySelector("#resultTableWrap");
const resultTable = document.querySelector("#resultTable");
const emptyState = document.querySelector("#emptyState");
const renderModeButtons = [...document.querySelectorAll("[data-render-mode]")];
const toast = document.querySelector("#toast");

const terminalStates = new Set(["completed", "failed", "cancelled"]);
const liveEventPrefix = "[fates-live] ";
const rawTailLimit = 240_000;
const geneticFieldIds = [
  "geneticPopulation",
  "geneticGenerations",
  "geneticSeed",
  "geneticElegance",
  "geneticRepair",
  "geneticRepairDepth",
  "geneticCrossover",
  "geneticNovelty",
  "geneticTournament",
];

let symbolCatalog = null;

const runtime = {
  jobId: null,
  stdoutOffset: 0,
  stderrOffset: 0,
  stdoutParts: [],
  stderrParts: [],
  stdoutLength: 0,
  stderrLength: 0,
  stdoutTail: "",
  stderrTail: "",
  stderrLineBuffer: "",
  liveData: null,
  finalData: null,
  renderMode: "latex",
  startedAt: null,
  pollTimer: null,
  rawRenderFrame: null,
  liveRenderFrame: null,
  cancelRequested: false,
  running: false,
};

try {
  const storedMode = localStorage.getItem("fates-render-mode");
  if (storedMode === "latex" || storedMode === "text") runtime.renderMode = storedMode;
} catch {
}

const presets = {
  quick: {
    maxCost: "10", beam: "3000", pairs: "4000000", valueBits: "42",
  },
  balanced: {
    maxCost: "14", beam: "10000", pairs: "20000000", valueBits: "48",
    valuePrune: "exact", explorePairs: "2", noStop: true,
  },
  deep: {
    maxCost: "16", beam: "20000", pairs: "50000000", valueBits: "48",
    paretoSlots: "2", inverseDepth: "2", inverseBeam: "96", inverseBudget: "3000000",
    deepRounds: "1", valuePrune: "exact", explorePairs: "4", noStop: true,
  },
  genetic: {
    maxCost: "16", beam: "10000", pairs: "20000000", valueBits: "48", valuePrune: "exact", explorePairs: "2",
    searchMode: "hybrid", geneticPopulation: "4096", geneticGenerations: "64",
    geneticElegance: "0.45", geneticRepair: "0.2", geneticRepairDepth: "3", noStop: true,
  },
  equation: {
    equations: true, equationNeighbors: "128", equationSearch: "wide", equationQuality: "strict",
    maxCost: "12", beam: "10000",
    pairs: "20000000", valueBits: "48", mode: "nearest",
  },
  "four-pi": {
    target: "2", digits: "", constants: "pi", ops: "+,-,*,/", maxCost: "7",
    valueBits: "52", mode: "nearest", symbolCounts: "pi=4",
  },
  "24-point": {
    target: "24", digits: "1234", maxLiteralLen: "1", constants: "none",
    ops: "+,-,*,/", maxCost: "7", valueBits: "52", mode: "nearest",
    symbolCounts: "1=1\n2=1\n3=1\n4=1",
  },
};

function element(id) {
  return document.getElementById(id);
}

function lines(value) {
  return value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
}

function fieldChanged(id) {
  const input = element(id);
  return input && input.value !== (input.dataset.default ?? "");
}

function geneticIntent() {
  const mode = element("searchMode").value;
  return element("genetic").checked
    || mode === "genetic"
    || mode === "hybrid"
    || geneticFieldIds.some(fieldChanged);
}

function buildArguments() {
  const utility = element("utilityAction").value;
  if (utility) {
    return [utility];
  }

  const args = [];
  const target = element("target").value.trim();
  if (target) {
    args.push(target);
  }
  const includeDefaults = element("includeDefaults").checked;
  const useGeneticFields = geneticIntent();

  for (const input of form.querySelectorAll("[data-option]")) {
    const option = input.dataset.option;
    if (!option) continue;
    if (geneticFieldIds.includes(input.id) && !useGeneticFields) continue;
    if (input.id === "liveJson" && !element("live").checked && !element("liveTop").value.trim()) continue;

    if (input.type === "checkbox") {
      if (input.checked) args.push(option);
      continue;
    }

    const value = input.value.trim();
    const defaultValue = input.dataset.default ?? "";
    if (!includeDefaults && value === defaultValue) continue;
    if (value === "") {
      if (input.dataset.emptyEquals === "true" && defaultValue !== "") {
        args.push(`${option}=`);
      }
      continue;
    }
    args.push(option, value);
  }

  for (const value of lines(element("customConstants").value)) {
    args.push("--constant", value);
  }
  for (const value of lines(element("symbolCounts").value)) {
    args.push("--symbol-count", value);
  }
  return args;
}

function validateConfiguration(args) {
  const utility = element("utilityAction").value;
  if (utility) return { valid: true, message: "控制动作无需 TARGET。" };

  const target = element("target").value.trim();
  const argsFile = element("argsFile").value.trim();
  if (!target && !argsFile) {
    return { valid: false, message: "请填写有限数值 TARGET，或选择一个自带 TARGET 的参数文件。" };
  }
  if (target && !Number.isFinite(Number(target))) {
    return { valid: false, message: "TARGET 必须是有限小数或科学计数法。" };
  }
  if (!form.checkValidity()) {
    return { valid: false, message: "有数值参数超出输入框标注的允许范围。" };
  }

  const equations = element("equations").checked;
  const genetic = geneticIntent();
  const portfolio = element("searchMode").value === "portfolio";
  const inverseDepth = Number(element("inverseDepth").value || 0);
  const deepRounds = Number(element("deepRounds").value || 0);
  const noBidirectional = element("noBidirectional").checked;
  const maxCost = Number(element("maxCost").value);
  const sideCost = Number(element("sideCost").value || 0);

  if (equations && genetic) {
    return { valid: false, message: "方程模式暂不能与遗传模式或 genetic 调参组合。" };
  }
  if (equations && portfolio) {
    return { valid: false, message: "portfolio 用于常量搜索；方程模式请选择 equation-search 的 wide 或 exhaustive。" };
  }
  if (equations && (inverseDepth > 0 || deepRounds > 0)) {
    return { valid: false, message: "方程模式不能启用 inverse-depth 或 deep-rounds。" };
  }
  if (noBidirectional && (inverseDepth > 0 || deepRounds > 0)) {
    return { valid: false, message: "完整逐层搜索不会进入 inverse/deep 终端阶段，请二选一。" };
  }
  if (noBidirectional && portfolio) {
    return { valid: false, message: "portfolio 包含 inverse/deep 路线，不能与 --no-bidirectional 同时使用。" };
  }
  if (sideCost > maxCost) {
    return { valid: false, message: "side-cost 不能大于 max-cost。" };
  }
  if (target && argsFile) {
    return { valid: true, message: "提示：若参数文件也包含 TARGET，会产生重复目标；仅含选项则没有问题。" };
  }
  if (genetic && !element("noStop").checked) {
    return { valid: true, message: "建议勾选 --no-stop；否则确定性阶段提前命中 epsilon 时会跳过遗传阶段。" };
  }
  return { valid: true, message: `${args.length} 个参数，配置可执行。` };
}

function quotePowerShell(value) {
  if (/^[A-Za-z0-9_.:\\/+==-]+$/.test(value) && !value.startsWith("@")) {
    return value;
  }
  return `'${value.replaceAll("'", "''")}'`;
}

function tokenizePowerShell(command) {
  const tokens = [];
  let current = "";
  let state = "bare";
  let started = false;

  const push = () => {
    if (!started) return;
    tokens.push(current);
    current = "";
    started = false;
  };

  for (let index = 0; index < command.length; index += 1) {
    const character = command[index];
    const next = command[index + 1];
    if (state === "single") {
      if (character === "'" && next === "'") {
        current += "'";
        index += 1;
      } else if (character === "'") {
        state = "bare";
      } else {
        current += character;
      }
      continue;
    }
    if (state === "double") {
      if (character === '"') {
        state = "bare";
      } else if (character === "`" && next !== undefined) {
        const escaped = { n: "\n", r: "\r", t: "\t" }[next] ?? next;
        current += escaped;
        index += 1;
      } else {
        current += character;
      }
      continue;
    }

    if (/\s/.test(character)) {
      push();
    } else if (character === "#" && !started) {
      while (index + 1 < command.length && command[index + 1] !== "\n") index += 1;
    } else if (character === "'") {
      state = "single";
      started = true;
    } else if (character === '"') {
      state = "double";
      started = true;
    } else if (character === "`" && next !== undefined) {
      started = true;
      if (next === "\r" && command[index + 2] === "\n") index += 2;
      else if (next === "\n") index += 1;
      else {
        current += next;
        index += 1;
      }
    } else {
      current += character;
      started = true;
    }
  }
  if (state !== "bare") throw new Error("命令中存在未闭合的引号。");
  push();
  return tokens;
}

function isExecutableToken(token) {
  const basename = token.replaceAll("/", "\\").split("\\").pop().toLowerCase();
  return ["fates", "fates.exe", "pries", "pries.exe"].includes(basename);
}

function takeOptionValue(tokens, index, inlineValue, option) {
  if (inlineValue !== null) return { value: inlineValue, nextIndex: index };
  if (index + 1 >= tokens.length || tokens[index + 1].startsWith("--")) {
    throw new Error(`${option} 缺少参数值。`);
  }
  return { value: tokens[index + 1], nextIndex: index + 1 };
}

function isRangeEndpoint(value) {
  return /^(?:[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?|[+-]?(?:inf|infinity))$/i.test(value);
}

function resetControl(control) {
  if (control.type === "checkbox" || control.type === "radio") {
    control.checked = control.defaultChecked;
  } else if (control.tagName === "SELECT") {
    const selected = [...control.options].findIndex((option) => option.defaultSelected);
    control.selectedIndex = selected >= 0 ? selected : 0;
  } else {
    control.value = control.defaultValue;
  }
}

function clearPresetSelection() {
  document.querySelectorAll("[data-preset]").forEach((button) => button.classList.remove("active"));
}

function csvEntries(inputId) {
  const raw = element(inputId).value.trim();
  if (!raw || raw === "none") return [];
  return raw.split(",").map((entry) => entry.trim()).filter(Boolean);
}

function entryName(entry) {
  return entry.replace(/:\d+$/, "");
}

function syncSymbolPickers() {
  const constants = new Set(csvEntries("constants").map(entryName));
  const operators = new Set(csvEntries("ops").map(entryName));
  document.querySelectorAll("[data-catalog-kind][data-symbol-name]").forEach((input) => {
    const active = input.dataset.catalogKind === "constant" ? constants : operators;
    input.checked = active.has(input.dataset.symbolName);
  });
}

function updateSymbolEntry(inputId, name, enabled) {
  let entries = csvEntries(inputId);
  const matches = (entry) => entryName(entry) === name;
  if (enabled) {
    if (!entries.some(matches)) entries.push(name);
  } else {
    entries = entries.filter((entry) => !matches(entry));
  }
  element(inputId).value = entries.length ? entries.join(",") : "none";
}

function symbolDisplayName(name, kind) {
  const labels = {
    pi: "π", tau: "τ", phi: "φ", sqrt2: "√2", ln2: "ln 2",
    catalan: "G", neg: "−x", inv: "1/x", sqrt: "√x", cbrt: "∛x",
    sqr: "x²", cube: "x³", gamma: kind === "constant" ? "γ" : "Γ(x)",
    fact: "x!", "*": "×", "/": "÷", "^": "xʸ",
  };
  return labels[name] || name;
}

function renderSymbolGroup(containerId, entries, kind) {
  const container = element(containerId);
  const fragment = document.createDocumentFragment();
  for (const item of entries) {
    const label = document.createElement("label");
    label.className = `symbol-choice${item.builtin === false ? " extension-symbol" : ""}`;
    const input = document.createElement("input");
    input.type = "checkbox";
    input.dataset.catalogKind = kind;
    input.dataset.symbolName = item.name;
    input.title = `${item.name} · 默认成本 ${item.default_cost}`;
    input.addEventListener("input", () => {
      updateSymbolEntry(kind === "constant" ? "constants" : "ops", item.name, input.checked);
      clearPresetSelection();
      refreshCommand();
    });
    const mark = document.createElement("span");
    mark.textContent = symbolDisplayName(item.name, kind);
    const meta = document.createElement("small");
    meta.textContent = `${item.name} · c${item.default_cost}${item.builtin === false ? " · 扩展" : ""}`;
    label.append(input, mark, meta);
    fragment.append(label);
  }
  container.replaceChildren(fragment);
}

function renderSymbolCatalog(catalog) {
  if (!catalog || !Array.isArray(catalog.constants) ||
      !Array.isArray(catalog.unary_operators) || !Array.isArray(catalog.binary_operators)) {
    throw new Error("服务器返回的符号目录不完整");
  }
  symbolCatalog = catalog;
  renderSymbolGroup("constantPicker", catalog.constants, "constant");
  renderSymbolGroup("binaryOperatorPicker", catalog.binary_operators, "operator");
  renderSymbolGroup("unaryOperatorPicker", catalog.unary_operators, "operator");
  syncSymbolPickers();
  const group = element("constantPicker").closest(".option-group");
  if (group) {
    const names = [
      ...catalog.constants, ...catalog.binary_operators, ...catalog.unary_operators,
    ].map((item) => item.name).join(" ");
    group.dataset.searchIndex = `${group.dataset.searchIndex || ""} ${names}`.toLowerCase();
  }
}

function importCommand() {
  const snapshot = [...form.querySelectorAll("input, select, textarea")].map((control) => ({
    control,
    value: control.value,
    checked: control.checked,
  }));
  try {
    const tokens = tokenizePowerShell(commandPreview.value.trim());
    if (!tokens.length) throw new Error("命令为空。");
    if (tokens[0] === "&") tokens.shift();
    if (tokens.length && isExecutableToken(tokens[0])) tokens.shift();
    if (!tokens.length) throw new Error("命令中没有 TARGET 或控制动作。");

    form.reset();
    element("target").value = "";
    element("customConstants").value = "";
    element("symbolCounts").value = "";
    element("symbolOrder").value = "";
    element("utilityAction").value = "";
    element("includeDefaults").checked = false;
    for (const input of form.querySelectorAll('[data-option][type="checkbox"]')) input.checked = false;

    const controls = new Map(
      [...form.querySelectorAll("[data-option]")].map((input) => [input.dataset.option, input]),
    );
    const constants = [];
    const counts = [];
    const utilities = new Map([
      ["--help", "--help"], ["-h", "--help"], ["--version", "--version"],
      ["--list-symbols", "--list-symbols"], ["--self-test", "--self-test"],
    ]);
    let targetSeen = false;

    for (let index = 0; index < tokens.length; index += 1) {
      const token = tokens[index];
      if (token.startsWith("@") && !token.startsWith("@@")) {
        element("argsFile").value = token.slice(1);
        continue;
      }
      if (!token.startsWith("-") || /^-(?:\d|\.\d)/.test(token)) {
        if (targetSeen) throw new Error(`发现多余的位置参数：${token}`);
        element("target").value = token;
        targetSeen = true;
        continue;
      }

      const equals = token.indexOf("=");
      const option = equals > 0 ? token.slice(0, equals) : token;
      const inlineValue = equals > 0 ? token.slice(equals + 1) : null;
      if (utilities.has(option)) {
        if (inlineValue !== null) throw new Error(`${option} 不接受参数值。`);
        element("utilityAction").value = utilities.get(option);
        continue;
      }
      if (option === "--constant" || option === "--symbol-count") {
        const taken = takeOptionValue(tokens, index, inlineValue, option);
        index = taken.nextIndex;
        (option === "--constant" ? constants : counts).push(taken.value);
        continue;
      }

      const control = controls.get(option);
      if (!control) throw new Error(`网页表单不认识参数：${option}`);
      if (control.type === "checkbox") {
        if (inlineValue !== null) throw new Error(`${option} 是开关，不接受参数值。`);
        control.checked = true;
        continue;
      }
      const taken = takeOptionValue(tokens, index, inlineValue, option);
      index = taken.nextIndex;
      let value = taken.value;
      if (option === "--error-range" && inlineValue === null && isRangeEndpoint(value) &&
          index + 1 < tokens.length && isRangeEndpoint(tokens[index + 1])) {
        value = `(${value},${tokens[index + 1]})`;
        index += 1;
      }
      control.value = value;
    }

    element("customConstants").value = constants.join("\n");
    element("symbolCounts").value = counts.join("\n");
    clearPresetSelection();
    syncSymbolPickers();
    refreshCommand();
    showToast(`已导入 ${tokens.length} 个命令行参数。`);
  } catch (error) {
    snapshot.forEach(({ control, value, checked }) => {
      control.value = value;
      if (control.type === "checkbox" || control.type === "radio") control.checked = checked;
    });
    syncSymbolPickers();
    validationMessage.textContent = String(error.message || error);
    validationMessage.className = "validation-message error";
    runButton.disabled = true;
    showToast("命令导入失败，请检查提示。", true);
  }
}

function refreshCommand() {
  const args = buildArguments();
  commandPreview.value = [".\\fates.exe", ...args.map(quotePowerShell)].join(" ");
  const validation = validateConfiguration(args);
  validationMessage.textContent = validation.message;
  validationMessage.className = `validation-message ${validation.valid ? "ok" : "error"}`;
  runButton.disabled = runtime.running || !validation.valid;
  syncSymbolPickers();
  return { args, validation };
}

function applyPreset(name) {
  const preservedTarget = element("target").value;
  form.reset();
  element("target").value = preservedTarget;
  element("customConstants").value = "";
  element("symbolCounts").value = "";
  element("symbolOrder").value = "";
  const values = presets[name];
  for (const [id, value] of Object.entries(values)) {
    const input = element(id);
    if (!input) continue;
    if (input.type === "checkbox") input.checked = Boolean(value);
    else input.value = String(value);
  }
  document.querySelectorAll("[data-preset]").forEach((button) => {
    button.classList.toggle("active", button.dataset.preset === name);
  });
  if (name === "genetic") {
    element("genetic").checked = false;
  }
  if (name === "equation") {
    element("inverseDepth").value = "0";
    element("deepRounds").value = "0";
    element("searchMode").value = "deterministic";
    element("genetic").checked = false;
  }
  refreshCommand();
}

function setRunState(status, detail = "") {
  const labels = {
    idle: "等待配置",
    queued: "任务排队中",
    running: "正在搜索表达式",
    completed: "搜索完成",
    failed: "搜索失败",
    cancelled: "搜索已停止",
  };
  runStatus.textContent = labels[status] || status;
  runMetrics.textContent = detail || "尚未运行";
  runDot.className = `run-dot ${status}`;
}

function resetOutput() {
  runtime.stdoutOffset = 0;
  runtime.stderrOffset = 0;
  runtime.stdoutParts = [];
  runtime.stderrParts = [];
  runtime.stdoutLength = 0;
  runtime.stderrLength = 0;
  runtime.stdoutTail = "";
  runtime.stderrTail = "";
  runtime.stderrLineBuffer = "";
  runtime.liveData = null;
  runtime.finalData = null;
  runtime.cancelRequested = false;
  stdoutOutput.textContent = "等待 Fates 输出…";
  stderrOutput.textContent = "等待实时进度或统计…";
  liveOutput.textContent = "";
  liveOutput.classList.add("hidden");
  liveEmpty.classList.remove("hidden");
  liveTableWrap.classList.add("hidden");
  liveResultTable.querySelector("thead").replaceChildren();
  liveResultTable.querySelector("tbody").replaceChildren();
  liveTitle.textContent = "等待实时结果";
  liveMeta.textContent = "启用 --live 或 --live-top 后显示当前 top N";
  fallbackOutput.classList.add("hidden");
  resultTableWrap.classList.add("hidden");
  emptyState.classList.remove("hidden");
  resultTable.querySelector("thead").replaceChildren();
  resultTable.querySelector("tbody").replaceChildren();
  resultSummary.textContent = "运行中";
}

async function startJob() {
  const { args, validation } = refreshCommand();
  if (!validation.valid || runtime.running) return;
  resetOutput();
  runtime.running = true;
  runtime.startedAt = Date.now() / 1000;
  runButton.disabled = true;
  cancelButton.disabled = false;
  setRunState("queued", "正在提交到本地服务器");
  try {
    const response = await fetch("/api/jobs", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ args }),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    runtime.jobId = payload.id;
    runtime.startedAt = payload.started_at || runtime.startedAt;
    setRunState(payload.status, "已创建本地任务");
    if (runtime.cancelRequested) {
      await cancelJob();
      return;
    }
    schedulePoll(0);
  } catch (error) {
    finishLocalFailure(error);
  }
}

function schedulePoll(delay = 300) {
  clearTimeout(runtime.pollTimer);
  runtime.pollTimer = setTimeout(pollJob, delay);
}

async function pollJob() {
  if (!runtime.jobId) return;
  try {
    const query = new URLSearchParams({
      stdout_offset: String(runtime.stdoutOffset),
      stderr_offset: String(runtime.stderrOffset),
    });
    const response = await fetch(`/api/jobs/${runtime.jobId}?${query}`);
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);

    const stdoutChunk = payload.stdout || "";
    const stderrChunk = payload.stderr || "";
    appendCapture("stdout", stdoutChunk);
    appendCapture("stderr", stderrChunk);
    ingestLiveEvents(stderrChunk);
    runtime.stdoutOffset = payload.stdout_offset;
    runtime.stderrOffset = payload.stderr_offset;
    scheduleRawRender();
    if (!runtime.liveData && stderrChunk) scheduleLiveFallback();

    const startedAt = payload.started_at || runtime.startedAt;
    const endedAt = payload.ended_at || Date.now() / 1000;
    const elapsed = startedAt ? Math.max(0, endedAt - startedAt) : 0;
    setRunState(payload.status, `${elapsed.toFixed(2)} 秒 · stdout ${runtime.stdoutLength} 字符`);
    resultSummary.textContent = `${payload.status} · ${elapsed.toFixed(2)}s`;

    if (terminalStates.has(payload.status)) {
      runtime.running = false;
      cancelButton.disabled = true;
      refreshCommand();
      renderResults(payload.status);
      return;
    }
    schedulePoll(stdoutChunk || stderrChunk ? 140 : 360);
  } catch (error) {
    finishLocalFailure(error);
  }
}

function scrollToEnd(node) {
  node.scrollTop = node.scrollHeight;
}

function appendCapture(channel, chunk) {
  if (!chunk) return;
  const partsName = `${channel}Parts`;
  const lengthName = `${channel}Length`;
  const tailName = `${channel}Tail`;
  runtime[partsName].push(chunk);
  runtime[lengthName] += chunk.length;
  runtime[tailName] = (runtime[tailName] + chunk).slice(-rawTailLimit);
}

function joinedCapture(channel) {
  return runtime[`${channel}Parts`].join("");
}

function scheduleRawRender() {
  if (runtime.rawRenderFrame !== null) return;
  runtime.rawRenderFrame = requestAnimationFrame(() => {
    runtime.rawRenderFrame = null;
    const stdoutPrefix = runtime.stdoutLength > runtime.stdoutTail.length ? "…（仅显示末尾）\n" : "";
    const stderrPrefix = runtime.stderrLength > runtime.stderrTail.length ? "…（仅显示末尾）\n" : "";
    stdoutOutput.textContent = runtime.stdoutTail ? stdoutPrefix + runtime.stdoutTail : "等待 Fates 输出…";
    stderrOutput.textContent = runtime.stderrTail ? stderrPrefix + runtime.stderrTail : "等待实时进度或统计…";
    const rawPane = document.querySelector('[data-pane="raw-output"].active');
    if (rawPane) {
      scrollToEnd(stdoutOutput);
      scrollToEnd(stderrOutput);
    }
  });
}

function ingestLiveEvents(chunk) {
  if (!chunk) return;
  runtime.stderrLineBuffer += chunk;
  const lines = runtime.stderrLineBuffer.split(/\r?\n/);
  runtime.stderrLineBuffer = lines.pop() || "";
  for (const line of lines) {
    const prefixAt = line.indexOf(liveEventPrefix);
    if (prefixAt < 0) continue;
    try {
      const event = JSON.parse(line.slice(prefixAt + liveEventPrefix.length));
      if (!Array.isArray(event.results)) continue;
      runtime.liveData = event;
      scheduleLiveRender();
    } catch {
      continue;
    }
  }
}

function scheduleLiveRender() {
  if (runtime.liveRenderFrame !== null) return;
  runtime.liveRenderFrame = requestAnimationFrame(() => {
    runtime.liveRenderFrame = null;
    if (runtime.liveData) renderLiveTable(runtime.liveData);
  });
}

function scheduleLiveFallback() {
  if (runtime.liveData) return;
  requestAnimationFrame(() => {
    if (runtime.liveData || !runtime.stderrTail) return;
    liveEmpty.classList.add("hidden");
    liveTableWrap.classList.add("hidden");
    liveOutput.classList.remove("hidden");
    liveOutput.textContent = runtime.stderrTail;
    scrollToEnd(liveOutput);
  });
}

function finishLocalFailure(error) {
  runtime.running = false;
  runtime.jobId = null;
  cancelButton.disabled = true;
  setRunState("failed", String(error.message || error));
  validationMessage.textContent = `服务器错误：${error.message || error}`;
  validationMessage.className = "validation-message error";
  refreshCommand();
}

async function cancelJob() {
  if (!runtime.running) return;
  runtime.cancelRequested = true;
  cancelButton.disabled = true;
  if (!runtime.jobId) {
    setRunState("running", "任务创建后立即停止…");
    return;
  }
  try {
    const response = await fetch(`/api/jobs/${runtime.jobId}/cancel`, { method: "POST" });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    setRunState("running", "正在停止本地进程…");
    schedulePoll(50);
  } catch (error) {
    finishLocalFailure(error);
  }
}

function renderResults(status) {
  if (status !== "completed") {
    showFallback(joinedCapture("stdout") || joinedCapture("stderr") || "任务未产生输出。");
    return;
  }
  const text = joinedCapture("stdout").trim();
  if (!text) {
    showFallback("Fates 没有返回 stdout。请查看实时/原始输出标签。");
    return;
  }
  try {
    const data = JSON.parse(text);
    if (Array.isArray(data.results)) {
      renderResultTable(data);
    } else {
      renderObjectTable(data);
    }
  } catch {
    showFallback(text);
  }
}

function clearResultViews() {
  emptyState.classList.add("hidden");
  fallbackOutput.classList.add("hidden");
  resultTableWrap.classList.add("hidden");
}

function showFallback(text) {
  runtime.finalData = null;
  clearResultViews();
  fallbackOutput.textContent = text;
  fallbackOutput.classList.remove("hidden");
  resultSummary.textContent = "文本输出";
}

function createCell(value) {
  const cell = document.createElement("td");
  cell.textContent = value;
  cell.title = value;
  return cell;
}

function createExpressionCell(text, latex) {
  const cell = document.createElement("td");
  cell.className = "expression-cell";
  cell.title = text || latex || "";
  const content = document.createElement(latex ? "button" : "div");
  content.className = "expression-content";
  if (latex) {
    content.type = "button";
    content.classList.add("copyable");
    content.dataset.latex = latex;
    content.setAttribute("aria-label", `复制 LaTeX 源码：${latex}`);
    content.title = "点击复制 LaTeX 源码";
    content.addEventListener("click", async () => {
      try {
        await writeClipboard(latex);
        showToast("已复制 LaTeX 源码。");
      } catch {
        showToast("无法访问剪贴板，请检查浏览器权限。", true);
      }
    });
  }
  const useLatex = runtime.renderMode === "latex" && latex;
  if (useLatex && window.katex) {
    try {
      window.katex.render(latex, content, {
        displayMode: false,
        throwOnError: true,
        strict: "error",
        trust: false,
        maxExpand: 1000,
        maxSize: 24,
      });
    } catch {
      content.classList.add("katex-error");
      content.textContent = text || latex;
    }
  } else {
    content.classList.add("text-mode");
    content.textContent = text || latex || "—";
  }
  cell.append(content);
  return cell;
}

function formatNumber(value) {
  if (value === null || value === undefined) return "—";
  const number = Number(value);
  if (!Number.isFinite(number)) return String(value);
  const absolute = Math.abs(number);
  if ((absolute > 0 && absolute < 1e-4) || absolute >= 1e7) return number.toExponential(6);
  return Number(number.toPrecision(13)).toString();
}

function renderResultTable(data) {
  runtime.finalData = data;
  clearResultViews();
  renderSearchRows(resultTable, data);
  resultTableWrap.classList.remove("hidden");
  const seconds = data.stats?.seconds;
  resultSummary.textContent = `${data.results.length} 条结果${seconds !== undefined ? ` · ${Number(seconds).toFixed(3)}s` : ""}`;
}

function renderSearchRows(table, data) {
  const equations = data.search_mode === "equations"
    || data.type === "equations"
    || data.results.some((row) => "equation" in row);
  const columns = equations
    ? ["#", "成本", "估计根", "有符号误差", "绝对误差", "残差", "方程"]
    : ["#", "成本", "值", "有符号误差", "绝对误差", "相对误差", "表达式"];
  const headRow = document.createElement("tr");
  columns.forEach((name) => {
    const cell = document.createElement("th");
    cell.textContent = name;
    headRow.append(cell);
  });
  table.querySelector("thead").replaceChildren(headRow);

  const body = document.createDocumentFragment();
  data.results.forEach((row, index) => {
    const tableRow = document.createElement("tr");
    const numericValues = equations
      ? [index + 1, row.cost, row.estimated_root, row.signed_error, row.absolute_error, row.residual_at_target]
      : [index + 1, row.cost, row.value, row.signed_error, row.absolute_error, row.relative_error];
    numericValues.forEach((value, valueIndex) => {
      tableRow.append(createCell(valueIndex >= 2 ? formatNumber(value) : String(value ?? "—")));
    });
    tableRow.append(createExpressionCell(
      equations ? row.equation : row.expression,
      row.latex,
    ));
    body.append(tableRow);
  });
  table.querySelector("tbody").replaceChildren(body);
}

function renderLiveTable(data) {
  liveEmpty.classList.add("hidden");
  liveOutput.classList.add("hidden");
  renderSearchRows(liveResultTable, data);
  liveTableWrap.classList.remove("hidden");
  liveTitle.textContent = data.type === "equations" ? "当前最优方程" : "当前 Top N 表达式";
  const total = data.capacity === undefined ? data.results.length : `${data.results.length}/${data.capacity}`;
  const cost = data.total_cost && data.total_cost !== data.cost
    ? `单边 ${data.cost} · 总成本 ${data.total_cost}`
    : `成本 ${data.cost}`;
  liveMeta.textContent = `${cost} · ${total} 条 · ${Number(data.elapsed || 0).toFixed(2)} 秒`;
}

function renderObjectTable(data) {
  runtime.finalData = null;
  clearResultViews();
  const headRow = document.createElement("tr");
  ["configuration", "value"].forEach((name) => {
    const cell = document.createElement("th");
    cell.textContent = name;
    headRow.append(cell);
  });
  resultTable.querySelector("thead").replaceChildren(headRow);
  const body = document.createDocumentFragment();
  for (const [key, value] of Object.entries(data)) {
    const row = document.createElement("tr");
    row.append(createCell(key), createCell(typeof value === "object" ? JSON.stringify(value) : String(value)));
    body.append(row);
  }
  resultTable.querySelector("tbody").replaceChildren(body);
  resultTableWrap.classList.remove("hidden");
  resultSummary.textContent = "配置结果";
}

function setRenderMode(mode) {
  if (mode !== "latex" && mode !== "text") return;
  runtime.renderMode = mode;
  renderModeButtons.forEach((button) => {
    button.classList.toggle("active", button.dataset.renderMode === mode);
  });
  try {
    localStorage.setItem("fates-render-mode", mode);
  } catch {
  }
  if (runtime.finalData) renderResultTable(runtime.finalData);
  if (runtime.liveData) renderLiveTable(runtime.liveData);
}

function activateTab(name) {
  document.querySelectorAll(".tab").forEach((tab) => tab.classList.toggle("active", tab.dataset.tab === name));
  document.querySelectorAll(".tab-pane").forEach((pane) => pane.classList.toggle("active", pane.dataset.pane === name));
  if (name === "raw-output") scheduleRawRender();
}

let toastTimer = null;

function showToast(message, error = false) {
  toast.textContent = message;
  toast.classList.toggle("error", error);
  toast.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("visible"), 1500);
}

async function writeClipboard(value) {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(value);
      return;
    } catch {
    }
  }
  const fallback = document.createElement("textarea");
  fallback.value = value;
  fallback.setAttribute("readonly", "");
  fallback.style.position = "fixed";
  fallback.style.left = "-10000px";
  document.body.append(fallback);
  fallback.select();
  const copied = document.execCommand("copy");
  fallback.remove();
  if (!copied) throw new Error("clipboard unavailable");
}

async function copyCommand() {
  try {
    await writeClipboard(commandPreview.value);
    const button = element("copyCommand");
    const oldText = button.textContent;
    button.textContent = "已复制";
    setTimeout(() => { button.textContent = oldText; }, 1000);
    showToast("命令已复制。");
  } catch {
    validationMessage.textContent = "浏览器未授予剪贴板权限，请手动复制命令。";
    validationMessage.className = "validation-message error";
    showToast("命令复制失败。", true);
  }
}

async function initializeServerStatus() {
  try {
    const response = await fetch("/api/meta");
    const metadata = await response.json();
    if (!response.ok) throw new Error(metadata.error || `HTTP ${response.status}`);
    renderSymbolCatalog(metadata.symbols);
    element("serverDot").classList.add("online");
    element("serverLabel").textContent = "本地服务器已连接";
    element("versionLabel").textContent = metadata.fates_version;
  } catch (error) {
    element("serverDot").classList.add("offline");
    element("serverLabel").textContent = "本地服务器不可用";
    element("versionLabel").textContent = String(error.message || error);
    runButton.disabled = true;
  }
}

let commandRefreshFrame = null;

function scheduleCommandRefresh() {
  if (commandRefreshFrame !== null) return;
  commandRefreshFrame = requestAnimationFrame(() => {
    commandRefreshFrame = null;
    refreshCommand();
  });
}

form.addEventListener("input", () => {
  clearPresetSelection();
  scheduleCommandRefresh();
});
form.addEventListener("change", refreshCommand);
runButton.addEventListener("click", startJob);
cancelButton.addEventListener("click", cancelJob);
element("copyCommand").addEventListener("click", copyCommand);
element("importCommand").addEventListener("click", importCommand);
commandPreview.addEventListener("input", () => {
  validationMessage.textContent = "命令已修改；请先导入到表单，再开始搜索。";
  validationMessage.className = "validation-message";
  runButton.disabled = true;
});
commandPreview.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
    event.preventDefault();
    importCommand();
  }
});
element("resetButton").addEventListener("click", () => {
  form.reset();
  element("customConstants").value = "";
  element("symbolCounts").value = "";
  clearPresetSelection();
  refreshCommand();
});

function setCatalogSelection(inputId, entries, predicate) {
  const existing = new Map(csvEntries(inputId).map((entry) => [entryName(entry), entry]));
  const selected = entries.filter(predicate).map((item) => existing.get(item.name) || item.name);
  element(inputId).value = selected.length ? selected.join(",") : "none";
  clearPresetSelection();
  refreshCommand();
}

document.querySelectorAll("[data-catalog-action]").forEach((button) => {
  button.addEventListener("click", () => {
    if (!symbolCatalog) return;
    const action = button.dataset.catalogAction;
    if (action === "constants-all") {
      setCatalogSelection("constants", symbolCatalog.constants, () => true);
    } else if (action === "constants-none") {
      setCatalogSelection("constants", symbolCatalog.constants, () => false);
    } else {
      const operators = [...symbolCatalog.binary_operators, ...symbolCatalog.unary_operators];
      if (action === "operators-all") setCatalogSelection("ops", operators, () => true);
      else if (action === "operators-none") setCatalogSelection("ops", operators, () => false);
      else if (action === "operators-default") {
        setCatalogSelection("ops", operators, (item) => item.default_enabled === true);
      }
    }
  });
});

document.querySelectorAll("[data-preset]").forEach((button) => {
  button.addEventListener("click", () => applyPreset(button.dataset.preset));
});

document.querySelectorAll(".tab").forEach((tab) => {
  tab.addEventListener("click", () => activateTab(tab.dataset.tab));
});

renderModeButtons.forEach((button) => {
  button.addEventListener("click", () => setRenderMode(button.dataset.renderMode));
});

document.querySelectorAll(".option-group").forEach((group) => {
  const summary = group.querySelector(":scope > summary");
  if (summary) {
    const reset = document.createElement("button");
    reset.type = "button";
    reset.className = "section-reset";
    reset.textContent = "恢复本组";
    reset.title = "只恢复这个配置板块的默认值";
    reset.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      group.querySelectorAll("input, select, textarea").forEach(resetControl);
      clearPresetSelection();
      refreshCommand();
      showToast("已恢复当前板块的默认值。");
    });
    summary.append(reset);
  }
  group.dataset.searchIndex = `${group.dataset.searchText || ""} ${group.textContent}`.toLowerCase();
});

element("optionFilter").addEventListener("input", (event) => {
  const query = event.target.value.trim().toLowerCase();
  document.querySelectorAll(".option-group").forEach((group) => {
    const visible = !query || group.dataset.searchIndex.includes(query);
    group.classList.toggle("filtered-out", !visible);
    if (query && visible) group.open = true;
  });
});

window.addEventListener("beforeunload", () => {
  if (runtime.running && runtime.jobId) {
    navigator.sendBeacon(`/api/jobs/${runtime.jobId}/cancel`, "{}");
  }
});

setRenderMode(runtime.renderMode);
refreshCommand();
initializeServerStatus();
