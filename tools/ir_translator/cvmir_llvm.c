#include "cvmir.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Target.h>

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CVMIR_NAME_MAX 192

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

typedef struct {
    LLVMValueRef value;
    int64_t offset;
} ValueSlot;

typedef struct {
    LLVMValueRef alloca_value;
    int64_t offset;
} StackObject;

typedef struct {
    LLVMBasicBlockRef block;
    unsigned label;
} BlockLabel;

typedef struct {
    LLVMValueRef function;
    ValueSlot *slots;
    size_t slot_count;
    StackObject *objects;
    size_t object_count;
    BlockLabel *blocks;
    size_t block_count;
    uint64_t frame_size;
} FunctionLayout;

typedef struct {
    TextBuffer output;
    LLVMTargetDataRef target_data;
    CvmIrError *error;
    unsigned next_label;
    int failed;
} Generator;

typedef enum {
    CVM_INTRINSIC_VOID0,
    CVM_INTRINSIC_VALUE0,
    CVM_INTRINSIC_VOID1,
    CVM_INTRINSIC_CAS64,
    CVM_INTRINSIC_XCHG64,
    CVM_INTRINSIC_ATOMIC_ADD64
} CvmIntrinsicForm;

typedef struct {
    const char *name;
    const char *mnemonic;
    CvmIntrinsicForm form;
} CvmIntrinsic;

static const CvmIntrinsic CVM_INTRINSICS[] = {
    {"cvm_halt", "HALT", CVM_INTRINSIC_VOID0},
    {"cvm_nop", "NOP", CVM_INTRINSIC_VOID0},
    {"cvm_enable_interrupts", "EI", CVM_INTRINSIC_VOID0},
    {"cvm_disable_interrupts", "DI", CVM_INTRINSIC_VOID0},
    {"cvm_wait", "WAIT", CVM_INTRINSIC_VOID0},
    {"cvm_fence", "FENCE", CVM_INTRINSIC_VOID0},
    {"cvm_core_id", "COREID", CVM_INTRINSIC_VALUE0},
    {"cvm_thread_id", "THREADID", CVM_INTRINSIC_VALUE0},
    {"cvm_exception_cause", "EXCAUSE", CVM_INTRINSIC_VALUE0},
    {"cvm_exception_address", "EXADDR", CVM_INTRINSIC_VALUE0},
    {"cvm_exception_pc", "EXPC", CVM_INTRINSIC_VALUE0},
    {"cvm_exception_info", "EXINFO", CVM_INTRINSIC_VALUE0},
    {"cvm_set_vbr", "SETVBR", CVM_INTRINSIC_VOID1},
    {"cvm_get_vbr", "GETVBR", CVM_INTRINSIC_VALUE0},
    {"cvm_get_mode", "GETMODE", CVM_INTRINSIC_VALUE0},
    {"cvm_enter_user", "ENTERUSER", CVM_INTRINSIC_VOID0},
    {"cvm_set_ptbr", "SETPTBR", CVM_INTRINSIC_VOID1},
    {"cvm_get_ptbr", "GETPTBR", CVM_INTRINSIC_VALUE0},
    {"cvm_mmu_on", "MMUON", CVM_INTRINSIC_VOID0},
    {"cvm_mmu_off", "MMUOFF", CVM_INTRINSIC_VOID0},
    {"cvm_get_mmu", "GETMMU", CVM_INTRINSIC_VALUE0},
    {"cvm_set_ksp", "SETKSP", CVM_INTRINSIC_VOID1},
    {"cvm_get_ksp", "GETKSP", CVM_INTRINSIC_VALUE0},
    {"cvm_cas64", "CAS64", CVM_INTRINSIC_CAS64},
    {"cvm_xchg64", "XCHG64", CVM_INTRINSIC_XCHG64},
    {"cvm_atomic_add64", "ATOMIC_ADD64", CVM_INTRINSIC_ATOMIC_ADD64}
};

static void fail(Generator *generator, const char *format, ...)
{
    if (generator->failed) return;
    generator->failed = 1;
    generator->error->line = 1;
    generator->error->column = 1;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(generator->error->message, sizeof(generator->error->message),
              format, arguments);
    va_end(arguments);
}

static int reserve_text(TextBuffer *buffer, size_t additional)
{
    if (additional > SIZE_MAX - buffer->length - 1) return 0;
    size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) return 1;
    size_t capacity = buffer->capacity != 0 ? buffer->capacity : 4096;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static void emit(Generator *generator, const char *format, ...)
{
    if (generator->failed) return;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !reserve_text(&generator->output, (size_t)needed)) {
        va_end(arguments);
        fail(generator, "out of memory while generating assembly");
        return;
    }
    vsnprintf(generator->output.data + generator->output.length,
              generator->output.capacity - generator->output.length,
              format, arguments);
    va_end(arguments);
    generator->output.length += (size_t)needed;
}

static int grow_array(Generator *generator, void **array, size_t element_size,
                      size_t count)
{
    if (count > SIZE_MAX / element_size) {
        fail(generator, "LLVM module is too large");
        return 0;
    }
    void *grown = realloc(*array, count * element_size);
    if (grown == NULL) {
        fail(generator, "out of memory while laying out function");
        return 0;
    }
    *array = grown;
    return 1;
}

static int get_name(Generator *generator, LLVMValueRef value,
                    char output[CVMIR_NAME_MAX])
{
    size_t length = 0;
    const char *name = LLVMGetValueName2(value, &length);
    if (name == NULL || length == 0 || length >= CVMIR_NAME_MAX) {
        fail(generator, "CVM requires a named LLVM global or function");
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        int valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                    c == '$';
        if (!valid || (i == 0 && c >= '0' && c <= '9')) {
            fail(generator, "LLVM symbol contains a character unsupported by vmasm");
            return 0;
        }
    }
    memcpy(output, name, length);
    output[length] = '\0';
    return 1;
}

static int is_local_linkage(LLVMLinkage linkage)
{
    return linkage == LLVMInternalLinkage || linkage == LLVMPrivateLinkage ||
           linkage == LLVMLinkerPrivateLinkage ||
           linkage == LLVMLinkerPrivateWeakLinkage;
}

static int is_weak_linkage(LLVMLinkage linkage)
{
    return linkage == LLVMWeakAnyLinkage || linkage == LLVMWeakODRLinkage ||
           linkage == LLVMExternalWeakLinkage ||
           linkage == LLVMLinkerPrivateWeakLinkage;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    if (alignment <= 1) return value;
    uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

static unsigned scalar_bits(LLVMTypeRef type)
{
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMPointerTypeKind) return 64;
    if (kind != LLVMIntegerTypeKind) return 0;
    unsigned bits = LLVMGetIntTypeWidth(type);
    return bits == 1 || bits == 8 || bits == 16 || bits == 32 || bits == 64
               ? bits : 0;
}

