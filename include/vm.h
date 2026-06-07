/* vm.h - the P-Code-like instruction set and virtual machine.
 *
 * INSTRUCTION ENCODING (locked):
 *   An instruction is { VmOp op; int l; int a; }.
 *   `op` is an enum (not a 3-char string) so codegen/VM share one symbol.
 *   `l`  is the level difference (0 for the single activation record, but
 *        kept in the contract to honour the base CAL/LOD/STO format).
 *   `a`  is the address / immediate / operand.
 *
 * The BASE opcodes (INT/OPR/CAL/LIT/LOD/STO/JMP/JPC) match AGENTS.md exactly.
 * OPR sub-functions are the integer A operand of an OPR instruction, captured
 * here as named OprFunc values so codegen never hardcodes a magic number.
 *
 * NEW SET OPCODES (decision #7: add new opcodes, never overload integer ones)
 * operate on inline 201-cell set regions addressed by a base offset in the
 * activation record. They are documented exactly in DESIGN.md.  Their stack
 * effects are spelled out in comments below.
 *
 * Pure C99.  The VM's only I/O is via a caller-supplied VmIO callback pair,
 * so the core stays embeddable (CLI feeds stdio; wasm feeds JS; tests feed
 * buffers).  vm_run with io==NULL defaults to stdio.
 */
#ifndef L26_VM_H
#define L26_VM_H

#include <stdio.h>
#include "common.h"
#include "diag.h"

/* ------------------------------------------------------------------ */
/* Opcodes                                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    OP_INT = 0,  /* INT 0 A : allocate A cells at top of frame           */
    OP_OPR,      /* OPR 0 F : arithmetic/compare/io, F = OprFunc         */
    OP_CAL,      /* CAL L A : call (present for completeness; single AR) */
    OP_LIT,      /* LIT 0 A : push immediate A                           */
    OP_LOD,      /* LOD L A : push frame cell at offset A                */
    OP_STO,      /* STO L A : pop into frame cell at offset A            */
    OP_JMP,      /* JMP 0 A : jump to code address A                     */
    OP_JPC,      /* JPC 0 A : pop; if 0 jump to A                        */

    /* ---- NEW set opcodes (operate on inline 201-cell set regions) ---- */
    OP_SCLR,     /* SCLR 0 A : set region at offset A to empty (count=0).
                  *            No stack effect.                          */
    OP_SADD,     /* SADD 0 A : pop v; insert v into set at offset A
                  *            (dedup, keep sorted, cap L26_MAX_SET).
                  *            t -= 1.                                   */
    OP_SREM,     /* SREM 0 A : pop v; remove v from set at offset A
                  *            (no-op if absent). t -= 1.                */
    OP_SIN,      /* SIN  0 A : pop v; push 1 if v in set@A else 0.
                  *            t unchanged (pop v, push bool).           */
    OP_SEMPTY,   /* SEMPTY 0 A : push 1 if set@A is empty else 0. t += 1.*/
    OP_SUNION,   /* SUNION 0 A : binary set union. Operands are two set
                  *   base offsets pushed as plain ints (left then right)
                  *   onto the stack; result is written into the set
                  *   region at offset A. Pops both offsets. See DESIGN. */
    OP_SINTER,   /* SINTER 0 A : binary set intersection, same protocol
                  *   as SUNION, result into set@A.                      */
    OP_SCOPY,    /* SCOPY 0 A : pop a source set base offset; copy that
                  *   whole 201-cell region into set@A. t -= 1.          */
    OP_SEQ,      /* SEQ  0 A : pop two set base offsets (left,right);
                  *   push 1 if the sets are equal else 0. t -= 1
                  *   (two offsets popped, one bool pushed). BONUS.      */
    OP_SWRITE,   /* SWRITE 0 A : print set@A as {e1,e2,...}. No stack fx.*/
    OP_SREAD     /* SREAD 0 A : read a line of space/comma separated ints
                  *   into set@A (cleared first). No stack effect.       */
} VmOp;

/* OPR sub-function codes (the A operand of an OPR instruction). Values
 * MATCH AGENTS.md so generated listings line up with the spec table. */
