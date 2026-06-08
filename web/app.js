/* app.js - L26 Web visualizer frontend.
 *
 * Drives the EXISTING pure-C compiler/VM core through the wasm exports defined
 * in src/wasm_api.c. NO compiler logic is reimplemented here: we only call the
 * exported functions and render the JSON they return.
 *
 * Exported C API (via cwrap):
 *   l26_compile(source)  -> JSON {ok,diagnostics,pcode,frameSize,symbols}
 *   l26_step()           -> JSON machine-state
 *   l26_reset()          -> JSON machine-state
 *   l26_run()            -> JSON machine-state
 *   l26_feed_input(line) -> int 1/0
 *   l26_clear_input()    -> void
 *   l26_frame_size()     -> int
 */

"use strict";

/* ------------------------------------------------------------------ */
/* Example programs (mirrors of tests/*.l26).                          */
/* ------------------------------------------------------------------ */
const EXAMPLES = {
  example1: {
    label: "example1 — read + in + add + while",
    src: `{
    int val;
    set s;
    s={1,2,3};
    read val;
    if (val in s) {
        add s 10;
        write val;
    }
    while (val>0) {
        val = val -1;
        write val;
    }
    write 999;
}`,
  },
  example2: {
    label: "example2 — block scoping / shadowing",
    src: `{
    int x;
    set s;
    x=10;
    s={1,2};
    {
        set x;        // shadows the outer int x
        x={5,6};      // x is now a set here
        add x 7;
        write x;      // {5,6,7}
        write s;      // outer s -> {1,2}
    }
    write x;          // 10: inner scope gone, x is int again
}`,
  },
  example3: {
    label: "example3 — comprehension + set equality + logic",
    src: `{
    // comprehension (bonus) + set equality (bonus) + logic
    set base;
    set derived;
    set expect;
    int n;
    bool same;

    base = {1, 2, 3, 4, 5, 6};

    // comprehension: take x>2, emit x+1  ->  {4,5,6,7}
    derived = { x + 1 | x in base if x > 2 };

    expect = {4, 5, 6, 7};
    same = derived == expect;       // set equality -> bool
    write same;                     // expect 1 (true)

    read n;
    if (n in derived && !isempty(derived)) {
        write n;
        add derived 100;
    } else {
        write 0;
    }

    write derived;
}`,
  },
  gcd: {
    label: "gcd — Euclid (reads two ints)",
    src: `{
    // GCD via repeated subtraction. Reads a and b, writes gcd(a,b).
    int a;
    int b;
    read a;
    read b;
    while (a > b || a < b) {
        if (a > b) {
            a = a - b;
        } else {
            b = b - a;
        }
    }
    write a;
}`,
  },
  factorial: {
    label: "factorial — iterative n! (reads n)",
    src: `{
    int n;
    int fact;
    int k;
    read n;
    fact = 1;
    k = 1;
    while (k <= n) {
        fact = fact * k;
        k = k + 1;
    }
    write fact;
}`,
  },
  set_algebra: {
    label: "set_algebra — union / inter / comprehension / equality",
    src: `{
    // union, intersection, comprehension, set equality, membership
    set a;
    set b;
    set u;
    set i;
    set doubled;
    set expect;

    a = {1, 2, 3, 4, 5};
    b = {4, 5, 6, 7, 8};

    u = a union b;          // {1,2,3,4,5,6,7,8}
    write u;

    i = a inter b;          // {4,5}
    write i;

    // comprehension: keep x>2 from a, emit 2*x -> {6,8,10}
    doubled = { x + x | x in a if x > 2 };
    write doubled;

    expect = {4, 5};
    if (i == expect) { write 1; } else { write 0; }

    if ((6 in u) && !isempty(i)) { write 1; } else { write 0; }
}`,
  },
};