static int is_scalar_type(LLVMTypeRef type)
{
    return scalar_bits(type) != 0;
}

static int add_slot(Generator *generator, FunctionLayout *layout,
                    LLVMValueRef value, uint64_t *depth)
{
    size_t count = layout->slot_count + 1;
    if (!grow_array(generator, (void **)&layout->slots,
                    sizeof(*layout->slots), count)) return 0;
    *depth = align_up(*depth + 8, 8);
    if (*depth > INT32_MAX) {
        fail(generator, "function stack frame exceeds CVM disp32 range");
        return 0;
    }
    layout->slots[layout->slot_count++] = (ValueSlot){value, -(int64_t)*depth};
    return 1;
}

static int add_object(Generator *generator, FunctionLayout *layout,
                      LLVMValueRef alloca_value, uint64_t size,
                      uint64_t alignment, uint64_t *depth)
{
    size_t count = layout->object_count + 1;
    if (!grow_array(generator, (void **)&layout->objects,
                    sizeof(*layout->objects), count)) return 0;
    if (size > INT32_MAX || *depth > INT32_MAX - size) {
        fail(generator, "function stack object exceeds CVM disp32 range");
        return 0;
    }
    uint64_t end = align_up(*depth + size, alignment);
    if (end > INT32_MAX) {
        fail(generator, "function stack frame exceeds CVM disp32 range");
        return 0;
    }
    *depth = end;
    layout->objects[layout->object_count++] =
        (StackObject){alloca_value, -(int64_t)end};
    return 1;
}

static const ValueSlot *find_slot(const FunctionLayout *layout,
                                  LLVMValueRef value)
{
    for (size_t i = 0; i < layout->slot_count; ++i) {
        if (layout->slots[i].value == value) return &layout->slots[i];
    }
    return NULL;
}

static const StackObject *find_object(const FunctionLayout *layout,
                                      LLVMValueRef value)
{
    for (size_t i = 0; i < layout->object_count; ++i) {
        if (layout->objects[i].alloca_value == value) return &layout->objects[i];
    }
    return NULL;
}

static const BlockLabel *find_block(const FunctionLayout *layout,
                                    LLVMBasicBlockRef block)
{
    for (size_t i = 0; i < layout->block_count; ++i) {
        if (layout->blocks[i].block == block) return &layout->blocks[i];
    }
    return NULL;
}

static int constant_u64(LLVMValueRef value, uint64_t *result)
{
    if (LLVMGetValueKind(value) != LLVMConstantIntValueKind) return 0;
    *result = LLVMConstIntGetZExtValue(value);
    return 1;
}

static int constant_i64(LLVMValueRef value, int64_t *result)
{
    if (LLVMGetValueKind(value) != LLVMConstantIntValueKind) return 0;
    *result = (int64_t)LLVMConstIntGetSExtValue(value);
    return 1;
}

static void emit_zero_normalize(Generator *generator, const char *reg,
                                unsigned bits)
{
    if (bits == 1) emit(generator, "    ANDI32 %s, 1\n", reg);
    else if (bits == 8) emit(generator, "    ZEXT8 %s, %s\n", reg, reg);
    else if (bits == 16) emit(generator, "    ZEXT16 %s, %s\n", reg, reg);
    else if (bits == 32) emit(generator, "    ZEXT32 %s, %s\n", reg, reg);
}

static void emit_sign_normalize(Generator *generator, const char *reg,
                                unsigned bits)
{
    if (bits == 1) {
        emit(generator,
             "    ANDI32 %s, 1\n    MOVI64 R13, 63\n"
             "    SHLV %s, R13\n    SARV %s, R13\n", reg, reg, reg);
    } else if (bits == 8) emit(generator, "    SEXT8 %s, %s\n", reg, reg);
    else if (bits == 16) emit(generator, "    SEXT16 %s, %s\n", reg, reg);
    else if (bits == 32) emit(generator, "    SEXT32 %s, %s\n", reg, reg);
}

static int enum_attribute(LLVMValueRef function, LLVMAttributeIndex index,
                          const char *name)
{
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    return kind != 0 && LLVMGetEnumAttributeAtIndex(function, index, kind) != NULL;
}

static int call_attribute(LLVMValueRef call, LLVMAttributeIndex index,
                          const char *name)
{
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    return kind != 0 && LLVMGetCallSiteEnumAttribute(call, index, kind) != NULL;
}

static int constant_address(Generator *generator, LLVMValueRef value,
                            char symbol[CVMIR_NAME_MAX], int64_t *addend);

static int gep_constant_offset(Generator *generator, LLVMValueRef gep,
                               int64_t *offset)
{
    LLVMTypeRef current = LLVMGetGEPSourceElementType(gep);
    unsigned operands = LLVMGetNumOperands(gep);
    int64_t total = 0;
    for (unsigned i = 1; i < operands; ++i) {
        int64_t index = 0;
        if (!constant_i64(LLVMGetOperand(gep, i), &index)) {
            fail(generator, "global initializer GEP requires constant indices");
            return 0;
        }
        if (i == 1) {
            uint64_t stride = LLVMABISizeOfType(generator->target_data, current);
            total += index * (int64_t)stride;
            continue;
        }
        LLVMTypeKind kind = LLVMGetTypeKind(current);
        if (kind == LLVMStructTypeKind) {
            if (index < 0 || (uint64_t)index >= LLVMCountStructElementTypes(current)) {
                fail(generator, "GEP structure index is out of range");
                return 0;
            }
            total += (int64_t)LLVMOffsetOfElement(generator->target_data,
                                                  current, (unsigned)index);
            current = LLVMStructGetTypeAtIndex(current, (unsigned)index);
        } else if (kind == LLVMArrayTypeKind || kind == LLVMVectorTypeKind) {
            current = LLVMGetElementType(current);
            total += index * (int64_t)LLVMABISizeOfType(generator->target_data,
                                                        current);
        } else {
            fail(generator, "unsupported aggregate type in constant GEP");
            return 0;
        }
    }
    *offset = total;
    return 1;
}

