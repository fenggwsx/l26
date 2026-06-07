/* instr.c - instruction mnemonics & disassembly. Fully implemented.
 *
 * Shared by the VM and the future Web visualizer. No execution here, only the
 * mapping between the Instruction struct and human-readable text.
 */
#include "vm.h"

#include <stdio.h>

const char *op_mnemonic(VmOp op) {
    switch (op) {
        case OP_INT:    return "INT";
        case OP_OPR:    return "OPR";
        case OP_CAL:    return "CAL";
        case OP_LIT:    return "LIT";
        case OP_LOD:    return "LOD";
        case OP_STO:    return "STO";
        case OP_JMP:    return "JMP";
        case OP_JPC:    return "JPC";
        case OP_SCLR:   return "SCLR";
        case OP_SADD:   return "SADD";
        case OP_SREM:   return "SREM";
        case OP_SIN:    return "SIN";
        case OP_SEMPTY: return "SEMPTY";
        case OP_SUNION: return "SUNION";
        case OP_SINTER: return "SINTER";
        case OP_SCOPY:  return "SCOPY";
        case OP_SEQ:    return "SEQ";
        case OP_SWRITE: return "SWRITE";
        case OP_SREAD:  return "SREAD";
        default:        return "???";
    }
}

const char *opr_func_name(OprFunc f) {
    switch (f) {
        case OPR_RET:   return "ret";
        case OPR_NEG:   return "neg";
        case OPR_ADD:   return "add";
        case OPR_SUB:   return "sub";
        case OPR_MUL:   return "mul";
        case OPR_DIV:   return "div";
        case OPR_ODD:   return "odd";
        case OPR_EQ:    return "eq";
        case OPR_NE:    return "ne";
        case OPR_LT:    return "lt";
        case OPR_GE:    return "ge";
        case OPR_GT:    return "gt";
        case OPR_LE:    return "le";
        case OPR_WRITE: return "write";
        case OPR_NL:    return "nl";
        case OPR_READ:  return "read";
        default:        return "?";
    }
}

int instr_to_string(Instruction ins, char *buf, size_t bufsz) {
    if (ins.op == OP_OPR) {
        return snprintf(buf, bufsz, "%-6s %d %-2d   ; %s",
                        op_mnemonic(ins.op), ins.l, ins.a,
                        opr_func_name((OprFunc)ins.a));
    }
    return snprintf(buf, bufsz, "%-6s %d %d",
                    op_mnemonic(ins.op), ins.l, ins.a);
}

void program_disassemble(const Program *p, FILE *out) {
    char buf[128];
    for (int i = 0; i < p->count; ++i) {
        instr_to_string(p->code[i], buf, sizeof(buf));
        fprintf(out, "%4d  %s\n", i, buf);
    }
}