/* ------------------------------------------------------------------ */
/* DOM handles                                                         */
/* ------------------------------------------------------------------ */
const $ = (id) => document.getElementById(id);
const els = {
  status: $("wasm-status"),
  source: $("source"),
  hlLayer: $("hl-layer"),
  exampleSelect: $("example-select"),
  btnCompile: $("btn-compile"),
  btnReset: $("btn-reset"),
  btnStep: $("btn-step"),
  btnRun: $("btn-run"),
  btnClearOutput: $("btn-clear-output"),
  btnClearBp: $("btn-clear-bp"),
  runState: $("run-state"),
  diagnostics: $("diagnostics"),
  output: $("output"),
  pcodeBody: document.querySelector("#pcode tbody"),
  frameInfo: $("frame-info"),
  pcBadge: $("pc-badge"),
  memory: $("memory"),
  stack: $("stack"),
  // input modal
  inputOverlay: $("input-overlay"),
  inputLine: $("input-line"),
  inputFeed: $("input-feed"),
  inputCancel: $("input-cancel"),
  inputTitle: $("input-title"),
  inputHint: $("input-hint"),
};

/* ------------------------------------------------------------------ */
/* Module / API state                                                  */
/* ------------------------------------------------------------------ */
let api = null;            // cwrapped functions
let lastCompile = null;    // parsed JSON of last successful compile
let pcRowEls = [];         // <tr> per pcode index, for highlight
let compiled = false;      // a program is available to step
let waitingForInput = false;
let pendingResume = null;  // function to call after feeding input

const breakpoints = new Set(); // P-Code instruction indices with breakpoints
let errorLines = new Set();    // 1-based source lines flagged by diagnostics
let liveTimer = null;          // debounce handle for live diagnostics
let liveSeq = 0;               // monotonically rising; guards stale debounced runs

/* ------------------------------------------------------------------ */
/* Wasm bootstrap                                                      */
/* ------------------------------------------------------------------ */
function boot() {
  if (typeof L26 !== "function") {
    setStatus("failed", "l26.js not loaded");
    return;
  }
  L26().then((m) => {
    api = {
      compile: m.cwrap("l26_compile", "string", ["string"]),
      check: m.cwrap("l26_check", "string", ["string"]),
      step: m.cwrap("l26_step", "string", []),
      reset: m.cwrap("l26_reset", "string", []),
      run: m.cwrap("l26_run", "string", []),
      feedInput: m.cwrap("l26_feed_input", "number", ["string"]),
      clearInput: m.cwrap("l26_clear_input", null, []),
      frameSize: m.cwrap("l26_frame_size", "number", []),
    };
    setStatus("ready", "wasm ready");
    els.btnCompile.disabled = false;
  }).catch((e) => {
    setStatus("failed", "wasm init failed");
    console.error(e);
  });
}

function setStatus(cls, text) {
  els.status.className = "status " + cls;
  els.status.textContent = text;
}

/* ------------------------------------------------------------------ */
/* Compile                                                             */
/* ------------------------------------------------------------------ */
/* Shared by the manual Compile button and the debounced live compile so
 * both paths behave identically. Recompiling rebuilds the P-Code layout,
 * so breakpoints (keyed by instruction index) are CLEARED here — stale
 * indices would point at the wrong instructions. Likewise, any in-progress
 * run is invalidated: compile auto-inits the VM at pc=0 and we reflect that
 * reset state, which is the predictable behavior when source changes. */
function compileAndRefresh(src) {
  if (!api) return null;
  hideInputModal();
  api.clearInput();
  waitingForInput = false;
  pendingResume = null;

  let res;
  try {
    res = JSON.parse(api.compile(src));
  } catch (e) {
    renderDiagnostics([{ severity: "error", phase: "codegen", line: 0, col: 0,
      message: "internal: bad JSON from compiler: " + e }]);
    return null;
  }

  lastCompile = res;
  renderDiagnostics(res.diagnostics, res.ok, res.frameSize);

  // recompiled layout invalidates breakpoints
  clearBreakpoints();
  renderPcode(res.pcode);

  // inline error markers in the editor track every compile
  errorLines = collectErrorLines(res.diagnostics);
  renderHighlight();

  els.output.textContent = "";
  els.runState.textContent = "";

  compiled = !!res.ok && res.pcode && res.pcode.length > 0;
  els.btnReset.disabled = !compiled;
  els.btnStep.disabled = !compiled;
  els.btnRun.disabled = !compiled;

  if (compiled) {
    // compile already auto-inits VM at pc=0; reflect that state.
    refreshFromState(parseState(api.reset()));
  } else {
    clearMemoryView();
  }
  return res;
}

function doCompile() {
  if (!api) return;
  // a manual compile cancels any pending debounced live compile
  if (liveTimer) { clearTimeout(liveTimer); liveTimer = null; }
  liveSeq++;
  compileAndRefresh(els.source.value);
}