static int constant_address(Generator *generator, LLVMValueRef value,
                            char symbol[CVMIR_NAME_MAX], int64_t *addend)
{
    LLVMValueKind kind = LLVMGetValueKind(value);
    if (kind == LLVMGlobalVariableValueKind || kind == LLVMFunctionValueKind ||
        kind == LLVMGlobalAliasValueKind) {
        *addend = 0;
        return get_name(generator, value, symbol);
    }
    if (kind == LLVMConstantPointerNullValueKind || LLVMIsNull(value)) {
        symbol[0] = '\0';
        *addend = 0;
        return 1;
    }
    if (kind == LLVMConstantIntValueKind) {
        symbol[0] = '\0';
        *addend = LLVMConstIntGetSExtValue(value);
        return 1;
    }
    if (kind != LLVMConstantExprValueKind) return 0;
    LLVMOpcode opcode = LLVMGetConstOpcode(value);
    if (opcode == LLVMBitCast || opcode == LLVMAddrSpaceCast ||
        opcode == LLVMPtrToInt || opcode == LLVMIntToPtr ||
        opcode == LLVMPtrToAddr) {
        return constant_address(generator, LLVMGetOperand(value, 0), symbol,
                                addend);
    }
    if (opcode == LLVMGetElementPtr) {
        if (!constant_address(generator, LLVMGetOperand(value, 0), symbol,
                              addend)) return 0;
        int64_t offset = 0;
        if (!gep_constant_offset(generator, value, &offset)) return 0;
        *addend += offset;
        return 1;
    }
    return 0;
}

static int emit_load_value(Generator *generator, const FunctionLayout *layout,
                           LLVMValueRef value, const char *reg)
{
    LLVMValueKind kind = LLVMGetValueKind(value);
    if (kind == LLVMConstantIntValueKind) {
        emit(generator, "    MOVI64 %s, 0x%llx\n", reg,
             (unsigned long long)LLVMConstIntGetZExtValue(value));
        return 1;
    }
    if (kind == LLVMConstantPointerNullValueKind ||
        kind == LLVMUndefValueValueKind || kind == LLVMPoisonValueValueKind ||
        LLVMIsNull(value)) {
        emit(generator, "    MOVI64 %s, 0\n", reg);
        return 1;
    }
    if (kind == LLVMGlobalVariableValueKind || kind == LLVMFunctionValueKind ||
        kind == LLVMGlobalAliasValueKind || kind == LLVMConstantExprValueKind) {
        char symbol[CVMIR_NAME_MAX];
        int64_t addend = 0;
        if (!constant_address(generator, value, symbol, &addend)) {
            char *printed = LLVMPrintValueToString(value);
            fail(generator, "unsupported LLVM constant pointer expression: %.120s",
                 printed != NULL ? printed : "<unknown>");
            LLVMDisposeMessage(printed);
            return 0;
        }
        if (symbol[0] == '\0') emit(generator, "    MOVI64 %s, %lld\n", reg,
                                     (long long)addend);
        else if (addend == 0) emit(generator, "    MOVI64 %s, %s\n", reg, symbol);
        else emit(generator, "    MOVI64 %s, %s%+lld\n", reg, symbol,
                  (long long)addend);
        return 1;
    }
    const ValueSlot *slot = find_slot(layout, value);
    if (slot == NULL) {
        fail(generator, "LLVM value has no assigned CVM stack slot");
        return 0;
    }
    emit(generator, "    LOAD64O %s, R14, %lld\n", reg,
         (long long)slot->offset);
    return 1;
}

static int emit_store_result(Generator *generator,
                             const FunctionLayout *layout,
                             LLVMValueRef value, const char *reg)
{
    const ValueSlot *slot = find_slot(layout, value);
    if (slot == NULL) {
        fail(generator, "LLVM instruction result has no CVM stack slot");
        return 0;
    }
    emit(generator, "    STORE64O R14, %s, %lld\n", reg,
         (long long)slot->offset);
    return 1;
}

static int build_layout(Generator *generator, LLVMValueRef function,
                        FunctionLayout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->function = function;
    uint64_t depth = 0;
    unsigned parameter_count = LLVMCountParams(function);
    for (unsigned i = 0; i < parameter_count; ++i) {
        LLVMValueRef parameter = LLVMGetParam(function, i);
        if (!is_scalar_type(LLVMTypeOf(parameter))) {
            fail(generator, "CVM bridge supports only scalar integer/pointer parameters");
            return 0;
        }
        if (!add_slot(generator, layout, parameter, &depth)) return 0;
    }
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block != NULL;
         block = LLVMGetNextBasicBlock(block)) {
        size_t count = layout->block_count + 1;
        if (!grow_array(generator, (void **)&layout->blocks,
                        sizeof(*layout->blocks), count)) return 0;
        layout->blocks[layout->block_count++] =
            (BlockLabel){block, ++generator->next_label};
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
             instruction != NULL; instruction = LLVMGetNextInstruction(instruction)) {
            LLVMTypeRef type = LLVMTypeOf(instruction);
            if (LLVMGetTypeKind(type) != LLVMVoidTypeKind) {
                if (!is_scalar_type(type)) {
                    fail(generator, "LLVM instruction produces an unsupported aggregate or floating result");
                    return 0;
                }
                if (!add_slot(generator, layout, instruction, &depth)) return 0;
            }
            if (LLVMGetInstructionOpcode(instruction) == LLVMAlloca) {
                LLVMTypeRef allocated = LLVMGetAllocatedType(instruction);
                uint64_t count_value = 0;
                if (!constant_u64(LLVMGetOperand(instruction, 0), &count_value)) {
                    fail(generator, "dynamic alloca is not supported by the CVM bridge");
                    return 0;
                }
                uint64_t element_size = LLVMABISizeOfType(generator->target_data,
                                                          allocated);
                uint64_t alignment = LLVMGetAlignment(instruction);
                if (alignment == 0)
                    alignment = LLVMABIAlignmentOfType(generator->target_data,
                                                       allocated);
                if (count_value != 0 && element_size > UINT64_MAX / count_value) {
                    fail(generator, "alloca size overflows 64 bits");
                    return 0;
                }
                if (!add_object(generator, layout, instruction,
                                element_size * count_value, alignment, &depth))
                    return 0;
            }
        }
    }
    layout->frame_size = align_up(depth, 16);
    return !generator->failed;
}

static void destroy_layout(FunctionLayout *layout)
{
    free(layout->slots);
    free(layout->objects);
    free(layout->blocks);
    memset(layout, 0, sizeof(*layout));
}

