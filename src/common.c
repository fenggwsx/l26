/* common.c - tiny shared helpers. */
#include "common.h"

const char *value_type_name(ValueType t) {
    switch (t) {
        case TYPE_INT:   return "int";
        case TYPE_BOOL:  return "bool";
        case TYPE_SET:   return "set";
        case TYPE_VOID:  return "void";
        case TYPE_ERROR: default: return "<error>";
    }
}