typedef enum {
    OPR_RET   = 0,   /* return from procedure / halt the program        */
    OPR_NEG   = 1,   /* unary minus on top                              */
    OPR_ADD   = 2,   /* st1 + st0 -> st1, t-1                           */
    OPR_SUB   = 3,   /* st1 - st0                                       */
    OPR_MUL   = 4,   /* st1 * st0                                       */
    OPR_DIV   = 5,   /* st1 / st0                                       */
    OPR_ODD   = 6,   /* odd(st0): 1 if odd else 0                       */
    /* 7 reserved (unused in AGENTS.md) */
    OPR_EQ    = 8,   /* st1 == st0                                      */
    OPR_NE    = 9,   /* st1 != st0                                      */
    OPR_LT    = 10,  /* st1 <  st0                                      */
    OPR_GE    = 11,  /* st1 >= st0                                      */
    OPR_GT    = 12,  /* st1 >  st0                                      */
    OPR_LE    = 13,  /* st1 <= st0                                      */
    OPR_WRITE = 14,  /* print st0 (int), t-1                            */
    OPR_NL    = 15,  /* print newline                                   */
    OPR_READ  = 16   /* read int line, push, t+1                        */
} OprFunc;

/* One P-Code instruction. */
typedef struct {
    VmOp op;
    int    l;
    int    a;
} Instruction;

/* A code image. */
typedef struct {
    Instruction code[L26_MAX_CODE];
    int         count;
} Program;

/* ------------------------------------------------------------------ */
/* Encoding / decoding / disassembly helpers (shared by VM + viz)      */
/* ------------------------------------------------------------------ */

/* The 3-letter mnemonic for an opcode, e.g. "INT","OPR","SADD". Never NULL. */
const char *op_mnemonic(VmOp op);

/* Name of an OPR sub-function, e.g. "ADD","WRITE". Never NULL. */
const char *opr_func_name(OprFunc f);

/* Disassemble one instruction into `buf`, e.g. "OPR 0 14   ; write".
 * Returns bytes that would be written (snprintf semantics). No I/O. */
int instr_to_string(Instruction ins, char *buf, size_t bufsz);

/* Disassemble a whole program into `out` (the only stream use here). */
void program_disassemble(const Program *p, FILE *out);

/* ------------------------------------------------------------------ */
/* VM execution                                                        */
/* ------------------------------------------------------------------ */

/* I/O hooks so the VM core has no hardwired stdio dependency.
 * read_int  : return an integer read from input; set *ok=0 on EOF/error.
 * write_str : emit a string (no newline added).
 * `ud` is opaque user data passed back to both. */
typedef struct {
    long (*read_int)(void *ud, int *ok);
    void (*write_str)(void *ud, const char *s);
    void  *ud;
} VmIO;

/* Run program `p`. `io` may be NULL to use built-in stdio. Runtime errors
 * (stack overflow, div by zero, set overflow) are reported to `dl` and stop
 * execution. Returns 0 on clean halt (OPR_RET), non-zero on runtime error. */
int vm_run(const Program *p, const VmIO *io, DiagList *dl);

/* ---- Single-step interface (for the Web visualizer / debugger) ---- */
/* Stack/frame sizing for the embedded VM state. */
#define L26_VM_STACK 8192

typedef struct {
    int  stack[L26_VM_STACK]; /* operand stack AND activation record    */
    int  t;                   /* top-of-stack index (-1 when empty)     */
    int  pc;                  /* program counter                        */
    int  b;                   /* base of activation record (0 here)     */
    int  halted;              /* set when OPR_RET executes              */
    int  error;               /* set on runtime error                   */
} VmState;

/* Initialise VM state for program `p` (does not execute). */
void vm_init(VmState *vm, const Program *p);

/* Execute exactly one instruction at vm->pc. Returns 0 to continue, 1 when
 * halted, <0 on runtime error (diagnostic emitted to dl). Lets the
 * visualizer drive the VM one P-Code at a time. */
int vm_step(VmState *vm, const Program *p, const VmIO *io, DiagList *dl);

#endif /* L26_VM_H */