static const char *memory_mnemonic(int store, unsigned bits)
{
    if (store) {
        if (bits == 1 || bits == 8) return "STORE8";
        if (bits == 16) return "STORE16";
        if (bits == 32) return "STORE32";
        if (bits == 64) return "STORE64";
    } else {
        if (bits == 1 || bits == 8) return "LOAD8U";
        if (bits == 16) return "LOAD16U";
        if (bits == 32) return "LOAD32U";
        if (bits == 64) return "LOAD64";
    }
    return NULL;
}

static int emit_gep(Generator *generator, const FunctionLayout *layout,
                    LLVMValueRef instruction)
{
    if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 0),
                         "R0")) return 0;
    LLVMTypeRef current = LLVMGetGEPSourceElementType(instruction);
    unsigned operands = LLVMGetNumOperands(instruction);
    for (unsigned i = 1; i < operands; ++i) {
        LLVMValueRef index_value = LLVMGetOperand(instruction, i);
        uint64_t stride = 0;
        int64_t constant_offset = 0;
        if (i == 1) {
            stride = LLVMABISizeOfType(generator->target_data, current);
        } else {
            LLVMTypeKind kind = LLVMGetTypeKind(current);
            if (kind == LLVMStructTypeKind) {
                int64_t field = 0;
                if (!constant_i64(index_value, &field) || field < 0 ||
                    (uint64_t)field >= LLVMCountStructElementTypes(current)) {
                    fail(generator, "GEP structure indices must be valid constants");
                    return 0;
                }
                constant_offset = (int64_t)LLVMOffsetOfElement(
                    generator->target_data, current, (unsigned)field);
                current = LLVMStructGetTypeAtIndex(current, (unsigned)field);
            } else if (kind == LLVMArrayTypeKind || kind == LLVMVectorTypeKind) {
                current = LLVMGetElementType(current);
                stride = LLVMABISizeOfType(generator->target_data, current);
            } else {
                fail(generator, "unsupported LLVM type in GEP");
                return 0;
            }
        }
        int64_t constant_index = 0;
        if (stride != 0 && constant_i64(index_value, &constant_index)) {
            constant_offset += constant_index * (int64_t)stride;
        } else if (stride != 0) {
            if (!emit_load_value(generator, layout, index_value, "R1")) return 0;
            emit(generator, "    MOVI64 R2, %llu\n    MUL R1, R2\n    ADD R0, R1\n",
                 (unsigned long long)stride);
        }
        if (constant_offset != 0) {
            if (constant_offset >= INT32_MIN && constant_offset <= INT32_MAX)
                emit(generator, "    ADDI32 R0, %lld\n", (long long)constant_offset);
            else
                emit(generator, "    MOVI64 R1, %lld\n    ADD R0, R1\n",
                     (long long)constant_offset);
        }
    }
    return emit_store_result(generator, layout, instruction, "R0");
}

static int emit_phi_edge(Generator *generator, const FunctionLayout *layout,
                         LLVMBasicBlockRef from, LLVMBasicBlockRef to)
{
    unsigned count = 0;
    for (LLVMValueRef phi = LLVMGetFirstInstruction(to); phi != NULL &&
         LLVMGetInstructionOpcode(phi) == LLVMPHI; phi = LLVMGetNextInstruction(phi)) {
        unsigned incoming_count = LLVMCountIncoming(phi);
        LLVMValueRef incoming = NULL;
        for (unsigned i = 0; i < incoming_count; ++i) {
            if (LLVMGetIncomingBlock(phi, i) == from) {
                incoming = LLVMGetIncomingValue(phi, i);
                break;
            }
        }
        if (incoming == NULL) {
            fail(generator, "PHI node is missing an incoming branch edge");
            return 0;
        }
        if (!emit_load_value(generator, layout, incoming, "R0")) return 0;
        emit(generator, "    PUSH R0\n");
        ++count;
    }
    if (count == 0) return 1;
    LLVMValueRef *phis = malloc((size_t)count * sizeof(*phis));
    if (phis == NULL) {
        fail(generator, "out of memory while lowering PHI nodes");
        return 0;
    }
    unsigned index = 0;
    for (LLVMValueRef phi = LLVMGetFirstInstruction(to); phi != NULL &&
         LLVMGetInstructionOpcode(phi) == LLVMPHI; phi = LLVMGetNextInstruction(phi))
        phis[index++] = phi;
    while (index != 0) {
        LLVMValueRef phi = phis[--index];
        emit(generator, "    POP R0\n");
        if (!emit_store_result(generator, layout, phi, "R0")) {
            free(phis);
            return 0;
        }
    }
    free(phis);
    return 1;
}

static int emit_branch_to(Generator *generator, const FunctionLayout *layout,
                          LLVMBasicBlockRef from, LLVMBasicBlockRef to)
{
    const BlockLabel *target = find_block(layout, to);
    if (target == NULL) {
        fail(generator, "branch target has no CVM label");
        return 0;
    }
    if (!emit_phi_edge(generator, layout, from, to)) return 0;
    emit(generator, "    JUMP .L%u\n", target->label);
    return 1;
}

static const CvmIntrinsic *find_cvm_intrinsic(const char *name)
{
    for (size_t i = 0; i < sizeof(CVM_INTRINSICS) / sizeof(CVM_INTRINSICS[0]);
         ++i) {
        if (strcmp(CVM_INTRINSICS[i].name, name) == 0)
            return &CVM_INTRINSICS[i];
    }
    return NULL;
}