/* Debounced LIVE DIAGNOSTICS: ~300ms after typing stops, run a diagnostics-only
 * check (l26_check) that NEVER touches the VM/program session. This only
 * refreshes the Diagnostics panel and the inline editor error markers — it does
 * NOT rebuild the right-side P-Code and does NOT reset a running program. Only
 * the manual Compile button (doCompile) rebuilds P-Code and re-inits the VM.
 * Stale runs are ignored if the source changed again before the timer fired. */
function scheduleLiveCheck() {
  if (!api) return;
  if (liveTimer) clearTimeout(liveTimer);
  const mySeq = ++liveSeq;
  liveTimer = setTimeout(() => {
    liveTimer = null;
    if (mySeq !== liveSeq) return;   // superseded by a newer edit
    liveCheck(els.source.value);
  }, 300);
}

function liveCheck(src) {
  if (!api) return;
  let res;
  try {
    res = JSON.parse(api.check(src));
  } catch (e) {
    renderDiagnostics([{ severity: "error", phase: "codegen", line: 0, col: 0,
      message: "internal: bad JSON from checker: " + e }]);
    return;
  }
  // diagnostics panel + inline markers only; right-side P-Code/VM untouched.
  renderDiagnostics(res.diagnostics, res.ok, undefined);
  errorLines = collectErrorLines(res.diagnostics);
  renderHighlight();
}

function collectErrorLines(diags) {
  const s = new Set();
  if (diags) {
    diags.forEach((d) => {
      if (d.severity === "error" && d.line >= 1) s.add(d.line | 0);
    });
  }
  return s;
}

/* ------------------------------------------------------------------ */
/* Syntax highlighting (transparent textarea over a colored <pre>)     */
/* ------------------------------------------------------------------ */
const KEYWORDS = new Set([
  "int", "bool", "set", "if", "else", "while",
  "write", "read", "isempty",
]);
const SETOPS = new Set(["add", "remove", "union", "inter", "in"]);
const BOOLS = new Set(["true", "false"]);

function escHtml(s) {
  return s.replace(/[&<>]/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : "&gt;");
}

/* Single-pass scanner over a single line. Returns HTML of colored spans.
 * Mirrors the L26 lexer token set; no catastrophic regex. */
