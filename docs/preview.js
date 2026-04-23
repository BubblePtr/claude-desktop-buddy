const SPECIES = [
  "capybara",
  "duck",
  "goose",
  "blob",
  "cat",
  "dragon",
  "octopus",
  "owl",
  "penguin",
  "turtle",
  "snail",
  "ghost",
  "axolotl",
  "cactus",
  "robot",
  "rabbit",
  "mushroom",
  "chonk",
];
const HUD_WIDTH = 25;
const KEEPALIVE_MS = 8000;

const elements = {
  connectBtn: document.getElementById("connectBtn"),
  disconnectBtn: document.getElementById("disconnectBtn"),
  applyBtn: document.getElementById("applyBtn"),
  snapshotBtn: document.getElementById("snapshotBtn"),
  clearLogBtn: document.getElementById("clearLogBtn"),
  serialStatus: document.getElementById("serialStatus"),
  mirrorState: document.getElementById("mirrorState"),
  snapshotStatus: document.getElementById("snapshotStatus"),
  serialLog: document.getElementById("serialLog"),
  speciesSelect: document.getElementById("speciesSelect"),
  presetSelect: document.getElementById("presetSelect"),
  stateSelect: document.getElementById("stateSelect"),
  baudSelect: document.getElementById("baudSelect"),
  totalInput: document.getElementById("totalInput"),
  runningInput: document.getElementById("runningInput"),
  waitingInput: document.getElementById("waitingInput"),
  msgInput: document.getElementById("msgInput"),
  entry1Input: document.getElementById("entry1Input"),
  entry2Input: document.getElementById("entry2Input"),
  entry3Input: document.getElementById("entry3Input"),
  tokensTodayInput: document.getElementById("tokensTodayInput"),
  promptToolInput: document.getElementById("promptToolInput"),
  promptHintInput: document.getElementById("promptHintInput"),
  autoApplyInput: document.getElementById("autoApplyInput"),
  clockHint: document.getElementById("clockHint"),
  attentionBar: document.getElementById("attentionBar"),
  petAscii: document.getElementById("petAscii"),
  clockLayer: document.getElementById("clockLayer"),
  clockTime: document.getElementById("clockTime"),
  clockSeconds: document.getElementById("clockSeconds"),
  clockDate: document.getElementById("clockDate"),
  hudLayer: document.getElementById("hudLayer"),
  hudSummary: document.getElementById("hudSummary"),
  hudLine1: document.getElementById("hudLine1"),
  hudLine2: document.getElementById("hudLine2"),
  approvalLayer: document.getElementById("approvalLayer"),
  approvalTimer: document.getElementById("approvalTimer"),
  approvalTool: document.getElementById("approvalTool"),
  approvalHint: document.getElementById("approvalHint"),
  snapshotAutoInput: document.getElementById("snapshotAutoInput"),
  snapshotIntervalSelect: document.getElementById("snapshotIntervalSelect"),
  snapshotCanvas: document.getElementById("snapshotCanvas"),
  snapshotMeta: document.getElementById("snapshotMeta"),
};

let promptStartedAt = Date.now();
let autoApplyTimer = null;
let keepaliveTimer = null;
let snapshotTimer = null;

let port = null;
let writer = null;
let reader = null;
let inputDone = null;
let screenshotCapture = null;

const snapshotContext = elements.snapshotCanvas.getContext("2d");
snapshotContext.imageSmoothingEnabled = false;

function appendLog(kind, text) {
  const line = document.createElement("div");
  line.className = kind;
  line.textContent = text.trimEnd();
  elements.serialLog.append(line);
  elements.serialLog.scrollTop = elements.serialLog.scrollHeight;
}

function setSerialStatus(label, online = false) {
  elements.serialStatus.textContent = label;
  elements.serialStatus.classList.toggle("online", online);
}

function setSnapshotStatus(label, online = false) {
  if (!elements.snapshotStatus) return;
  elements.snapshotStatus.textContent = label;
  elements.snapshotStatus.classList.toggle("online", online);
}

function browserSupportsSerial() {
  return "serial" in navigator;
}

