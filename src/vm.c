/* vm.c - P-Code virtual machine.
 *
 * Executes the full base instruction set from AGENTS.md (INT, OPR variants
 * 0..16, CAL, LIT, LOD, STO, JMP, JPC) plus every set opcode from the contract
 * (SCLR/SADD/SREM/SIN/SEMPTY/SUNION/SINTER/SCOPY/SEQ/SWRITE/SREAD).
 *
 * The operand stack and the single activation record share one int array
 * (base b = 0). Sets live inline in 201-cell regions:
 *     cell[base]            = element count (0..L26_MAX_SET)
 *     cell[base+1 .. +200]  = up to 200 element values, sorted ascending,
 *                             deduplicated.
 * Keeping elements sorted & deduped makes membership / add / remove / union /
 * inter / equality linear and deterministic.
 *
 * Runtime errors (stack over/underflow, div by zero, set overflow, bad pc,
 * bad set base) are reported to `dl` and stop execution. vm_step and vm_run
 * share identical semantics so the Web visualizer (single-step) and the CLI
 * (run-to-halt) behave the same.
 */
#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- default stdio I/O hooks ----
 *
 * Input is line-buffered so `read` (one int) and `read` of a set (a whole
 * line of ints) compose without stealing each other's input:
 *   - a scalar read consumes the next whitespace/comma-separated integer,
 *     pulling in a fresh line only when the current one is exhausted;
 *   - a set read (io_read_set_default) consumes the REST of the current line
 *     (refilling first if the current line is already exhausted), so it never
 *     reaches across a newline into input meant for a later read.
 * Keeping both on the same buffered cursor avoids the classic scanf/fgets
 * mixing hazard. */
typedef struct {
    char line[4096];
    int  pos;     /* cursor into line[] */
    int  len;     /* bytes valid in line[] */
    int  eof;     /* set once stdin is exhausted */
    int  primed;  /* whether line[] holds a line not yet fully consumed */
} StdinState;

static StdinState g_stdin = { {0}, 0, 0, 0, 0 };

/* Refill line[] with the next line of stdin. Returns 1 on success, 0 at EOF. */
static int stdin_fill_line(StdinState *s) {
    if (s->eof) return 0;
    if (fgets(s->line, (int)sizeof s->line, stdin) == NULL) {
        s->eof = 1;
        s->primed = 0;
        s->len = 0;
        s->pos = 0;
        return 0;
    }
    s->len = (int)strlen(s->line);
    s->pos = 0;
    s->primed = 1;
    return 1;
}

/* Parse the next integer from the current line (refilling lines as needed).
 * Returns 1 and writes *out on success, 0 at end of input. */
static int stdin_next_int(StdinState *s, long *out) {
    for (;;) {
        if (!s->primed && stdin_fill_line(s) == 0) return 0;
        /* skip separators (whitespace and commas) */
        while (s->pos < s->len) {
            char ch = s->line[s->pos];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                ch == ',') {
                s->pos++;
            } else {
                break;
            }
        }
        if (s->pos >= s->len) { s->primed = 0; continue; } /* line drained */
        {
            char *end = NULL;
            long v = strtol(s->line + s->pos, &end, 10);
            if (end == s->line + s->pos) {
                /* not a number: skip one char and retry */
                s->pos++;
                continue;
            }
            s->pos = (int)(end - s->line);
            *out = v;
            return 1;
        }
    }
}

static long io_read_int_default(void *ud, int *ok) {
    (void)ud;
    long v = 0;
    *ok = stdin_next_int(&g_stdin, &v);
    return v;
}

/* Read the REST of the current input line as a set's worth of ints.
 * Writes up to `max` values into `vals`, returns the count (>=0). */
static int io_read_set_default(void *ud, int *vals, int max) {
    (void)ud;
    StdinState *s = &g_stdin;
    int n = 0;
    /* If the current line is already drained (or none loaded yet), advance to
     * the next line so a set read always consumes exactly one fresh line. */
    if (!s->primed) {
        if (stdin_fill_line(s) == 0) return 0;
    }
    for (;;) {
        long v;
        /* Stop at end of the current line, never crossing into the next. */
        while (s->pos < s->len) {
            char ch = s->line[s->pos];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                ch == ',') {
                s->pos++;
            } else {
                break;
            }
        }
        if (s->pos >= s->len) break;
        {
            char *end = NULL;
            v = strtol(s->line + s->pos, &end, 10);
            if (end == s->line + s->pos) { s->pos++; continue; }
            s->pos = (int)(end - s->line);
        }
        if (n < max) vals[n] = (int)v;
        n++;
    }
    s->primed = 0; /* this line is now fully consumed */
    return n;
}