static int emit_cvm_intrinsic(Generator *generator,
                              const FunctionLayout *layout,
                              LLVMValueRef instruction,
                              const CvmIntrinsic *intrinsic)
{
    unsigned arguments = LLVMGetNumArgOperands(instruction);
    LLVMTypeKind result_kind = LLVMGetTypeKind(LLVMTypeOf(instruction));
    unsigned expected_arguments = 0;
    int returns_value = 0;
    switch (intrinsic->form) {
        case CVM_INTRINSIC_VOID0:
            break;
        case CVM_INTRINSIC_VALUE0:
            returns_value = 1;
            break;
        case CVM_INTRINSIC_VOID1:
            expected_arguments = 1;
            break;
        case CVM_INTRINSIC_CAS64:
            expected_arguments = 3;
            returns_value = 1;
            break;
        case CVM_INTRINSIC_XCHG64:
        case CVM_INTRINSIC_ATOMIC_ADD64:
            expected_arguments = 2;
            returns_value = 1;
            break;
    }
    if (arguments != expected_arguments ||
        (returns_value && !is_scalar_type(LLVMTypeOf(instruction))) ||
        (!returns_value && result_kind != LLVMVoidTypeKind)) {
        fail(generator, "CVM intrinsic '%s' has an invalid signature",
             intrinsic->name);
        return 0;
    }
    static const char *const argument_registers[] = {"R0", "R1", "R2"};
    for (unsigned i = 0; i < arguments; ++i) {
        LLVMValueRef argument = LLVMGetOperand(instruction, i);
        if (!is_scalar_type(LLVMTypeOf(argument))) {
            fail(generator, "CVM intrinsic '%s' requires scalar arguments",
                 intrinsic->name);
            return 0;
        }
        if (!emit_load_value(generator, layout, argument,
                             argument_registers[i])) return 0;
    }
    switch (intrinsic->form) {
        case CVM_INTRINSIC_VOID0:
            emit(generator, "    %s\n", intrinsic->mnemonic);
            break;
        case CVM_INTRINSIC_VALUE0:
            emit(generator, "    %s R0\n", intrinsic->mnemonic);
            break;
        case CVM_INTRINSIC_VOID1:
            emit(generator, "    %s R0\n", intrinsic->mnemonic);
            break;
        case CVM_INTRINSIC_CAS64:
            emit(generator, "    CAS64 R0, R1, R2, R3\n    MOV R0, R3\n");
            break;
        case CVM_INTRINSIC_XCHG64:
            emit(generator, "    XCHG64 R0, R1\n    MOV R0, R1\n");
            break;
        case CVM_INTRINSIC_ATOMIC_ADD64:
            emit(generator,
                 "    ATOMIC_ADD64 R0, R1, R2\n    MOV R0, R2\n");
            break;
    }
    return !returns_value ||
           emit_store_result(generator, layout, instruction, "R0");
}

static int emit_call(Generator *generator, const FunctionLayout *layout,
                     LLVMValueRef instruction)
{
    LLVMValueRef called = LLVMGetCalledValue(instruction);
    while (LLVMGetValueKind(called) == LLVMConstantExprValueKind &&
           LLVMGetConstOpcode(called) == LLVMBitCast)
        called = LLVMGetOperand(called, 0);
    if (LLVMGetValueKind(called) != LLVMFunctionValueKind) {
        fail(generator, "indirect calls are not supported by the P0 CVM bridge");
        return 0;
    }
    char callee[CVMIR_NAME_MAX];
    if (!get_name(generator, called, callee)) return 0;
    const CvmIntrinsic *intrinsic = find_cvm_intrinsic(callee);
    if (intrinsic != NULL)
        return emit_cvm_intrinsic(generator, layout, instruction, intrinsic);
    LLVMTypeRef function_type = LLVMGetCalledFunctionType(instruction);
    unsigned argument_count = LLVMGetNumArgOperands(instruction);
    unsigned fixed_count = LLVMCountParamTypes(function_type);
    int variadic = LLVMIsFunctionVarArg(function_type);
    unsigned register_count = variadic && fixed_count < 8 ? fixed_count :
                              (argument_count < 8 ? argument_count : 8);
    unsigned first_stack = register_count;
    unsigned stack_count = argument_count - first_stack;
    unsigned padding = stack_count & 1U;
    if (padding) emit(generator, "    ADDI32 SP, -8\n");
    for (unsigned i = argument_count; i > first_stack; --i) {
        LLVMValueRef argument = LLVMGetOperand(instruction, i - 1);
        if (!is_scalar_type(LLVMTypeOf(argument)) ||
            !emit_load_value(generator, layout, argument, "R0")) return 0;
        if (call_attribute(instruction, i, "signext"))
            emit_sign_normalize(generator, "R0", scalar_bits(LLVMTypeOf(argument)));
        else if (call_attribute(instruction, i, "zeroext"))
            emit_zero_normalize(generator, "R0", scalar_bits(LLVMTypeOf(argument)));
        emit(generator, "    PUSH R0\n");
    }
    for (unsigned i = 0; i < register_count; ++i) {
        char reg[8];
        snprintf(reg, sizeof(reg), "R%u", i);
        LLVMValueRef argument = LLVMGetOperand(instruction, i);
        if (!is_scalar_type(LLVMTypeOf(argument)) ||
            !emit_load_value(generator, layout, argument, reg)) return 0;
        if (call_attribute(instruction, i + 1, "signext"))
            emit_sign_normalize(generator, reg, scalar_bits(LLVMTypeOf(argument)));
        else if (call_attribute(instruction, i + 1, "zeroext"))
            emit_zero_normalize(generator, reg, scalar_bits(LLVMTypeOf(argument)));
    }
    emit(generator, "    CALLREL %s\n", callee);
    if (stack_count != 0 || padding)
        emit(generator, "    ADDI32 SP, %u\n", (stack_count + padding) * 8);
    LLVMTypeRef result_type = LLVMTypeOf(instruction);
    if (LLVMGetTypeKind(result_type) != LLVMVoidTypeKind) {
        unsigned bits = scalar_bits(result_type);
        if (call_attribute(instruction, LLVMAttributeReturnIndex, "signext"))
            emit_sign_normalize(generator, "R0", bits);
        else if (call_attribute(instruction, LLVMAttributeReturnIndex, "zeroext"))
            emit_zero_normalize(generator, "R0", bits);
        if (!emit_store_result(generator, layout, instruction, "R0")) return 0;
    }
    return 1;
}

static const char *binary_mnemonic(LLVMOpcode opcode)
{
    switch (opcode) {
        case LLVMAdd: return "ADD";
        case LLVMSub: return "SUB";
        case LLVMMul: return "MUL";
        case LLVMUDiv: return "DIVU";
        case LLVMSDiv: return "DIVS";
        case LLVMURem: return "MODU";
        case LLVMSRem: return "MODS";
        case LLVMAnd: return "AND";
        case LLVMOr: return "OR";
        case LLVMXor: return "XOR";
        case LLVMShl: return "SHLV";
        case LLVMLShr: return "SHRV";
        case LLVMAShr: return "SARV";
        default: return NULL;
    }
}

static int emit_binary(Generator *generator, const FunctionLayout *layout,
                       LLVMValueRef instruction, LLVMOpcode opcode)
{
    unsigned bits = scalar_bits(LLVMTypeOf(instruction));
    if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 0), "R0") ||
        !emit_load_value(generator, layout, LLVMGetOperand(instruction, 1), "R1"))
        return 0;
    if (opcode == LLVMSDiv || opcode == LLVMSRem || opcode == LLVMAShr) {
        emit_sign_normalize(generator, "R0", bits);
        emit_sign_normalize(generator, "R1", bits);
    }
    emit(generator, "    %s R0, R1\n", binary_mnemonic(opcode));
    emit_zero_normalize(generator, "R0", bits);
    return emit_store_result(generator, layout, instruction, "R0");
}

