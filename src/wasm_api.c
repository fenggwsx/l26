/* wasm_api.c - Emscripten/wasm shell for the L26 compiler.
 *
 * This is a SHELL alongside main.c (the native CLI shell). main.c stays the
 * native entry point; this file is compiled ONLY into the wasm build (see the
 * Makefile `wasm` target, which links every core .c EXCEPT main.c plus this).
 *
 * It drives the EXISTING pure-C core (lexer -> parser -> semantic -> codegen ->
 * vm) and exposes a tiny, string/JSON-based C API to JavaScript so the browser
 * visualizer can:
 *   1. compile a source string and read back diagnostics + P-Code + symbols,
 *   2. single-step the VM and read back the full machine state,
 *   3. feed input lines (for `read` / SREAD) without blocking,
 *   4. reset stepping, or run to completion.
 *
 * NO compiler logic is reimplemented here. Everything routes through the public
 * headers. The only core-internal knowledge used is the documented 201-cell set
 * layout (common.h: L26_SET_CELLS) so cells can be rendered, and that is a
 * public compile-time constant, not a private symbol.
 *
 * The core headers and .c files are UNCHANGED by this shell.
 *
 * Pure C99 plus emscripten.h. A small append-only JSON string builder lives
 * here; no external JSON library is used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "common.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "codegen.h"
#include "vm.h"
#include "ast.h"
#include "symtab.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
/* Allow this file to compile natively (for a quick host syntax check) by
 * making KEEPALIVE a no-op when not building under Emscripten. */
#define EMSCRIPTEN_KEEPALIVE
#endif

/* ================================================================== */
/* Tiny growable string buffer + JSON helpers (no external lib)        */
/* ================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

/* strdup is POSIX, not C99; provide a tiny portable copy. */
static char *dup_str(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void buf_init(Buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void buf_reserve(Buf *b, size_t extra) {
    if (!b->data) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (b->len + extra + 1 > nc) nc *= 2;
    char *nd = (char *)realloc(b->data, nc);
    if (!nd) return; /* on OOM we silently truncate; web demo, not flight sw */
    b->data = nd;
    b->cap = nc;
}

static void buf_puts(Buf *b, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    buf_reserve(b, n);
    if (!b->data) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    if (!b->data) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void buf_putlong(Buf *b, long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    buf_puts(b, tmp);
}

/* Append a JSON-escaped string literal (with surrounding quotes). */
static void buf_putjson_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            unsigned char c = *p;
            switch (c) {
                case '"':  buf_puts(b, "\\\""); break;
                case '\\': buf_puts(b, "\\\\"); break;
                case '\n': buf_puts(b, "\\n");  break;
                case '\r': buf_puts(b, "\\r");  break;
                case '\t': buf_puts(b, "\\t");  break;
                default:
                    if (c < 0x20) {
                        char u[8];
                        snprintf(u, sizeof(u), "\\u%04x", c);
                        buf_puts(b, u);
                    } else {
                        buf_putc(b, (char)c);
                    }
            }
        }
    }
    buf_putc(b, '"');
}

/* Append `"key":` */
static void buf_key(Buf *b, const char *key) {
    buf_putjson_str(b, key);
    buf_putc(b, ':');
}

/* ================================================================== */
/* Captured I/O for the stepping VM                                    */
/* ================================================================== */

/* Output produced by the VM is captured here instead of going to stdout.
 * `out_consumed` marks how much of it JS has already been shown, so step()
 * can report ONLY newly produced output. */
static Buf  g_out;
static size_t g_out_consumed = 0;

/* Pending input fed from JS. We keep a flat char buffer with a parse cursor.
 * read_int parses the next integer token; for SREAD's "read until stop"
 * protocol we treat a newline as the boundary that stops a single set read. */
static char  *g_in = NULL;
static size_t g_in_len = 0;
static size_t g_in_pos = 0;

/* Set to 1 by an io hook when a read could not be satisfied (no input
 * available). The visualizer uses this to know it must prompt for input and
 * re-step. When this happens we DO NOT advance the VM (see step wrapper). */
static int g_waiting_for_input = 0;

static void in_free(void) {
    free(g_in);
    g_in = NULL;
    g_in_len = 0;
    g_in_pos = 0;
}

/* read_int hook: parse the next integer from the pending input buffer.
 * Sets *ok=1 and returns the value on success; sets *ok=0 (and the global
 * g_waiting_for_input) when there is no further integer available. */