function toInt(value, fallback = 0) {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function getModel() {
  return {
    species: elements.speciesSelect.value,
    preset: elements.presetSelect.value,
    debugState: elements.stateSelect.value,
    total: toInt(elements.totalInput.value, 0),
    running: toInt(elements.runningInput.value, 0),
    waiting: toInt(elements.waitingInput.value, 0),
    msg: elements.msgInput.value.trim(),
    entries: [
      elements.entry1Input.value.trim(),
      elements.entry2Input.value.trim(),
      elements.entry3Input.value.trim(),
    ].filter(Boolean),
    tokensToday: toInt(elements.tokensTodayInput.value, 0),
    promptTool: elements.promptToolInput.value.trim(),
    promptHint: elements.promptHintInput.value.trim(),
    autoApply: elements.autoApplyInput.checked,
  };
}

function computeClockState(now) {
  const dow = now.getDay();
  const hour = now.getHours();
  const weekend = dow === 0 || dow === 6;
  const friday = dow === 5;
  const seconds = Math.floor(now.getTime() / 1000);

  if (hour >= 1 && hour < 7) return "sleep";
  if (weekend) return Math.floor(seconds / 8) % 6 === 0 ? "heart" : "sleep";
  if (hour < 9) return Math.floor(seconds / 6) % 4 === 0 ? "idle" : "sleep";
  if (hour === 12) return Math.floor(seconds / 5) % 3 === 0 ? "heart" : "idle";
  if (friday && hour >= 15) return Math.floor(seconds / 4) % 3 === 0 ? "celebrate" : "idle";
  if (hour >= 22 || hour === 0) return Math.floor(seconds / 7) % 3 === 0 ? "dizzy" : "sleep";
  return Math.floor(seconds / 10) % 5 === 0 ? "sleep" : "idle";
}

function deriveState(model) {
  if (model.preset === "clock") return computeClockState(new Date());
  if (model.debugState !== "auto") return model.debugState;
  if (model.preset === "approval" || model.waiting > 0) return "attention";
  if (model.running > 0) return "busy";
  return "idle";
}

function wrapLines(input, width, limit) {
  if (!input) return [];
  const words = input.split(/\s+/).filter(Boolean);
  if (!words.length) return [];

  const lines = [];
  let line = "";
  for (const word of words) {
    const sep = line ? " " : "";
    if ((line + sep + word).length > width) {
      if (line) lines.push(line);
      if (lines.length >= limit) return lines.slice(0, limit);
      line = word.slice(0, width);
      if (word.length > width) lines.push(line);
      if (lines.length >= limit) return lines.slice(0, limit);
      line = word.length > width ? word.slice(width) : word;
      if (line.length > width) line = line.slice(0, width);
      if (word.length > width) line = "";
    } else {
      line += sep + word;
    }
  }
  if (line && lines.length < limit) lines.push(line);
  return lines.slice(0, limit);
}

function buildHudLines(model) {
  const wrapped = [];
  for (const entry of model.entries) {
    wrapped.push(...wrapLines(entry, HUD_WIDTH, 3));
    if (wrapped.length >= 2) break;
  }
  if (!wrapped.length) {
    wrapped.push(...wrapLines(model.msg || "preview: idle", HUD_WIDTH, 2));
  }
  return [wrapped[0] || "", wrapped[1] || ""];
}

function buildHeartbeat(model) {
  const heartbeat = {
    total: Math.max(model.total, model.running, model.waiting ? 1 : 0),
    running: model.running,
    waiting: model.waiting,
    msg: model.msg || "preview: idle",
    entries: model.entries.slice(0, 3),
    tokens_today: model.tokensToday,
  };

  if (model.preset === "clock") {
    heartbeat.running = 0;
    heartbeat.waiting = 0;
    heartbeat.total = Math.max(heartbeat.total, 1);
  }

  if (model.preset === "approval") {
    heartbeat.waiting = Math.max(heartbeat.waiting, 1);
    heartbeat.total = Math.max(heartbeat.total, heartbeat.waiting);
    heartbeat.prompt = {
      id: "req_preview",
      tool: model.promptTool || "Edit",
      hint: model.promptHint || "Edit src/buddies/cat.cpp",
    };
  }

  return heartbeat;
}

function currentClockPayload() {
  const now = new Date();
  return { time: [Math.floor(now.getTime() / 1000), -now.getTimezoneOffset() * 60] };
}

async function sendJson(obj) {
  if (!writer) return;
  const line = `${JSON.stringify(obj)}\n`;
  await writer.write(new TextEncoder().encode(line));
  appendLog("tx", `> ${line.trim()}`);
}

async function applyToDevice({ fromKeepalive = false } = {}) {
  if (!writer) return;
  const model = getModel();

  try {
    if (!fromKeepalive) {
      await sendJson({ cmd: "species", idx: Number(elements.speciesSelect.selectedIndex) });
      await sendJson({ cmd: "debug_state", state: model.debugState });
    }
    if (model.preset === "clock") await sendJson(currentClockPayload());
    await sendJson(buildHeartbeat(model));
    if (!fromKeepalive && model.preset === "approval") promptStartedAt = Date.now();
  } catch (error) {
    appendLog("meta", `! send failed: ${error instanceof Error ? error.message : error}`);
  }
}

function scheduleApply() {
  if (!elements.autoApplyInput.checked) return;
  window.clearTimeout(autoApplyTimer);
  autoApplyTimer = window.setTimeout(() => {
    applyToDevice();
  }, 120);
}

function updateClockMirror() {
  if (!elements.clockTime || !elements.clockSeconds || !elements.clockDate) return;
  const now = new Date();
  elements.clockTime.textContent = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`;
  elements.clockSeconds.textContent = `:${String(now.getSeconds()).padStart(2, "0")}`;
  elements.clockDate.textContent = now.toLocaleDateString(undefined, { month: "short", day: "2-digit" });
}

function renderMirror() {
  if (elements.clockHint) elements.clockHint.hidden = elements.presetSelect.value !== "clock";
}

function decodeBase64ToBytes(base64) {
  const binary = window.atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function renderSnapshotFrame(frame) {
  if (!frame || frame.format !== "rgb332") return;

  const expected = frame.width * frame.height;
  if (frame.bytes.length < expected) {
    appendLog("meta", `! snapshot incomplete: expected ${expected}, got ${frame.bytes.length}`);
    setSnapshotStatus("snapshot failed");
    return;
  }

  elements.snapshotCanvas.width = frame.width;
  elements.snapshotCanvas.height = frame.height;
  const image = snapshotContext.createImageData(frame.width, frame.height);

  for (let i = 0; i < expected; i++) {
    const value = frame.bytes[i];
    const r = ((value >> 5) & 0x07) * 255 / 7;
    const g = ((value >> 2) & 0x07) * 255 / 7;
    const b = (value & 0x03) * 255 / 3;
    const offset = i * 4;
    image.data[offset] = Math.round(r);
    image.data[offset + 1] = Math.round(g);
    image.data[offset + 2] = Math.round(b);
    image.data[offset + 3] = 255;
  }

  snapshotContext.putImageData(image, 0, 0);
  elements.snapshotMeta.textContent =
    `Last frame: ${frame.width}x${frame.height} ${frame.format}, ${frame.bytes.length} bytes, ${frame.chunks} chunks.`;
  setSnapshotStatus("snapshot fresh", true);
}

function handleSerialLine(line) {
  let payload = null;
  try {
    payload = JSON.parse(line);
  } catch (_) {
    appendLog("rx", `< ${line}`);
    return;
  }

  const ack = payload.ack;
  if (ack === "screenshot") {
    screenshotCapture = {
      format: payload.fmt,
      width: Number(payload.w) || 0,
      height: Number(payload.h) || 0,
      parts: [],
      chunks: 0,
    };
    setSnapshotStatus("snapshot loading", true);
    appendLog("meta", `* snapshot start ${payload.w}x${payload.h} ${payload.fmt}`);
    return;
  }

  if (ack === "screenshot_chunk") {
    if (!screenshotCapture) return;
    screenshotCapture.parts.push(payload.d || "");
    screenshotCapture.chunks += 1;
    return;
  }

  if (ack === "screenshot_end") {
    if (!screenshotCapture) return;
    const frame = {
      format: screenshotCapture.format,
      width: screenshotCapture.width,
      height: screenshotCapture.height,
      chunks: screenshotCapture.chunks,
      bytes: decodeBase64ToBytes(screenshotCapture.parts.join("")),
    };
    screenshotCapture = null;
    renderSnapshotFrame(frame);
    appendLog("meta", `* snapshot complete ${frame.bytes.length} bytes in ${frame.chunks} chunks`);
    return;
  }

  appendLog("rx", `< ${line}`);
}

async function readLoop() {
  const decoder = new TextDecoderStream();
  inputDone = port.readable.pipeTo(decoder.writable);
  reader = decoder.readable.getReader();
  let buffer = "";

  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += value;
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop() ?? "";
      for (const line of lines) {
        if (line.trim()) handleSerialLine(line);
      }
    }
  } catch (error) {
    if (error && error.name !== "AbortError") {
      appendLog("meta", `! read failed: ${error.message || error}`);
    }
  } finally {
    reader?.releaseLock();
    reader = null;
  }
}

async function connectSerial() {
  if (!browserSupportsSerial()) {
    appendLog("meta", "! Web Serial not supported in this browser");
    return;
  }

  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: Number(elements.baudSelect.value) || 921600 });
    writer = port.writable.getWriter();
    setSerialStatus("serial online", true);
    elements.connectBtn.disabled = true;
    elements.disconnectBtn.disabled = false;
    appendLog("meta", "* serial connected");
    readLoop();
    keepaliveTimer = window.setInterval(() => applyToDevice({ fromKeepalive: true }), KEEPALIVE_MS);
    syncSnapshotLoop();
    await applyToDevice();
    await requestSnapshot();
  } catch (error) {
    appendLog("meta", `! connect failed: ${error instanceof Error ? error.message : error}`);
  }
}

async function disconnectSerial() {
  window.clearInterval(keepaliveTimer);
  keepaliveTimer = null;
  window.clearInterval(snapshotTimer);
  snapshotTimer = null;

  try {
    await reader?.cancel();
  } catch (_) {}
  try {
    await inputDone;
  } catch (_) {}
  try {
    writer?.releaseLock();
  } catch (_) {}
  try {
    await port?.close();
  } catch (_) {}

  port = null;
  writer = null;
  reader = null;
  inputDone = null;
  screenshotCapture = null;
  setSerialStatus("serial offline");
  setSnapshotStatus("snapshot idle");
  elements.snapshotMeta.textContent =
    "Connect over USB to pull the real board framebuffer. Snapshot mode requests the current 184x224 screen over USB and refreshes it in-place. It is low-frequency by design, not video.";
  elements.connectBtn.disabled = false;
  elements.disconnectBtn.disabled = true;
  appendLog("meta", "* serial disconnected");
}

async function requestSnapshot() {
  if (!writer || screenshotCapture) return;
  setSnapshotStatus("snapshot loading", true);
  try {
    await sendJson({ cmd: "screenshot" });
  } catch (error) {
    setSnapshotStatus("snapshot failed");
    appendLog("meta", `! snapshot request failed: ${error instanceof Error ? error.message : error}`);
  }
}

function syncSnapshotLoop() {
  window.clearInterval(snapshotTimer);
  snapshotTimer = null;
  if (!writer || !elements.snapshotAutoInput.checked) return;
  const everyMs = Number(elements.snapshotIntervalSelect.value) || 2000;
  snapshotTimer = window.setInterval(() => {
    requestSnapshot();
  }, everyMs);
}

function populateSpeciesSelect() {
  const frag = document.createDocumentFragment();
  SPECIES.forEach((species, idx) => {
    const option = document.createElement("option");
    option.value = species;
    option.textContent = `${idx}. ${species}`;
    frag.append(option);
  });
  elements.speciesSelect.append(frag);
  elements.speciesSelect.value = SPECIES[0] ?? "";
}

function bindEvents() {
  const inputs = [
    elements.speciesSelect,
    elements.presetSelect,
    elements.stateSelect,
    elements.totalInput,
    elements.runningInput,
    elements.waitingInput,
    elements.msgInput,
    elements.entry1Input,
    elements.entry2Input,
    elements.entry3Input,
    elements.tokensTodayInput,
    elements.promptToolInput,
    elements.promptHintInput,
    elements.autoApplyInput,
  ];

  for (const input of inputs) {
    input.addEventListener("input", () => {
      if (input === elements.presetSelect && elements.presetSelect.value === "approval") {
        promptStartedAt = Date.now();
      }
      renderMirror();
      scheduleApply();
    });
    input.addEventListener("change", () => {
      renderMirror();
      scheduleApply();
    });
  }

  elements.connectBtn.addEventListener("click", connectSerial);
  elements.disconnectBtn.addEventListener("click", disconnectSerial);
  elements.applyBtn.addEventListener("click", () => applyToDevice());
  elements.snapshotBtn.addEventListener("click", () => requestSnapshot());
  elements.clearLogBtn.addEventListener("click", () => {
    elements.serialLog.textContent = "";
  });
  elements.snapshotAutoInput.addEventListener("change", syncSnapshotLoop);
  elements.snapshotIntervalSelect.addEventListener("change", syncSnapshotLoop);
}

async function init() {
  if (!browserSupportsSerial()) {
    setSerialStatus("Web Serial unsupported");
    setSnapshotStatus("snapshot unavailable");
    elements.connectBtn.disabled = true;
  } else {
    setSnapshotStatus("snapshot idle");
  }

  try {
    populateSpeciesSelect();
    bindEvents();
    renderMirror();
  } catch (error) {
    appendLog("meta", `! init failed: ${error instanceof Error ? error.message : error}`);
  }
}

init();