static int emit_icmp(Generator *generator, const FunctionLayout *layout,
                     LLVMValueRef instruction)
{
    LLVMValueRef left = LLVMGetOperand(instruction, 0);
    LLVMValueRef right = LLVMGetOperand(instruction, 1);
    unsigned bits = scalar_bits(LLVMTypeOf(left));
    LLVMIntPredicate predicate = LLVMGetICmpPredicate(instruction);
    if (!emit_load_value(generator, layout, left, "R0") ||
        !emit_load_value(generator, layout, right, "R1")) return 0;
    if (predicate == LLVMIntSLT || predicate == LLVMIntSLE ||
        predicate == LLVMIntSGT || predicate == LLVMIntSGE) {
        emit_sign_normalize(generator, "R0", bits);
        emit_sign_normalize(generator, "R1", bits);
    }
    const char *jump = NULL;
    switch (predicate) {
        case LLVMIntEQ: jump = "JZ"; break;
        case LLVMIntNE: jump = "JNZ"; break;
        case LLVMIntUGT: jump = "JGTU"; break;
        case LLVMIntUGE: jump = "JGEU"; break;
        case LLVMIntULT: jump = "JLTU"; break;
        case LLVMIntULE: jump = "JLEU"; break;
        case LLVMIntSGT: jump = "JGT"; break;
        case LLVMIntSGE: jump = "JGE"; break;
        case LLVMIntSLT: jump = "JLT"; break;
        case LLVMIntSLE: jump = "JLE"; break;
        default: break;
    }
    if (jump == NULL) {
        fail(generator, "unsupported LLVM icmp predicate");
        return 0;
    }
    unsigned true_label = ++generator->next_label;
    unsigned end_label = ++generator->next_label;
    emit(generator,
         "    CMP R0, R1\n    MOVI64 R0, 0\n    %s .L%u\n"
         "    JUMP .L%u\n.L%u:\n    MOVI64 R0, 1\n.L%u:\n",
         jump, true_label, end_label, true_label, end_label);
    return emit_store_result(generator, layout, instruction, "R0");
}

static int emit_instruction(Generator *generator,
                            const FunctionLayout *layout,
                            LLVMBasicBlockRef block,
                            LLVMValueRef instruction, unsigned return_label)
{
    LLVMOpcode opcode = LLVMGetInstructionOpcode(instruction);
    const char *binary = binary_mnemonic(opcode);
    if (binary != NULL) return emit_binary(generator, layout, instruction, opcode);
    if (opcode == LLVMPHI) return 1;
    if (opcode == LLVMAlloca) {
        const StackObject *object = find_object(layout, instruction);
        if (object == NULL) {
            fail(generator, "alloca has no CVM stack object");
            return 0;
        }
        emit(generator, "    LEA R0, R14, %lld\n", (long long)object->offset);
        return emit_store_result(generator, layout, instruction, "R0");
    }
    if (opcode == LLVMLoad) {
        unsigned bits = scalar_bits(LLVMTypeOf(instruction));
        const char *mnemonic = memory_mnemonic(0, bits);
        if (mnemonic == NULL ||
            !emit_load_value(generator, layout, LLVMGetOperand(instruction, 0),
                             "R1")) {
            if (!generator->failed) fail(generator, "unsupported LLVM load type");
            return 0;
        }
        emit(generator, "    %s R0, R1\n", mnemonic);
        emit_zero_normalize(generator, "R0", bits);
        return emit_store_result(generator, layout, instruction, "R0");
    }
    if (opcode == LLVMStore) {
        LLVMValueRef value = LLVMGetOperand(instruction, 0);
        unsigned bits = scalar_bits(LLVMTypeOf(value));
        const char *mnemonic = memory_mnemonic(1, bits);
        if (mnemonic == NULL || !emit_load_value(generator, layout, value, "R0") ||
            !emit_load_value(generator, layout, LLVMGetOperand(instruction, 1),
                             "R1")) {
            if (!generator->failed) fail(generator, "unsupported LLVM store type");
            return 0;
        }
        emit(generator, "    %s R1, R0\n", mnemonic);
        return 1;
    }
    if (opcode == LLVMGetElementPtr) return emit_gep(generator, layout, instruction);
    if (opcode == LLVMTrunc || opcode == LLVMZExt || opcode == LLVMSExt ||
        opcode == LLVMPtrToInt || opcode == LLVMPtrToAddr ||
        opcode == LLVMIntToPtr || opcode == LLVMBitCast ||
        opcode == LLVMAddrSpaceCast) {
        LLVMValueRef source = LLVMGetOperand(instruction, 0);
        if (!emit_load_value(generator, layout, source, "R0")) return 0;
        unsigned source_bits = scalar_bits(LLVMTypeOf(source));
        unsigned result_bits = scalar_bits(LLVMTypeOf(instruction));
        if (opcode == LLVMSExt) emit_sign_normalize(generator, "R0", source_bits);
        else if (opcode == LLVMZExt) emit_zero_normalize(generator, "R0", source_bits);
        else if (opcode == LLVMTrunc) emit_zero_normalize(generator, "R0", result_bits);
        return emit_store_result(generator, layout, instruction, "R0");
    }
    if (opcode == LLVMICmp) return emit_icmp(generator, layout, instruction);
    if (opcode == LLVMSelect) {
        unsigned false_label = ++generator->next_label;
        unsigned end_label = ++generator->next_label;
        if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 0),
                             "R0")) return 0;
        emit(generator, "    CMPI32 R0, 0\n    JZ .L%u\n", false_label);
        if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 1),
                             "R0")) return 0;
        emit(generator, "    JUMP .L%u\n.L%u:\n", end_label, false_label);
        if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 2),
                             "R0")) return 0;
        emit(generator, ".L%u:\n", end_label);
        emit_zero_normalize(generator, "R0", scalar_bits(LLVMTypeOf(instruction)));
        return emit_store_result(generator, layout, instruction, "R0");
    }
    if (opcode == LLVMCall) return emit_call(generator, layout, instruction);
    if (opcode == LLVMBr) {
        unsigned successors = LLVMGetNumSuccessors(instruction);
        if (successors == 1)
            return emit_branch_to(generator, layout, block,
                                  LLVMGetSuccessor(instruction, 0));
        if (successors == 2) {
            unsigned false_edge = ++generator->next_label;
            if (!emit_load_value(generator, layout, LLVMGetOperand(instruction, 0),
                                 "R0")) return 0;
            emit(generator, "    CMPI32 R0, 0\n    JZ .L%u\n", false_edge);
            if (!emit_branch_to(generator, layout, block,
                                LLVMGetSuccessor(instruction, 0))) return 0;
            emit(generator, ".L%u:\n", false_edge);
            return emit_branch_to(generator, layout, block,
                                  LLVMGetSuccessor(instruction, 1));
        }
        fail(generator, "invalid LLVM branch successor count");
        return 0;
    }
    if (opcode == LLVMRet) {
        if (LLVMGetNumOperands(instruction) != 0) {
            LLVMValueRef value = LLVMGetOperand(instruction, 0);
            if (!emit_load_value(generator, layout, value, "R0")) return 0;
            LLVMValueRef function = layout->function;
            unsigned bits = scalar_bits(LLVMTypeOf(value));
            if (enum_attribute(function, LLVMAttributeReturnIndex, "signext"))
                emit_sign_normalize(generator, "R0", bits);
            else if (enum_attribute(function, LLVMAttributeReturnIndex, "zeroext"))
                emit_zero_normalize(generator, "R0", bits);
        }
        emit(generator, "    JUMP .L%u\n", return_label);
        return 1;
    }
    if (opcode == LLVMUnreachable) {
        emit(generator, "    HALT\n");
        return 1;
    }
    char *printed = LLVMPrintValueToString(instruction);
    fail(generator, "unsupported LLVM instruction: %.120s",
         printed != NULL ? printed : "<unknown>");
    LLVMDisposeMessage(printed);
    return 0;
}