static long io_read_int(void *ud, int *ok) {
    (void)ud;

    /* Skip leading separators. A newline marks the end of the current input
     * line: for SREAD this stops the per-line set read. We remember that we
     * stopped ON a line boundary (as opposed to a truly empty buffer) so the
     * step wrapper can tell a completed SREAD from genuine input starvation. */
    while (g_in_pos < g_in_len) {
        char c = g_in[g_in_pos];
        if (c == '\n') {
            g_in_pos++;
            *ok = 0;
            return 0;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == ',') { g_in_pos++; continue; }
        break;
    }

    if (g_in_pos >= g_in_len) {
        /* No more input buffered: ask JS for more. */
        g_waiting_for_input = 1;
        *ok = 0;
        return 0;
    }

    /* Parse an optionally-signed integer. */
    int neg = 0;
    if (g_in[g_in_pos] == '+' || g_in[g_in_pos] == '-') {
        neg = (g_in[g_in_pos] == '-');
        g_in_pos++;
    }
    if (g_in_pos >= g_in_len || !isdigit((unsigned char)g_in[g_in_pos])) {
        /* Malformed token; treat as no input rather than crashing. */
        g_waiting_for_input = 1;
        *ok = 0;
        return 0;
    }
    long v = 0;
    while (g_in_pos < g_in_len && isdigit((unsigned char)g_in[g_in_pos])) {
        v = v * 10 + (g_in[g_in_pos] - '0');
        g_in_pos++;
    }
    if (neg) v = -v;
    *ok = 1;
    return v;
}

static void io_write_str(void *ud, const char *s) {
    (void)ud;
    if (!g_out.data) buf_init(&g_out);
    buf_puts(&g_out, s);
}

static VmIO captured_io(void) {
    VmIO io;
    io.read_int  = io_read_int;
    io.write_str = io_write_str;
    io.ud        = NULL;
    return io;
}

/* ================================================================== */
/* Persistent compiler / VM session state                             */
/* ================================================================== */

static TokenStream g_ts;
static Node       *g_root = NULL;
static SymTab      g_st;
static Program     g_prog;
static int         g_have_program = 0;  /* 1 once codegen succeeded */

static VmState     g_vm;
static int         g_vm_inited = 0;

static char       *g_src = NULL;        /* owned copy of last source */

/* Returned-JSON buffers. We keep ONE static buffer per public function and
 * return a pointer into it; JS must copy/read it before the next call to the
 * same function. This avoids JS-side free() bookkeeping. */
static Buf g_compile_json;
static Buf g_step_json;

static void reset_session(void) {
    if (g_root) { ast_free(g_root); g_root = NULL; }
    g_have_program = 0;
    g_vm_inited = 0;
    g_out.len = 0;
    if (g_out.data) g_out.data[0] = '\0';
    g_out_consumed = 0;
    g_waiting_for_input = 0;
}

/* ================================================================== */
/* JSON emitters                                                       */
/* ================================================================== */