static void io_write_str_default(void *ud, const char *s) {
    (void)ud;
    fputs(s, stdout);
}
static VmIO io_default(void) {
    VmIO io;
    io.read_int = io_read_int_default;
    io.write_str = io_write_str_default;
    io.ud = NULL;
    return io;
}

void vm_init(VmState *vm, const Program *p) {
    (void)p;
    vm->t = -1;
    vm->pc = 0;
    vm->b = 0;
    vm->halted = 0;
    vm->error = 0;
}

/* ------------------------------------------------------------------ */
/* Error helper                                                        */
/* ------------------------------------------------------------------ */
static int vm_fail(VmState *vm, DiagList *dl, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_add(dl, DIAG_ERROR, DIAG_PHASE_VM, srcpos_make(0, 0, 0), "%s", buf);
    vm->error = 1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Stack guards                                                        */
/* ------------------------------------------------------------------ */
/* Ensure at least `n` operands are present on the stack. */
static int need(VmState *vm, DiagList *dl, int n) {
    if (vm->t + 1 < n) {
        return vm_fail(vm, dl, "stack underflow");
    }
    return 0;
}
/* Ensure there is room to push one more value. */
static int room(VmState *vm, DiagList *dl) {
    if (vm->t + 1 >= L26_VM_STACK) {
        return vm_fail(vm, dl, "stack overflow");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Set helpers (operate on inline 201-cell regions)                    */
/* ------------------------------------------------------------------ */
/* Validate that a set region of L26_SET_CELLS cells fits in the frame at
 * base offset `base`. */
static int set_base_ok(VmState *vm, DiagList *dl, int base) {
    if (base < 0 || base + L26_SET_CELLS > L26_VM_STACK) {
        return vm_fail(vm, dl, "invalid set base offset: %d", base);
    }
    return 0;
}

/* Find index of value v in the sorted set; returns slot index in
 * cell[base+1..] if found, else -1. Also returns via *ins the slot where v
 * would be inserted to keep order. */
static int set_find(const int *st, int base, int v, int *ins) {
    int count = st[base];
    int lo = 0, hi = count; /* search range over element slots [0,count) */
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int e = st[base + 1 + mid];
        if (e == v) {
            if (ins) *ins = mid;
            return mid;
        } else if (e < v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (ins) *ins = lo;
    return -1;
}

/* Insert v into set@base keeping sorted & deduped. Reports overflow to dl. */
static int set_add(VmState *vm, DiagList *dl, int base, int v) {
    if (set_base_ok(vm, dl, base) < 0) return -1;
    int *st = vm->stack;
    int ins;
    if (set_find(st, base, v, &ins) >= 0) return 0; /* already present */
    int count = st[base];
    if (count >= L26_MAX_SET) {
        return vm_fail(vm, dl, "set overflow: cannot exceed %d elements",
                       L26_MAX_SET);
    }
    /* shift elements [ins, count) up by one to make room at slot `ins` */
    for (int i = count; i > ins; --i) {
        st[base + 1 + i] = st[base + 1 + (i - 1)];
    }
    st[base + 1 + ins] = v;
    st[base] = count + 1;
    return 0;
}

/* Remove v from set@base (no-op if absent). */
static int set_remove(VmState *vm, DiagList *dl, int base, int v) {
    if (set_base_ok(vm, dl, base) < 0) return -1;
    int *st = vm->stack;
    int idx = set_find(st, base, v, NULL);
    if (idx < 0) return 0; /* absent */
    int count = st[base];
    for (int i = idx; i < count - 1; ++i) {
        st[base + 1 + i] = st[base + 1 + (i + 1)];
    }
    st[base] = count - 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* OPR sub-functions                                                   */
/* ------------------------------------------------------------------ */
static int do_opr(VmState *vm, const VmIO *io, DiagList *dl, int f) {
    switch (f) {
        case OPR_RET:
            vm->halted = 1;
            return 1;

        case OPR_NEG:
            if (need(vm, dl, 1) < 0) return -1;
            vm->stack[vm->t] = -vm->stack[vm->t];
            return 0;

        case OPR_ODD:
            if (need(vm, dl, 1) < 0) return -1;
            vm->stack[vm->t] = (vm->stack[vm->t] & 1) ? 1 : 0;
            return 0;

        case OPR_ADD:
        case OPR_SUB:
        case OPR_MUL:
        case OPR_DIV: {
            if (need(vm, dl, 2) < 0) return -1;
            int rhs = vm->stack[vm->t--];
            int lhs = vm->stack[vm->t];
            int res;
            if (f == OPR_ADD)      res = lhs + rhs;
            else if (f == OPR_SUB) res = lhs - rhs;
            else if (f == OPR_MUL) res = lhs * rhs;
            else {
                if (rhs == 0) return vm_fail(vm, dl, "division by zero");
                res = lhs / rhs;
            }
            vm->stack[vm->t] = res;
            return 0;
        }

        case OPR_EQ:
        case OPR_NE:
        case OPR_LT:
        case OPR_GE:
        case OPR_GT:
        case OPR_LE: {
            if (need(vm, dl, 2) < 0) return -1;
            int rhs = vm->stack[vm->t--];
            int lhs = vm->stack[vm->t];
            int res;
            switch (f) {
                case OPR_EQ: res = (lhs == rhs); break;
                case OPR_NE: res = (lhs != rhs); break;
                case OPR_LT: res = (lhs <  rhs); break;
                case OPR_GE: res = (lhs >= rhs); break;
                case OPR_GT: res = (lhs >  rhs); break;
                default:     res = (lhs <= rhs); break; /* OPR_LE */
            }
            vm->stack[vm->t] = res ? 1 : 0;
            return 0;
        }

        case OPR_WRITE: {
            if (need(vm, dl, 1) < 0) return -1;
            char buf[32];
            snprintf(buf, sizeof buf, "%d", vm->stack[vm->t--]);
            io->write_str(io->ud, buf);
            return 0;
        }

        case OPR_NL:
            io->write_str(io->ud, "\n");
            return 0;

        case OPR_READ: {
            if (room(vm, dl) < 0) return -1;
            int ok = 0;
            long v = io->read_int(io->ud, &ok);
            if (!ok) return vm_fail(vm, dl, "failed to read integer input");
            vm->stack[++vm->t] = (int)v;
            return 0;
        }

        default:
            return vm_fail(vm, dl, "unknown OPR sub-function: %d", f);
    }
}

/* ------------------------------------------------------------------ */
/* Set opcodes                                                         */
/* ------------------------------------------------------------------ */
static int do_set_op(VmState *vm, const VmIO *io, DiagList *dl,
                     Instruction ins) {
    int *st = vm->stack;
    switch (ins.op) {
        case OP_SCLR:
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            st[ins.a] = 0;
            return 0;

        case OP_SADD: {
            if (need(vm, dl, 1) < 0) return -1;
            int v = st[vm->t--];
            return set_add(vm, dl, ins.a, v);
        }

        case OP_SREM: {
            if (need(vm, dl, 1) < 0) return -1;
            int v = st[vm->t--];
            return set_remove(vm, dl, ins.a, v);
        }

        case OP_SIN: {
            if (need(vm, dl, 1) < 0) return -1;
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            int v = st[vm->t]; /* pop v, push bool: net no change */
            int found = (set_find(st, ins.a, v, NULL) >= 0);
            st[vm->t] = found ? 1 : 0;
            return 0;
        }

        case OP_SEMPTY: {
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            if (room(vm, dl) < 0) return -1;
            st[++vm->t] = (st[ins.a] == 0) ? 1 : 0;
            return 0;
        }

        case OP_SUNION:
        case OP_SINTER: {
            /* operands on stack: left base (pushed first), right base (top) */
            if (need(vm, dl, 2) < 0) return -1;
            int rbase = st[vm->t--];
            int lbase = st[vm->t--];
            if (set_base_ok(vm, dl, lbase) < 0) return -1;
            if (set_base_ok(vm, dl, rbase) < 0) return -1;
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            /* Compute into a local scratch buffer first so dest may alias an
             * operand without corrupting the merge. Both inputs are sorted. */
            int tmp[L26_MAX_SET];
            int n = 0;
            int lc = st[lbase], rc = st[rbase];
            int i = 0, j = 0;
            if (ins.op == OP_SUNION) {
                while (i < lc && j < rc) {
                    int a = st[lbase + 1 + i];
                    int b = st[rbase + 1 + j];
                    int pick;
                    if (a < b)      { pick = a; ++i; }
                    else if (a > b) { pick = b; ++j; }
                    else            { pick = a; ++i; ++j; }
                    if (n == 0 || tmp[n - 1] != pick) {
                        if (n >= L26_MAX_SET)
                            return vm_fail(vm, dl,
                                "set overflow in union: exceeds %d elements",
                                L26_MAX_SET);
                        tmp[n++] = pick;
                    }
                }
                while (i < lc) {
                    int a = st[lbase + 1 + i++];
                    if (n == 0 || tmp[n - 1] != a) {
                        if (n >= L26_MAX_SET)
                            return vm_fail(vm, dl,
                                "set overflow in union: exceeds %d elements",
                                L26_MAX_SET);
                        tmp[n++] = a;
                    }
                }
                while (j < rc) {
                    int b = st[rbase + 1 + j++];
                    if (n == 0 || tmp[n - 1] != b) {
                        if (n >= L26_MAX_SET)
                            return vm_fail(vm, dl,
                                "set overflow in union: exceeds %d elements",
                                L26_MAX_SET);
                        tmp[n++] = b;
                    }
                }
            } else { /* OP_SINTER */
                while (i < lc && j < rc) {
                    int a = st[lbase + 1 + i];
                    int b = st[rbase + 1 + j];
                    if (a < b)      ++i;
                    else if (a > b) ++j;
                    else            { tmp[n++] = a; ++i; ++j; }
                }
            }
            st[ins.a] = n;
            for (int k = 0; k < n; ++k) st[ins.a + 1 + k] = tmp[k];
            return 0;
        }

        case OP_SCOPY: {
            if (need(vm, dl, 1) < 0) return -1;
            int src = st[vm->t--];
            if (set_base_ok(vm, dl, src) < 0) return -1;
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            if (src != ins.a)
                memmove(&st[ins.a], &st[src],
                        (size_t)L26_SET_CELLS * sizeof(int));
            return 0;
        }

        case OP_SEQ: {
            /* pop left then right base offsets; push 1 if equal else 0 */
            if (need(vm, dl, 2) < 0) return -1;
            int rbase = st[vm->t--];
            int lbase = st[vm->t--];
            if (set_base_ok(vm, dl, lbase) < 0) return -1;
            if (set_base_ok(vm, dl, rbase) < 0) return -1;
            int eq = (st[lbase] == st[rbase]);
            if (eq) {
                int c = st[lbase];
                for (int k = 0; k < c; ++k) {
                    if (st[lbase + 1 + k] != st[rbase + 1 + k]) { eq = 0; break; }
                }
            }
            st[++vm->t] = eq ? 1 : 0; /* popped 2, push 1: room guaranteed */
            return 0;
        }

        case OP_SWRITE: {
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            int count = st[ins.a];
            io->write_str(io->ud, "{");
            for (int k = 0; k < count; ++k) {
                char buf[32];
                if (k) io->write_str(io->ud, ",");
                snprintf(buf, sizeof buf, "%d", st[ins.a + 1 + k]);
                io->write_str(io->ud, buf);
            }
            io->write_str(io->ud, "}");
            return 0;
        }

        case OP_SREAD: {
            /* read one line of space/comma separated ints into set@A */
            if (set_base_ok(vm, dl, ins.a) < 0) return -1;
            st[ins.a] = 0; /* cleared first */
            if (io->read_int == io_read_int_default) {
                /* Default stdio path: consume exactly one input line so a set
                 * read does not steal integers intended for a later read. */
                int vals[L26_MAX_SET];
                int n = io_read_set_default(io->ud, vals, L26_MAX_SET);
                for (int k = 0; k < n; ++k) {
                    if (set_add(vm, dl, ins.a, vals[k]) < 0) return -1;
                }
            } else {
                /* Custom IO exposes only read_int (no line boundary); read
                 * until it stops. */
                int ok = 0;
                long v = io->read_int(io->ud, &ok);
                while (ok) {
                    if (set_add(vm, dl, ins.a, (int)v) < 0) return -1;
                    v = io->read_int(io->ud, &ok);
                }
            }
            return 0;
        }

        default:
            return vm_fail(vm, dl, "unknown set opcode: %d", (int)ins.op);
    }
}

/* ------------------------------------------------------------------ */
/* Single-step dispatcher                                              */
/* ------------------------------------------------------------------ */
int vm_step(VmState *vm, const Program *p, const VmIO *io, DiagList *dl) {
    if (vm->halted) return 1;
    if (vm->error)  return -1;
    if (vm->pc < 0 || vm->pc >= p->count) {
        return vm_fail(vm, dl, "pc out of range: %d", vm->pc);
    }
    Instruction ins = p->code[vm->pc++];
    switch (ins.op) {
        case OP_INT:
            /* reserve `a` cells for the activation record (zeroed) */
            if (ins.a < 0)
                return vm_fail(vm, dl, "INT with negative size %d", ins.a);
            if (vm->t + ins.a >= L26_VM_STACK) {
                return vm_fail(vm, dl, "stack overflow allocating frame");
            }
            for (int i = 0; i < ins.a; ++i) vm->stack[++vm->t] = 0;
            return 0;

        case OP_LIT:
            if (room(vm, dl) < 0) return -1;
            vm->stack[++vm->t] = ins.a;
            return 0;

        case OP_LOD:
            if (ins.a < 0 || ins.a >= L26_VM_STACK)
                return vm_fail(vm, dl, "LOD offset out of range: %d", ins.a);
            if (room(vm, dl) < 0) return -1;
            vm->stack[++vm->t] = vm->stack[vm->b + ins.a];
            return 0;

        case OP_STO:
            if (need(vm, dl, 1) < 0) return -1;
            if (ins.a < 0 || ins.a >= L26_VM_STACK)
                return vm_fail(vm, dl, "STO offset out of range: %d", ins.a);
            vm->stack[vm->b + ins.a] = vm->stack[vm->t--];
            return 0;

        case OP_LODX: {
            /* pop a frame offset, push the cell value at that offset */
            if (need(vm, dl, 1) < 0) return -1;
            int off = vm->stack[vm->t];
            if (off < 0 || vm->b + off >= L26_VM_STACK)
                return vm_fail(vm, dl, "LODX offset out of range: %d", off);
            vm->stack[vm->t] = vm->stack[vm->b + off];
            return 0;
        }

        case OP_JMP:
            if (ins.a < 0 || ins.a > p->count)
                return vm_fail(vm, dl, "JMP target out of range: %d", ins.a);
            vm->pc = ins.a;
            return 0;

        case OP_JPC:
            if (need(vm, dl, 1) < 0) return -1;
            if (ins.a < 0 || ins.a > p->count)
                return vm_fail(vm, dl, "JPC target out of range: %d", ins.a);
            if (vm->stack[vm->t--] == 0) vm->pc = ins.a;
            return 0;

        case OP_CAL:
            /* Present for completeness; single activation record never emits
             * CAL. Treat as a no-op rather than faulting. */
            return 0;

        case OP_OPR:
            return do_opr(vm, io, dl, ins.a);

        case OP_SCLR:
        case OP_SADD:
        case OP_SREM:
        case OP_SIN:
        case OP_SEMPTY:
        case OP_SUNION:
        case OP_SINTER:
        case OP_SCOPY:
        case OP_SEQ:
        case OP_SWRITE:
        case OP_SREAD:
            return do_set_op(vm, io, dl, ins);

        default:
            return vm_fail(vm, dl, "unknown opcode: %d", (int)ins.op);
    }
}

int vm_run(const Program *p, const VmIO *io, DiagList *dl) {
    VmIO local = io ? *io : io_default();
    VmState vm;
    vm_init(&vm, p);
    for (;;) {
        int r = vm_step(&vm, p, &local, dl);
        if (r == 1) return 0;       /* halted cleanly */
        if (r < 0) return 1;        /* runtime error  */
        /* r == 0: continue */
    }
}