static int emit_function(Generator *generator, LLVMValueRef function)
{
    FunctionLayout layout;
    if (!build_layout(generator, function, &layout)) {
        destroy_layout(&layout);
        return 0;
    }
    char name[CVMIR_NAME_MAX];
    if (!get_name(generator, function, name)) {
        destroy_layout(&layout);
        return 0;
    }
    LLVMLinkage linkage = LLVMGetLinkage(function);
    emit(generator, "\n.section .text\n");
    if (!is_local_linkage(linkage)) emit(generator, ".global %s\n", name);
    if (is_weak_linkage(linkage)) emit(generator, ".weak %s\n", name);
    emit(generator, ".type %s, function\n%s:\n", name, name);
    emit(generator, "    PUSH R14\n    MOV R14, SP\n");
    if (layout.frame_size != 0)
        emit(generator, "    ADDI32 SP, -%llu\n",
             (unsigned long long)layout.frame_size);
    unsigned parameters = LLVMCountParams(function);
    for (unsigned i = 0; i < parameters; ++i) {
        LLVMValueRef parameter = LLVMGetParam(function, i);
        const ValueSlot *slot = find_slot(&layout, parameter);
        if (slot == NULL) {
            fail(generator, "function parameter has no CVM stack slot");
            destroy_layout(&layout);
            return 0;
        }
        if (i < 8)
            emit(generator, "    STORE64O R14, R%u, %lld\n", i,
                 (long long)slot->offset);
        else {
            emit(generator,
                 "    LOAD64O R0, R14, %u\n    STORE64O R14, R0, %lld\n",
                 16 + (i - 8) * 8, (long long)slot->offset);
        }
    }
    unsigned return_label = ++generator->next_label;
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block != NULL;
         block = LLVMGetNextBasicBlock(block)) {
        const BlockLabel *label = find_block(&layout, block);
        emit(generator, ".L%u:\n", label->label);
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
             instruction != NULL; instruction = LLVMGetNextInstruction(instruction)) {
            if (!emit_instruction(generator, &layout, block, instruction,
                                  return_label)) {
                destroy_layout(&layout);
                return 0;
            }
        }
    }
    emit(generator,
         ".L%u:\n    MOV SP, R14\n    POP R14\n    RET\n.size %s, $ - %s\n",
         return_label, name, name);
    destroy_layout(&layout);
    return !generator->failed;
}

static void emit_zero_bytes(Generator *generator, uint64_t count)
{
    if (count != 0) emit(generator, "    .zero %llu\n",
                         (unsigned long long)count);
}

static int emit_initializer(Generator *generator, LLVMValueRef constant,
                            LLVMTypeRef type)
{
    uint64_t size = LLVMABISizeOfType(generator->target_data, type);
    if (LLVMIsNull(constant) || LLVMGetValueKind(constant) == LLVMUndefValueValueKind ||
        LLVMGetValueKind(constant) == LLVMPoisonValueValueKind) {
        emit_zero_bytes(generator, size);
        return 1;
    }
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(type);
        if (LLVMGetValueKind(constant) != LLVMConstantIntValueKind || bits > 64) {
            fail(generator, "unsupported integer global initializer");
            return 0;
        }
        uint64_t value = LLVMConstIntGetZExtValue(constant);
        if (bits <= 8) emit(generator, "    .byte 0x%llx\n", (unsigned long long)value);
        else if (bits == 16) emit(generator, "    .word 0x%llx\n", (unsigned long long)value);
        else if (bits == 32) emit(generator, "    .dword 0x%llx\n", (unsigned long long)value);
        else if (bits == 64) emit(generator, "    .qword 0x%llx\n", (unsigned long long)value);
        else {
            fail(generator, "CVM globals support i1/i8/i16/i32/i64 only");
            return 0;
        }
        return 1;
    }
    if (kind == LLVMPointerTypeKind) {
        char symbol[CVMIR_NAME_MAX];
        int64_t addend = 0;
        if (!constant_address(generator, constant, symbol, &addend)) {
            fail(generator, "unsupported pointer global initializer");
            return 0;
        }
        if (symbol[0] == '\0') emit(generator, "    .qword %lld\n", (long long)addend);
        else if (addend == 0) emit(generator, "    .qword %s\n", symbol);
        else emit(generator, "    .qword %s%+lld\n", symbol, (long long)addend);
        return 1;
    }
    if (kind == LLVMArrayTypeKind || kind == LLVMVectorTypeKind) {
        uint64_t count = kind == LLVMArrayTypeKind ? LLVMGetArrayLength2(type)
                                                   : LLVMGetVectorSize(type);
        LLVMTypeRef element_type = LLVMGetElementType(type);
        for (uint64_t i = 0; i < count; ++i) {
            LLVMValueRef element = LLVMGetAggregateElement(constant, (unsigned)i);
            if (element == NULL || !emit_initializer(generator, element, element_type))
                return 0;
        }
        return 1;
    }
    if (kind == LLVMStructTypeKind) {
        unsigned count = LLVMCountStructElementTypes(type);
        uint64_t cursor = 0;
        for (unsigned i = 0; i < count; ++i) {
            uint64_t offset = LLVMOffsetOfElement(generator->target_data, type, i);
            emit_zero_bytes(generator, offset - cursor);
            LLVMTypeRef element_type = LLVMStructGetTypeAtIndex(type, i);
            LLVMValueRef element = LLVMGetAggregateElement(constant, i);
            if (element == NULL || !emit_initializer(generator, element, element_type))
                return 0;
            cursor = offset + LLVMABISizeOfType(generator->target_data, element_type);
        }
        emit_zero_bytes(generator, size - cursor);
        return 1;
    }
    fail(generator, "unsupported LLVM global initializer type");
    return 0;
}