function tokenizeLineToHtml(line) {
  let out = "";
  let i = 0;
  const n = line.length;
  const isIdStart = (c) => (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c === "_";
  const isIdPart = (c) => isIdStart(c) || (c >= "0" && c <= "9");
  const isDigit = (c) => c >= "0" && c <= "9";

  while (i < n) {
    const c = line[i];

    // line comment: // ... to end of line
    if (c === "/" && line[i + 1] === "/") {
      out += '<span class="tok-comment">' + escHtml(line.slice(i)) + "</span>";
      break;
    }

    // whitespace (kept verbatim, no span needed)
    if (c === " " || c === "\t") {
      let j = i + 1;
      while (j < n && (line[j] === " " || line[j] === "\t")) j++;
      out += escHtml(line.slice(i, j));
      i = j;
      continue;
    }

    // numbers
    if (isDigit(c)) {
      let j = i + 1;
      while (j < n && isDigit(line[j])) j++;
      out += '<span class="tok-num">' + escHtml(line.slice(i, j)) + "</span>";
      i = j;
      continue;
    }

    // identifiers / keywords
    if (isIdStart(c)) {
      let j = i + 1;
      while (j < n && isIdPart(line[j])) j++;
      const word = line.slice(i, j);
      let cls = "tok-id";
      if (KEYWORDS.has(word)) cls = "tok-kw";
      else if (SETOPS.has(word)) cls = "tok-setop";
      else if (BOOLS.has(word)) cls = "tok-bool";
      out += '<span class="' + cls + '">' + escHtml(word) + "</span>";
      i = j;
      continue;
    }

    // multi-char operators
    const two = line.slice(i, i + 2);
    if (two === "<=" || two === ">=" || two === "==" || two === "!=" ||
        two === "&&" || two === "||") {
      out += '<span class="tok-op">' + escHtml(two) + "</span>";
      i += 2;
      continue;
    }

    // single-char operators and the comprehension bar
    if ("+-*/<>=!|".indexOf(c) !== -1) {
      out += '<span class="tok-op">' + escHtml(c) + "</span>";
      i += 1;
      continue;
    }

    // punctuation
    if ("{}();,".indexOf(c) !== -1) {
      out += '<span class="tok-punct">' + escHtml(c) + "</span>";
      i += 1;
      continue;
    }

    // anything else: plain
    out += escHtml(c);
    i += 1;
  }
  return out;
}

/* Rebuild the colored layer from the current textarea text, wrapping each
 * source line so we can flag error lines. Keeps scroll in sync. */
function renderHighlight() {
  if (!els.hlLayer) return;
  const text = els.source.value;
  const lines = text.split("\n");
  let html = "";
  for (let k = 0; k < lines.length; k++) {
    const lineNo = k + 1;
    const cls = errorLines.has(lineNo) ? "hl-line err-line" : "hl-line";
    // trailing newline preserved so wrapping/heights match the textarea
    const body = tokenizeLineToHtml(lines[k]);
    html += '<span class="' + cls + '">' + body + "</span>" +
            (k < lines.length - 1 ? "\n" : "");
  }
  // a trailing empty line in the textarea needs a final glyph slot
  els.hlLayer.innerHTML = html + (text.endsWith("\n") ? "\n" : "");
  syncHighlightScroll();
}

function syncHighlightScroll() {
  if (!els.hlLayer) return;
  els.hlLayer.scrollTop = els.source.scrollTop;
  els.hlLayer.scrollLeft = els.source.scrollLeft;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */
function renderDiagnostics(diags, ok, frameSize) {
  els.diagnostics.innerHTML = "";
  const hasItems = diags && diags.length;
  if (ok && !errorPresent(diags)) {
    const div = document.createElement("div");
    div.className = "diag-ok";
    if (frameSize === undefined) {
      // live diagnostics-only check (no codegen): don't claim a compile happened
      div.innerHTML = "✓ No errors " +
        '<span class="frame">(press Compile to build P-Code)</span>';
    } else {
      div.innerHTML = "✓ Compiled OK " +
        '<span class="frame">(frame size: ' + (frameSize | 0) + " cells)</span>";
    }
    els.diagnostics.appendChild(div);
  }
  if (hasItems) {
    diags.forEach((d) => {
      const row = document.createElement("div");
      row.className = "diag " + d.severity;
      row.innerHTML =
        '<span class="loc">' + d.line + ":" + d.col + "</span>" +
        '<span class="sev">' + d.severity + "</span>" +
        '<span class="msg"></span>';
      row.querySelector(".msg").textContent = d.message;
      els.diagnostics.appendChild(row);
    });
  }
  if (!hasItems && !ok) {
    els.diagnostics.innerHTML = '<span class="muted">No diagnostics.</span>';
  }
}
function errorPresent(diags) {
  return diags && diags.some((d) => d.severity === "error");
}

/* ------------------------------------------------------------------ */
/* P-Code table                                                        */
/* ------------------------------------------------------------------ */
function renderPcode(pcode) {
  els.pcodeBody.innerHTML = "";
  pcRowEls = [];
  if (!pcode || !pcode.length) {
    els.pcodeBody.innerHTML =
      '<tr><td colspan="5" class="muted">No program compiled.</td></tr>';
    els.frameInfo.textContent = "";
    return;
  }
  els.frameInfo.textContent = pcode.length + " instructions";
  const frag = document.createDocumentFragment();
  pcode.forEach((ins) => {
    const tr = document.createElement("tr");
    tr.title = "toggle breakpoint";
    tr.innerHTML =
      '<td class="idx">' + ins.idx + "</td>" +
      '<td class="op">' + ins.op + "</td>" +
      "<td>" + ins.l + "</td>" +
      "<td>" + ins.a + "</td>" +
      '<td class="text"></td>';
    tr.querySelector(".text").textContent = ins.text;
    const idx = ins.idx;
    tr.addEventListener("click", () => toggleBreakpoint(idx));
    frag.appendChild(tr);
    pcRowEls[ins.idx] = tr;
  });
  els.pcodeBody.appendChild(frag);
}

/* ------------------------------------------------------------------ */
/* Breakpoints (on intermediate-code / P-Code rows)                    */
/* ------------------------------------------------------------------ */
function toggleBreakpoint(idx) {
  if (breakpoints.has(idx)) breakpoints.delete(idx);
  else breakpoints.add(idx);
  const tr = pcRowEls[idx];
  if (tr) tr.classList.toggle("breakpoint", breakpoints.has(idx));
  updateClearBpBtn();
}

function clearBreakpoints() {
  breakpoints.forEach((idx) => {
    const tr = pcRowEls[idx];
    if (tr) tr.classList.remove("breakpoint");
  });
  breakpoints.clear();
  updateClearBpBtn();
}

function updateClearBpBtn() {
  if (els.btnClearBp) els.btnClearBp.disabled = breakpoints.size === 0;
}

function highlightPc(pc) {
  pcRowEls.forEach((tr) => tr && tr.classList.remove("current"));
  const tr = pcRowEls[pc];
  if (tr) {
    tr.classList.add("current");
    tr.scrollIntoView({ block: "nearest" });
  }
}

/* ------------------------------------------------------------------ */
/* State plumbing                                                      */
/* ------------------------------------------------------------------ */
function parseState(json) {
  try { return JSON.parse(json); }
  catch (e) { console.error("bad state json", e, json); return null; }
}

function refreshFromState(st) {
  if (!st) return;
  highlightPc(st.pc);
  els.pcBadge.textContent = "pc = " + st.pc + (st.base ? "  base = " + st.base : "");
  renderMemory(st);
  renderStack(st);
  if (st.output) appendOutput(st.output);

  waitingForInput = !!st.waitingForInput;

  // run-state label
  let label = "";
  if (st.error) label = "runtime error";
  else if (st.halted) label = "halted";
  else if (st.waitingForInput) label = "waiting for input…";
  else label = "ready";
  els.runState.textContent = label;
  els.runState.style.color =
    st.error ? "var(--err)" : st.halted ? "var(--ok)" : "var(--muted)";

  const done = st.halted || st.error;
  els.btnStep.disabled = done && !st.waitingForInput;
  els.btnRun.disabled = done && !st.waitingForInput;
}

/* ------------------------------------------------------------------ */
/* Memory visualization                                                */
/* ------------------------------------------------------------------ */
const SET_CELLS = 201; // L26_SET_CELLS: cell[0]=count, cell[1..200]=elements

function clearMemoryView() {
  els.memory.innerHTML = '<span class="muted">Compile and step to view memory.</span>';
  els.stack.innerHTML = '<span class="muted">empty</span>';
  els.pcBadge.textContent = "";
}

function renderMemory(st) {
  els.memory.innerHTML = "";
  const syms = (lastCompile && lastCompile.symbols) || [];
  const cells = st.cells || [];
  // `cells` is the activation-record view starting at base (== index 0 here).
  // symbol.offset indexes into it directly.

  if (!syms.length) {
    els.memory.innerHTML = '<span class="muted">No variables declared.</span>';
    return;
  }

  // Show every symbol's region. Multiple symbols may share an offset across
  // disjoint scopes (shadowing); we show each declared symbol once.
  syms.forEach((s) => {
    const card = document.createElement("div");
    card.className = "var";

    const head = document.createElement("div");
    head.className = "var-head";
    head.innerHTML =
      '<span class="var-name"></span>' +
      '<span class="var-type ' + s.type + '">' + s.type + "</span>" +
      '<span class="var-scope">@' + s.offset +
        (s.scopeDepth ? "  depth " + s.scopeDepth : "") + "</span>";
    head.querySelector(".var-name").textContent = s.name;
    card.appendChild(head);

    if (s.isSet) {
      card.appendChild(renderSetRegion(cells, s.offset));
    } else {
      card.appendChild(renderScalar(cells, s.offset, s.type));
    }
    els.memory.appendChild(card);
  });
}

function renderScalar(cells, offset, type) {
  const wrap = document.createElement("div");
  const have = offset < cells.length;
  const val = have ? cells[offset] : 0;
  const div = document.createElement("div");
  div.className = "scalar-cell";
  let shown = String(val);
  if (type === "bool") {
    div.classList.add(val ? "true" : "false");
    shown = (val ? "true" : "false") + " (" + val + ")";
  }
  div.innerHTML = '<span class="val"></span><span class="addr">cell[' + offset + "]</span>";
  div.querySelector(".val").textContent = have ? shown : "—";
  wrap.appendChild(div);
  return wrap;
}

function renderSetRegion(cells, offset) {
  const wrap = document.createElement("div");
  wrap.className = "set-region";

  const haveCount = offset < cells.length;
  let count = haveCount ? cells[offset] : 0;
  if (count < 0) count = 0;
  if (count > SET_CELLS - 1) count = SET_CELLS - 1;

  const info = document.createElement("div");
  info.className = "set-count";
  info.innerHTML = "count cell[" + offset + "] = <b>" + (haveCount ? cells[offset] : "—") +
    "</b>  &middot; elements cell[" + (offset + 1) + ".." + (offset + SET_CELLS - 1) + "]";
  wrap.appendChild(info);

  const chips = document.createElement("div");
  chips.className = "chips";
  if (!haveCount || count === 0) {
    const c = document.createElement("span");
    c.className = "chip empty";
    c.textContent = "∅ (empty)";
    chips.appendChild(c);
  } else {
    const vals = [];
    for (let k = 0; k < count; k++) {
      const idx = offset + 1 + k;
      if (idx < cells.length) vals.push(cells[idx]);
    }
    vals.forEach((v) => {
      const c = document.createElement("span");
      c.className = "chip";
      c.textContent = v;
      chips.appendChild(c);
    });
  }
  wrap.appendChild(chips);

  const lit = document.createElement("div");
  lit.className = "set-literal";
  if (haveCount && count > 0) {
    const vals = [];
    for (let k = 0; k < count; k++) {
      const idx = offset + 1 + k;
      if (idx < cells.length) vals.push(cells[idx]);
    }
    lit.textContent = "{" + vals.join(",") + "}";
  } else {
    lit.textContent = "{}";
  }
  wrap.appendChild(lit);

  return wrap;
}

/* ------------------------------------------------------------------ */
/* Operand stack                                                       */
/* ------------------------------------------------------------------ */
function renderStack(st) {
  els.stack.innerHTML = "";
  const stack = st.stack || [];
  const top = st.stackTop;
  if (top < 0 || stack.length === 0) {
    els.stack.innerHTML = '<span class="muted">empty</span>';
    return;
  }
  // column-reverse layout: render bottom..top; CSS flips so top sits on top.
  for (let i = 0; i <= top && i < stack.length; i++) {
    const cell = document.createElement("div");
    cell.className = "stack-cell" + (i === top ? " top" : "");
    cell.innerHTML =
      '<span class="si">[' + i + "]" + (i === top ? " ◀ top" : "") + "</span>" +
      '<span class="sv">' + stack[i] + "</span>";
    els.stack.appendChild(cell);
  }
}

/* ------------------------------------------------------------------ */
/* Output console                                                      */
/* ------------------------------------------------------------------ */
function appendOutput(text) {
  els.output.textContent += text;
  els.output.scrollTop = els.output.scrollHeight;
}

/* ------------------------------------------------------------------ */
/* Stepping / running                                                  */
/* ------------------------------------------------------------------ */
function doStep() {
  if (!api || !compiled) return;
  const st = parseState(api.step());
  refreshFromState(st);
  if (st && st.waitingForInput) {
    promptForInput(doStep);
  }
}

function doRun() {
  if (!api || !compiled) return;

  // Fast path: no breakpoints -> let the C VM run to completion in one call.
  if (breakpoints.size === 0) {
    const st = parseState(api.run());
    refreshFromState(st);
    if (st && st.waitingForInput) promptForInput(doRun);
    return;
  }

  // Breakpoint-aware path: drive api.step() in JS, stopping at the next
  // instruction that carries a breakpoint. We execute at LEAST one step so a
  // run resumes off a breakpoint we're currently parked on.
  const CAP = 5000000;
  let st = null;
  let iters = 0;
  for (;;) {
    if (iters++ >= CAP) {
      // surface a runtime-error-style message; reflect last known state
      if (st) { st.output = null; refreshFromState(st); }
      els.runState.textContent = "runtime error: step cap exceeded (" + CAP + ")";
      els.runState.style.color = "var(--err)";
      els.btnStep.disabled = true;
      els.btnRun.disabled = true;
      return;
    }

    st = parseState(api.step());
    if (!st) return;

    // accumulate output incrementally; do NOT full-refresh every step.
    if (st.output) appendOutput(st.output);

    if (st.error || st.halted) {
      st.output = null;          // already appended above
      refreshFromState(st);
      return;
    }
    if (st.waitingForInput) {
      st.output = null;
      refreshFromState(st);      // shows "waiting for input…" + current view
      promptForInput(doRun);     // resume THIS same breakpoint loop
      return;
    }
    // stop if the NEXT instruction to execute carries a breakpoint
    if (breakpoints.has(st.pc)) {
      st.output = null;          // already appended above; avoid double-append
      refreshFromState(st);
      els.runState.textContent = "paused at breakpoint @" + st.pc;
      els.runState.style.color = "var(--pc)";
      return;
    }
  }
}

function doReset() {
  if (!api || !compiled) return;
  hideInputModal();
  api.clearInput();
  waitingForInput = false;
  pendingResume = null;
  els.output.textContent = "";
  const st = parseState(api.reset());
  refreshFromState(st);
}

/* ------------------------------------------------------------------ */
/* Input modal (for read / SREAD starvation)                           */
/* ------------------------------------------------------------------ */
function promptForInput(resumeFn) {
  pendingResume = resumeFn;
  els.inputLine.value = "";
  els.inputOverlay.classList.remove("hidden");
  setTimeout(() => els.inputLine.focus(), 0);
}
function hideInputModal() {
  els.inputOverlay.classList.add("hidden");
}
function feedAndResume() {
  if (!api) return;
  const line = els.inputLine.value;
  api.feedInput(line);
  hideInputModal();
  const resume = pendingResume;
  pendingResume = null;
  waitingForInput = false;
  if (resume) resume();
}

/* ------------------------------------------------------------------ */
/* Examples dropdown                                                   */
/* ------------------------------------------------------------------ */
function populateExamples() {
  Object.keys(EXAMPLES).forEach((key) => {
    const opt = document.createElement("option");
    opt.value = key;
    opt.textContent = EXAMPLES[key].label;
    els.exampleSelect.appendChild(opt);
  });
  els.exampleSelect.value = "example1";
  els.source.value = EXAMPLES.example1.src;
}

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */
function wire() {
  els.btnCompile.addEventListener("click", doCompile);
  els.btnStep.addEventListener("click", doStep);
  els.btnRun.addEventListener("click", doRun);
  els.btnReset.addEventListener("click", doReset);
  els.btnClearOutput.addEventListener("click", () => { els.output.textContent = ""; });
  els.btnClearBp.addEventListener("click", clearBreakpoints);

  // editor: re-highlight on every keystroke (cheap), and schedule a debounced
  // diagnostics-only check (does NOT rebuild P-Code or reset the VM, so a
  // running program is not disturbed by typing). Keep the colored layer
  // scroll-locked to the textarea.
  els.source.addEventListener("input", () => {
    renderHighlight();
    scheduleLiveCheck();
  });
  els.source.addEventListener("scroll", syncHighlightScroll);

  els.exampleSelect.addEventListener("change", (e) => {
    const ex = EXAMPLES[e.target.value];
    if (ex) {
      els.source.value = ex.src;
      // reset downstream view; require a fresh compile
      compiled = false;
      els.btnReset.disabled = true;
      els.btnStep.disabled = true;
      els.btnRun.disabled = true;
      els.diagnostics.innerHTML = '<span class="muted">Compile to see diagnostics.</span>';
      clearBreakpoints();
      renderPcode(null);
      clearMemoryView();
      els.output.textContent = "";
      els.runState.textContent = "";
      errorLines = new Set();
      renderHighlight();           // re-color the freshly loaded example
      scheduleLiveCheck();         // live diagnostics for the new source too
    }
  });

  els.inputFeed.addEventListener("click", feedAndResume);
  els.inputCancel.addEventListener("click", () => {
    hideInputModal();
    pendingResume = null;
  });
  els.inputLine.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); feedAndResume(); }
    if (e.key === "Escape") { hideInputModal(); pendingResume = null; }
  });

  // Ctrl/Cmd+Enter compiles; F10 steps; F5 runs (when not in textarea-only focus)
  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
      e.preventDefault(); doCompile();
    }
  });
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
els.btnCompile.disabled = true;
populateExamples();
renderHighlight();   // color the initially loaded example
wire();
boot();