static void emit_diagnostics(Buf *b, const DiagList *dl) {
    buf_key(b, "diagnostics");
    buf_putc(b, '[');
    for (int i = 0; i < dl->count; ++i) {
        const Diagnostic *d = &dl->items[i];
        if (i) buf_putc(b, ',');
        buf_putc(b, '{');
        buf_key(b, "severity"); buf_putjson_str(b, diag_severity_name(d->severity)); buf_putc(b, ',');
        buf_key(b, "phase");    buf_putjson_str(b, diag_phase_name(d->phase));        buf_putc(b, ',');
        buf_key(b, "line");     buf_putlong(b, d->pos.line);  buf_putc(b, ',');
        buf_key(b, "col");      buf_putlong(b, d->pos.col);   buf_putc(b, ',');
        buf_key(b, "message");  buf_putjson_str(b, d->message);
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

static void emit_pcode(Buf *b, const Program *p) {
    buf_key(b, "pcode");
    buf_putc(b, '[');
    for (int i = 0; i < p->count; ++i) {
        char text[96];
        instr_to_string(p->code[i], text, sizeof(text));
        if (i) buf_putc(b, ',');
        buf_putc(b, '{');
        buf_key(b, "idx"); buf_putlong(b, i); buf_putc(b, ',');
        buf_key(b, "op");  buf_putjson_str(b, op_mnemonic(p->code[i].op)); buf_putc(b, ',');
        buf_key(b, "l");   buf_putlong(b, p->code[i].l); buf_putc(b, ',');
        buf_key(b, "a");   buf_putlong(b, p->code[i].a); buf_putc(b, ',');
        buf_key(b, "text"); buf_putjson_str(b, text);
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

static void emit_symbols(Buf *b, const SymTab *st) {
    buf_key(b, "symbols");
    buf_putc(b, '[');
    int first = 1;
    for (int i = 0; i < st->count; ++i) {
        const Symbol *s = symtab_get(st, i);
        if (!s) continue;
        if (!first) buf_putc(b, ',');
        first = 0;
        buf_putc(b, '{');
        buf_key(b, "name");       buf_putjson_str(b, s->name); buf_putc(b, ',');
        buf_key(b, "type");       buf_putjson_str(b, value_type_name(s->type)); buf_putc(b, ',');
        buf_key(b, "offset");     buf_putlong(b, s->offset); buf_putc(b, ',');
        buf_key(b, "size");       buf_putlong(b, s->size);   buf_putc(b, ',');
        buf_key(b, "scopeDepth"); buf_putlong(b, s->scope_depth); buf_putc(b, ',');
        buf_key(b, "isSet");      buf_puts(b, s->type == TYPE_SET ? "true" : "false");
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

/* Compile a source string. Runs lexer -> parser -> semantic -> codegen and
 * returns a JSON object (see the contract below). On success the program and
 * symbol table are retained so step()/run() can drive the VM. The VM is also
 * (re)initialised so the caller can step immediately.
 *
 * JSON shape:
 * {
 *   "ok": bool,                      // true iff no DIAG_ERROR
 *   "diagnostics": [ { "severity","phase","line","col","message" } ],
 *   "pcode": [ { "idx","op","l","a","text" } ],
 *   "frameSize": int,                // operand A of the leading INT 0 A
 *   "symbols": [ { "name","type","offset","size","scopeDepth","isSet" } ]
 * }
 *
 * The returned pointer is valid until the NEXT call to l26_compile.
 */
EMSCRIPTEN_KEEPALIVE
const char *l26_compile(const char *source) {
    reset_session();

    /* keep our own copy of the source so positions stay valid */
    free(g_src);
    g_src = source ? dup_str(source) : dup_str("");

    DiagList dl;
    diag_init(&dl);

    lexer_scan(g_src ? g_src : "", &g_ts, &dl);

    g_root = parse(&g_ts, &dl);

    int sem_ok = 0;
    int frame_size = 0;
    symtab_init(&g_st);
    if (g_root && !diag_has_errors(&dl)) {
        semantic_check(g_root, &g_st, &dl);
        frame_size = symtab_frame_size(&g_st);
        if (!diag_has_errors(&dl)) {
            sem_ok = 1;
        }
    }

    if (sem_ok) {
        memset(&g_prog, 0, sizeof(g_prog));
        codegen_run(g_root, &g_st, &g_prog, &dl);
        if (!diag_has_errors(&dl)) {
            g_have_program = 1;
            vm_init(&g_vm, &g_prog);
            g_vm_inited = 1;
            g_out.len = 0;
            if (g_out.data) g_out.data[0] = '\0';
            g_out_consumed = 0;
            g_waiting_for_input = 0;
        }
    }

    /* Build the JSON result. */
    Buf *b = &g_compile_json;
    if (!b->data) buf_init(b);
    b->len = 0;
    if (b->data) b->data[0] = '\0';

    buf_putc(b, '{');
    buf_key(b, "ok"); buf_puts(b, diag_has_errors(&dl) ? "false" : "true"); buf_putc(b, ',');
    emit_diagnostics(b, &dl); buf_putc(b, ',');

    /* P-Code and symbols only meaningful when we got far enough. */
    if (g_have_program) {
        emit_pcode(b, &g_prog);
    } else {
        buf_key(b, "pcode"); buf_putc(b, '['); buf_putc(b, ']');
    }
    buf_putc(b, ',');

    buf_key(b, "frameSize"); buf_putlong(b, frame_size); buf_putc(b, ',');

    /* Symbols come from the symbol table after semantic; emit whatever exists
     * (even on codegen failure they help diagnose). */
    emit_symbols(b, &g_st);

    buf_putc(b, '}');
    return b->data ? b->data : "{\"ok\":false,\"diagnostics\":[],\"pcode\":[],\"frameSize\":0,\"symbols\":[]}";
}

/* Append the current VM machine-state object to `b`.
 * {
 *   "pc": int, "stackTop": int, "halted": bool, "error": bool,
 *   "waitingForInput": bool,
 *   "stack": [ ... cells 0..stackTop ... ],   // raw operand stack + frame
 *   "cells": [ ... cells 0..frameTop ... ],   // activation-record view
 *   "output": "newly produced output since last state report"
 * }
 *
 * `stack` and `cells` overlap (the single activation record lives at the base
 * of the stack); both are exposed so the frontend can render the operand stack
 * AND group a set variable's 201-cell region by base offset. */
static void emit_vm_state(Buf *b) {
    buf_putc(b, '{');
    buf_key(b, "pc");       buf_putlong(b, g_vm.pc);        buf_putc(b, ',');
    buf_key(b, "stackTop"); buf_putlong(b, g_vm.t);         buf_putc(b, ',');
    buf_key(b, "base");     buf_putlong(b, g_vm.b);         buf_putc(b, ',');
    buf_key(b, "halted");   buf_puts(b, g_vm.halted ? "true" : "false"); buf_putc(b, ',');
    buf_key(b, "error");    buf_puts(b, g_vm.error ? "true" : "false");   buf_putc(b, ',');
    buf_key(b, "waitingForInput");
        buf_puts(b, g_waiting_for_input ? "true" : "false"); buf_putc(b, ',');

    /* Whole live stack region: indices 0..t. */
    buf_key(b, "stack");
    buf_putc(b, '[');
    for (int i = 0; i <= g_vm.t && i < L26_VM_STACK; ++i) {
        if (i) buf_putc(b, ',');
        buf_putlong(b, g_vm.stack[i]);
    }
    buf_putc(b, ']');
    buf_putc(b, ',');

    /* Activation-record cells: same backing array, exposed separately so the
     * frontend can group set regions (count + 200 elements) by base offset.
     * We expose the frame region [base .. t] (which contains the declared
     * variables after the leading INT executes). */
    buf_key(b, "cells");
    buf_putc(b, '[');
    {
        int top = g_vm.t;
        if (top >= L26_VM_STACK) top = L26_VM_STACK - 1;
        for (int i = g_vm.b; i <= top; ++i) {
            if (i > g_vm.b) buf_putc(b, ',');
            buf_putlong(b, g_vm.stack[i]);
        }
    }
    buf_putc(b, ']');
    buf_putc(b, ',');

    /* Newly produced output since the previous state report. */
    buf_key(b, "output");
    {
        const char *fresh = "";
        if (g_out.data && g_out_consumed <= g_out.len)
            fresh = g_out.data + g_out_consumed;
        buf_putjson_str(b, fresh);
        g_out_consumed = g_out.len;
    }

    buf_putc(b, '}');
}

/* True if the instruction at code index `pc` reads input (OPR_READ or SREAD). */
static int instr_is_read(int pc) {
    if (pc < 0 || pc >= g_prog.count) return 0;
    Instruction ins = g_prog.code[pc];
    if (ins.op == OP_SREAD) return 1;
    if (ins.op == OP_OPR && ins.a == OPR_READ) return 1;
    return 0;
}

/* Execute exactly one instruction with input-starvation handling.
 * Returns the vm_step return code. On input starvation it rolls the VM back to
 * `saved_pc`, clears the spurious error flag, and leaves g_waiting_for_input
 * set so the caller surfaces waitingForInput=true. */
static int step_with_input_guard(DiagList *dl, VmIO *io) {
    g_waiting_for_input = 0;
    int saved_pc = g_vm.pc;
    int was_read = instr_is_read(saved_pc);
    int r = vm_step(&g_vm, &g_prog, io, dl);

    if (was_read) {
        /* OPR_READ faults (vm->error) when it cannot obtain a single int;
         * SREAD never faults but yields an empty set if no input was buffered.
         * In BOTH cases, if the read was not satisfied from real input we want
         * the visualizer to prompt for input and re-step rather than treat it
         * as a terminal error. We detect "not satisfied" as: the read hook
         * reported buffer exhaustion (g_waiting_for_input) OR (for OPR_READ)
         * it stopped on an empty line, OR vm error was raised by the read. */
        int starved = g_waiting_for_input ||
                      (g_vm.error && g_prog.code[saved_pc].op == OP_OPR);
        if (starved) {
            g_vm.error = 0;          /* not a real error: just need input   */
            g_vm.pc = saved_pc;      /* re-run the same read after feeding   */
            g_waiting_for_input = 1; /* tell the frontend to prompt          */
            return 0;
        }
    }
    return r;
}

/* Execute exactly ONE instruction and return the resulting machine state JSON.
 * If the next instruction needs input that has not been fed yet, the VM is NOT
 * advanced; instead the returned state has waitingForInput=true and pc is left
 * pointing at the read instruction so the caller can feed input and step again.
 *
 * Returned pointer valid until the next call to l26_step / l26_run / l26_reset.
 */
EMSCRIPTEN_KEEPALIVE
const char *l26_step(void) {
    Buf *b = &g_step_json;
    if (!b->data) buf_init(b);
    b->len = 0;
    if (b->data) b->data[0] = '\0';

    if (!g_have_program || !g_vm_inited) {
        buf_puts(b, "{\"pc\":0,\"stackTop\":-1,\"base\":0,\"halted\":true,"
                    "\"error\":true,\"waitingForInput\":false,\"stack\":[],"
                    "\"cells\":[],\"output\":\"\"}");
        return b->data;
    }

    DiagList dl;
    diag_init(&dl);
    VmIO io = captured_io();

    int r = step_with_input_guard(&dl, &io);
    (void)r; /* state already reflects halted/error/waiting flags */
    emit_vm_state(b);
    return b->data;
}

/* Re-initialise the VM to pc=0 over the last compiled program (does not
 * recompile). Clears captured output and the waiting flag but KEEPS any
 * buffered input. Returns the fresh machine state JSON. */
EMSCRIPTEN_KEEPALIVE
const char *l26_reset(void) {
    Buf *b = &g_step_json;
    if (!b->data) buf_init(b);
    b->len = 0;
    if (b->data) b->data[0] = '\0';

    if (!g_have_program) {
        buf_puts(b, "{\"pc\":0,\"stackTop\":-1,\"base\":0,\"halted\":true,"
                    "\"error\":true,\"waitingForInput\":false,\"stack\":[],"
                    "\"cells\":[],\"output\":\"\"}");
        return b->data;
    }
    vm_init(&g_vm, &g_prog);
    g_vm_inited = 1;
    g_out.len = 0;
    if (g_out.data) g_out.data[0] = '\0';
    g_out_consumed = 0;
    g_waiting_for_input = 0;

    emit_vm_state(b);
    return b->data;
}

/* Run the last compiled program to completion (or until it blocks on input
 * that is not buffered, or until a safety step cap is hit). Returns the final
 * machine state JSON. Convenience wrapper over repeated stepping that does NOT
 * roll back on input starvation - it simply stops with waitingForInput=true. */
EMSCRIPTEN_KEEPALIVE
const char *l26_run(void) {
    Buf *b = &g_step_json;
    if (!b->data) buf_init(b);
    b->len = 0;
    if (b->data) b->data[0] = '\0';

    if (!g_have_program || !g_vm_inited) {
        buf_puts(b, "{\"pc\":0,\"stackTop\":-1,\"base\":0,\"halted\":true,"
                    "\"error\":true,\"waitingForInput\":false,\"stack\":[],"
                    "\"cells\":[],\"output\":\"\"}");
        return b->data;
    }

    DiagList dl;
    diag_init(&dl);
    VmIO io = captured_io();

    long guard = 0;
    const long GUARD_MAX = 50000000L; /* generous cap against infinite loops */
    for (;;) {
        if (g_vm.halted || g_vm.error) break;
        int r = step_with_input_guard(&dl, &io);
        if (g_waiting_for_input) break; /* blocked on input; stop and prompt */
        if (r == 1 || r < 0) break;
        if (++guard >= GUARD_MAX) {
            g_vm.error = 1;
            break;
        }
    }
    emit_vm_state(b);
    return b->data;
}

/* Feed one line of input (used by `read` / SREAD). The text is APPENDED to the
 * pending-input buffer with a trailing newline so SREAD's per-line boundary is
 * respected. Returns 1 on success. After feeding input the caller should step()
 * (or run()) again; the previously-blocked read will now succeed. */
EMSCRIPTEN_KEEPALIVE
int l26_feed_input(const char *line) {
    if (!line) line = "";
    size_t add = strlen(line);
    /* +1 for the newline boundary, +1 for NUL */
    char *nb = (char *)realloc(g_in, g_in_len + add + 2);
    if (!nb) return 0;
    g_in = nb;
    memcpy(g_in + g_in_len, line, add);
    g_in_len += add;
    g_in[g_in_len++] = '\n';
    g_in[g_in_len] = '\0';
    g_waiting_for_input = 0;
    return 1;
}

/* Clear any pending (unconsumed) input. Useful when restarting a run. */
EMSCRIPTEN_KEEPALIVE
void l26_clear_input(void) {
    in_free();
    g_waiting_for_input = 0;
}

/* Report the leading frame size (operand A of INT 0 A) of the current program,
 * or -1 if no program is compiled. A convenience for the frontend's memory
 * view sizing. */
EMSCRIPTEN_KEEPALIVE
int l26_frame_size(void) {
    if (!g_have_program || g_prog.count == 0) return -1;
    if (g_prog.code[0].op == OP_INT) return g_prog.code[0].a;
    return -1;
}