static int emit_globals(Generator *generator, LLVMModuleRef module)
{
    for (LLVMValueRef global = LLVMGetFirstGlobal(module); global != NULL;
         global = LLVMGetNextGlobal(global)) {
        char name[CVMIR_NAME_MAX];
        if (!get_name(generator, global, name)) return 0;
        LLVMValueRef initializer = LLVMGetInitializer(global);
        if (initializer == NULL) {
            emit(generator, ".extern %s\n.type %s, object\n", name, name);
            continue;
        }
        LLVMTypeRef type = LLVMGlobalGetValueType(global);
        uint64_t size = LLVMABISizeOfType(generator->target_data, type);
        unsigned alignment = LLVMGetAlignment(global);
        if (alignment == 0)
            alignment = LLVMABIAlignmentOfType(generator->target_data, type);
        LLVMLinkage linkage = LLVMGetLinkage(global);
        if (linkage == LLVMCommonLinkage) {
            emit(generator, ".comm %s, %llu, %u\n", name,
                 (unsigned long long)size, alignment);
            continue;
        }
        const char *section = LLVMIsGlobalConstant(global) ? ".rodata" :
                              LLVMIsNull(initializer) ? ".bss" : ".data";
        emit(generator, "\n.section %s\n", section);
        if (!is_local_linkage(linkage)) emit(generator, ".global %s\n", name);
        if (is_weak_linkage(linkage)) emit(generator, ".weak %s\n", name);
        emit(generator, ".type %s, object\n.align %u\n%s:\n", name,
             alignment, name);
        if (strcmp(section, ".bss") == 0) emit_zero_bytes(generator, size);
        else if (!emit_initializer(generator, initializer, type)) return 0;
        emit(generator, ".size %s, $ - %s\n", name, name);
    }
    return !generator->failed;
}

static int emit_module(Generator *generator, LLVMModuleRef module)
{
    emit(generator, "; generated by LLVM-backed cvmir for %s\n",
         RARCH_M64_LLVM_TARGET_TRIPLE);
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function != NULL;
         function = LLVMGetNextFunction(function)) {
        if (!LLVMIsDeclaration(function)) continue;
        char name[CVMIR_NAME_MAX];
        if (!get_name(generator, function, name)) return 0;
        if (find_cvm_intrinsic(name) != NULL) continue;
        emit(generator, ".extern %s\n.type %s, function\n", name, name);
    }
    if (!emit_globals(generator, module)) return 0;
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function != NULL;
         function = LLVMGetNextFunction(function)) {
        if (!LLVMIsDeclaration(function) && !emit_function(generator, function))
            return 0;
    }
    return !generator->failed;
}

static void set_llvm_error(CvmIrError *error, const char *message)
{
    error->line = 1;
    error->column = 1;
    const char *text = message != NULL ? message : "unknown LLVM error";
    const char *marker = strstr(text, "error: ");
    if (marker != NULL) text = marker + 7;
    snprintf(error->message, sizeof(error->message), "%s", text);
    size_t length = strlen(error->message);
    while (length != 0 && (error->message[length - 1] == '\n' ||
                           error->message[length - 1] == '\r'))
        error->message[--length] = '\0';
}

int cvmir_translate(const char *source, const CvmIrOptions *options,
                    char **assembly, CvmIrError *error)
{
    if (source == NULL || options == NULL || assembly == NULL || error == NULL)
        return 0;
    *assembly = NULL;
    memset(error, 0, sizeof(*error));
    LLVMContextRef context = LLVMContextCreate();
    if (context == NULL) {
        set_llvm_error(error, "cannot create LLVM context");
        return 0;
    }
    LLVMMemoryBufferRef buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        source, strlen(source), "<cvmir-input>");
    LLVMModuleRef module = NULL;
    char *message = NULL;
    if (buffer == NULL || LLVMParseIRInContext(context, buffer, &module, &message)) {
        set_llvm_error(error, message != NULL ? message : "cannot parse LLVM IR");
        LLVMDisposeMessage(message);
        LLVMContextDispose(context);
        return 0;
    }
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) {
        set_llvm_error(error, message != NULL ? message : "invalid LLVM module");
        LLVMDisposeMessage(message);
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        return 0;
    }
    LLVMDisposeMessage(message);
    const char *triple = LLVMGetTarget(module);
    const char *layout = LLVMGetDataLayoutStr(module);
    if (!options->allow_foreign_triple && triple != NULL && *triple != '\0' &&
        strcmp(triple, RARCH_M64_LLVM_TARGET_TRIPLE) != 0) {
        snprintf(error->message, sizeof(error->message),
                 "IR target triple '%s' is not '%s'", triple,
                 RARCH_M64_LLVM_TARGET_TRIPLE);
        error->line = error->column = 1;
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        return 0;
    }
    if (!options->allow_foreign_triple && layout != NULL && *layout != '\0' &&
        strcmp(layout, CVM_LLVM_DATA_LAYOUT) != 0) {
        set_llvm_error(error,
                       "IR DataLayout does not match RArchM64 LLVM DataLayout v1");
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        return 0;
    }
    const char *effective_layout = layout != NULL && *layout != '\0'
                                       ? layout : CVM_LLVM_DATA_LAYOUT;
    Generator generator;
    memset(&generator, 0, sizeof(generator));
    generator.error = error;
    generator.target_data = LLVMCreateTargetData(effective_layout);
    if (generator.target_data == NULL) {
        set_llvm_error(error, "cannot create LLVM target data");
        LLVMDisposeModule(module);
        LLVMContextDispose(context);
        return 0;
    }
    int okay = emit_module(&generator, module);
    if (okay && reserve_text(&generator.output, 0)) {
        generator.output.data[generator.output.length] = '\0';
        *assembly = generator.output.data;
    } else {
        if (!generator.failed) set_llvm_error(error, "out of memory");
        free(generator.output.data);
        okay = 0;
    }
    LLVMDisposeTargetData(generator.target_data);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    return okay;
}
