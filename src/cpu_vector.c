#include "cpu_vector.h"

#include "cpu_internal.h"
#include "isa.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "the VM requires 32-bit float");
_Static_assert(sizeof(double) == 8, "the VM requires 64-bit double");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "the VM requires IEEE-754 binary32 float");
_Static_assert(DBL_MANT_DIG == 53,
               "the VM requires IEEE-754 binary64 double");

typedef enum {
    VECTOR_INTEGER_ADD,
    VECTOR_INTEGER_SUBTRACT,
    VECTOR_INTEGER_MULTIPLY
} VectorIntegerOperation;

typedef enum {
    FLOAT_ADD,
    FLOAT_SUBTRACT,
    FLOAT_MULTIPLY,
    FLOAT_DIVIDE
} FloatingPointOperation;

static int fetch_general_register(CPU *cpu, RAM *ram, uint8_t *reg)
{
    if (!cpu_internal_fetch_u8(cpu, ram, reg)) {
        return 0;
    }
    return *reg < REGISTER_COUNT
               ? 1
               : cpu_internal_illegal_instruction(cpu);
}

static int fetch_vector_register(CPU *cpu, RAM *ram, uint8_t *reg)
{
    if (!cpu_internal_fetch_u8(cpu, ram, reg)) {
        return 0;
    }
    return *reg < VECTOR_REGISTER_COUNT
               ? 1
               : cpu_internal_illegal_instruction(cpu);
}

static int fetch_vector_pair(CPU *cpu,
                             RAM *ram,
                             uint8_t *first,
                             uint8_t *second)
{
    return fetch_vector_register(cpu, ram, first) &&
           fetch_vector_register(cpu, ram, second);
}

static int fetch_vector_triple(CPU *cpu,
                               RAM *ram,
                               uint8_t *destination,
                               uint8_t *left,
                               uint8_t *right)
{
    return fetch_vector_register(cpu, ram, destination) &&
           fetch_vector_register(cpu, ram, left) &&
           fetch_vector_register(cpu, ram, right);
}

static uint64_t read_lane(const CPUVectorRegister *reg,
                          size_t lane,
                          size_t width)
{
    uint64_t value = 0;
    size_t offset = lane * width;
    for (size_t i = 0; i < width; ++i) {
        value |= (uint64_t)reg->bytes[offset + i] << (i * 8);
    }
    return value;
}

static void write_lane(CPUVectorRegister *reg,
                       size_t lane,
                       size_t width,
                       uint64_t value)
{
    size_t offset = lane * width;
    for (size_t i = 0; i < width; ++i) {
        reg->bytes[offset + i] = (uint8_t)(value >> (i * 8));
    }
}

static float read_float32(const CPUVectorRegister *reg, size_t lane)
{
    uint32_t bits = (uint32_t)read_lane(reg, lane, 4);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double read_float64(const CPUVectorRegister *reg, size_t lane)
{
    uint64_t bits = read_lane(reg, lane, 8);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_float32(CPUVectorRegister *reg, size_t lane, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_lane(reg, lane, 4, bits);
}

static void write_float64(CPUVectorRegister *reg, size_t lane, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_lane(reg, lane, 8, bits);
}

static float calculate_float32(float left,
                               float right,
                               FloatingPointOperation operation)
{
    switch (operation) {
        case FLOAT_ADD: return left + right;
        case FLOAT_SUBTRACT: return left - right;
        case FLOAT_MULTIPLY: return left * right;
        case FLOAT_DIVIDE: return left / right;
    }
    return 0.0f;
}

static double calculate_float64(double left,
                                double right,
                                FloatingPointOperation operation)
{
    switch (operation) {
        case FLOAT_ADD: return left + right;
        case FLOAT_SUBTRACT: return left - right;
        case FLOAT_MULTIPLY: return left * right;
        case FLOAT_DIVIDE: return left / right;
    }
    return 0.0;
}

static void update_float32_status(CPU *cpu,
                                  float left,
                                  float right,
                                  float result,
                                  FloatingPointOperation operation)
{
    int divide_by_zero = operation == FLOAT_DIVIDE &&
                         right == 0.0f && left != 0.0f && isfinite(left);
    if (isnan(result)) {
        cpu->fp_status |= CPU_FP_INVALID;
    }
    if (divide_by_zero) {
        cpu->fp_status |= CPU_FP_DIVIDE_BY_ZERO;
    }
    if (!divide_by_zero && isfinite(left) && isfinite(right) &&
        isinf(result)) {
        cpu->fp_status |= CPU_FP_OVERFLOW;
    }
    if (result != 0.0f && fpclassify(result) == FP_SUBNORMAL) {
        cpu->fp_status |= CPU_FP_UNDERFLOW;
    }
}

static void update_float64_status(CPU *cpu,
                                  double left,
                                  double right,
                                  double result,
                                  FloatingPointOperation operation)
{
    int divide_by_zero = operation == FLOAT_DIVIDE &&
                         right == 0.0 && left != 0.0 && isfinite(left);
    if (isnan(result)) {
        cpu->fp_status |= CPU_FP_INVALID;
    }
    if (divide_by_zero) {
        cpu->fp_status |= CPU_FP_DIVIDE_BY_ZERO;
    }
    if (!divide_by_zero && isfinite(left) && isfinite(right) &&
        isinf(result)) {
        cpu->fp_status |= CPU_FP_OVERFLOW;
    }
    if (result != 0.0 && fpclassify(result) == FP_SUBNORMAL) {
        cpu->fp_status |= CPU_FP_UNDERFLOW;
    }
}

static int execute_vload128(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t address;
    uint8_t value[VECTOR_REGISTER_SIZE];
    if (!fetch_vector_register(cpu, ram, &destination) ||
        !fetch_general_register(cpu, ram, &address) ||
        !cpu_internal_vector_read(cpu,
                                  ram,
                                  cpu->registers[address],
                                  value)) {
        return 0;
    }
    memcpy(cpu->vector_registers[destination].bytes,
           value,
           sizeof(value));
    return 1;
}

static int execute_vstore128(CPU *cpu, RAM *ram)
{
    uint8_t address;
    uint8_t source;
    return fetch_general_register(cpu, ram, &address) &&
           fetch_vector_register(cpu, ram, &source) &&
           cpu_internal_vector_write(cpu,
                                     ram,
                                     cpu->registers[address],
                                     cpu->vector_registers[source].bytes);
}

static int execute_vmov(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_pair(cpu, ram, &destination, &source)) {
        return 0;
    }
    cpu->vector_registers[destination] = cpu->vector_registers[source];
    return 1;
}

static int execute_vector_integer(CPU *cpu,
                                  RAM *ram,
                                  size_t width,
                                  VectorIntegerOperation operation)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }

    CPUVectorRegister result = {{0}};
    size_t lanes = VECTOR_REGISTER_SIZE / width;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t left = read_lane(&cpu->vector_registers[left_register],
                                  lane,
                                  width);
        uint64_t right = read_lane(&cpu->vector_registers[right_register],
                                   lane,
                                   width);
        uint64_t value;
        switch (operation) {
            case VECTOR_INTEGER_ADD: value = left + right; break;
            case VECTOR_INTEGER_SUBTRACT: value = left - right; break;
            case VECTOR_INTEGER_MULTIPLY: value = left * right; break;
        }
        write_lane(&result, lane, width, value);
    }
    cpu->vector_registers[destination] = result;
    return 1;
}

#define DEFINE_VECTOR_INTEGER_HANDLER(name, width, operation) \
    static int name(CPU *cpu, RAM *ram)                       \
    {                                                         \
        return execute_vector_integer(cpu, ram, width, operation); \
    }

DEFINE_VECTOR_INTEGER_HANDLER(execute_vadd8, 1, VECTOR_INTEGER_ADD)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vadd16, 2, VECTOR_INTEGER_ADD)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vadd32, 4, VECTOR_INTEGER_ADD)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vadd64, 8, VECTOR_INTEGER_ADD)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vsub8, 1, VECTOR_INTEGER_SUBTRACT)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vsub16, 2, VECTOR_INTEGER_SUBTRACT)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vsub32, 4, VECTOR_INTEGER_SUBTRACT)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vsub64, 8, VECTOR_INTEGER_SUBTRACT)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vmul8, 1, VECTOR_INTEGER_MULTIPLY)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vmul16, 2, VECTOR_INTEGER_MULTIPLY)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vmul32, 4, VECTOR_INTEGER_MULTIPLY)
DEFINE_VECTOR_INTEGER_HANDLER(execute_vmul64, 8, VECTOR_INTEGER_MULTIPLY)

static int execute_vector_bitwise(CPU *cpu, RAM *ram, uint8_t opcode)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }

    CPUVectorRegister result = {{0}};
    for (size_t i = 0; i < VECTOR_REGISTER_SIZE; ++i) {
        uint8_t left = cpu->vector_registers[left_register].bytes[i];
        uint8_t right = cpu->vector_registers[right_register].bytes[i];
        if (opcode == OP_VAND) {
            result.bytes[i] = left & right;
        } else if (opcode == OP_VOR) {
            result.bytes[i] = left | right;
        } else {
            result.bytes[i] = left ^ right;
        }
    }
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_float_load(CPU *cpu, RAM *ram, size_t width)
{
    uint8_t destination;
    uint8_t address;
    uint64_t value;
    if (!fetch_vector_register(cpu, ram, &destination) ||
        !fetch_general_register(cpu, ram, &address) ||
        !cpu_internal_data_read(cpu,
                                ram,
                                cpu->registers[address],
                                width,
                                &value)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    write_lane(&result, 0, width, value);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_fload32(CPU *cpu, RAM *ram)
{
    return execute_float_load(cpu, ram, 4);
}

static int execute_fload64(CPU *cpu, RAM *ram)
{
    return execute_float_load(cpu, ram, 8);
}

static int execute_float_store(CPU *cpu, RAM *ram, size_t width)
{
    uint8_t address;
    uint8_t source;
    if (!fetch_general_register(cpu, ram, &address) ||
        !fetch_vector_register(cpu, ram, &source)) {
        return 0;
    }
    return cpu_internal_data_write(cpu,
                                   ram,
                                   cpu->registers[address],
                                   width,
                                   read_lane(&cpu->vector_registers[source],
                                             0,
                                             width));
}

static int execute_fstore32(CPU *cpu, RAM *ram)
{
    return execute_float_store(cpu, ram, 4);
}

static int execute_fstore64(CPU *cpu, RAM *ram)
{
    return execute_float_store(cpu, ram, 8);
}

static int execute_scalar_float32(CPU *cpu,
                                  RAM *ram,
                                  FloatingPointOperation operation)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }
    float left = read_float32(&cpu->vector_registers[left_register], 0);
    float right = read_float32(&cpu->vector_registers[right_register], 0);
    float value = calculate_float32(left, right, operation);
    update_float32_status(cpu, left, right, value, operation);
    CPUVectorRegister result = {{0}};
    write_float32(&result, 0, value);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_scalar_float64(CPU *cpu,
                                  RAM *ram,
                                  FloatingPointOperation operation)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }
    double left = read_float64(&cpu->vector_registers[left_register], 0);
    double right = read_float64(&cpu->vector_registers[right_register], 0);
    double value = calculate_float64(left, right, operation);
    update_float64_status(cpu, left, right, value, operation);
    CPUVectorRegister result = {{0}};
    write_float64(&result, 0, value);
    cpu->vector_registers[destination] = result;
    return 1;
}

#define DEFINE_SCALAR_FLOAT_HANDLERS(suffix, operation)              \
    static int execute_f##suffix##32(CPU *cpu, RAM *ram)             \
    { return execute_scalar_float32(cpu, ram, operation); }          \
    static int execute_f##suffix##64(CPU *cpu, RAM *ram)             \
    { return execute_scalar_float64(cpu, ram, operation); }

DEFINE_SCALAR_FLOAT_HANDLERS(add, FLOAT_ADD)
DEFINE_SCALAR_FLOAT_HANDLERS(sub, FLOAT_SUBTRACT)
DEFINE_SCALAR_FLOAT_HANDLERS(mul, FLOAT_MULTIPLY)
DEFINE_SCALAR_FLOAT_HANDLERS(div, FLOAT_DIVIDE)

static int execute_float_compare(CPU *cpu, RAM *ram, size_t width)
{
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_pair(cpu, ram, &left_register, &right_register)) {
        return 0;
    }

    cpu->flags &= CPU_FLAG_INTERRUPT_ENABLE;
    if (width == 4) {
        float left = read_float32(&cpu->vector_registers[left_register], 0);
        float right = read_float32(&cpu->vector_registers[right_register], 0);
        if (isnan(left) || isnan(right)) {
            cpu->flags |= CPU_FLAG_UNORDERED;
            cpu->fp_status |= CPU_FP_INVALID;
        } else if (left == right) {
            cpu->flags |= CPU_FLAG_ZERO | CPU_FLAG_CARRY;
        } else if (left < right) {
            cpu->flags |= CPU_FLAG_NEGATIVE;
        } else {
            cpu->flags |= CPU_FLAG_CARRY;
        }
    } else {
        double left = read_float64(&cpu->vector_registers[left_register], 0);
        double right = read_float64(&cpu->vector_registers[right_register], 0);
        if (isnan(left) || isnan(right)) {
            cpu->flags |= CPU_FLAG_UNORDERED;
            cpu->fp_status |= CPU_FP_INVALID;
        } else if (left == right) {
            cpu->flags |= CPU_FLAG_ZERO | CPU_FLAG_CARRY;
        } else if (left < right) {
            cpu->flags |= CPU_FLAG_NEGATIVE;
        } else {
            cpu->flags |= CPU_FLAG_CARRY;
        }
    }
    return 1;
}

static int execute_fcmp32(CPU *cpu, RAM *ram)
{
    return execute_float_compare(cpu, ram, 4);
}

static int execute_fcmp64(CPU *cpu, RAM *ram)
{
    return execute_float_compare(cpu, ram, 8);
}

static int execute_float_sign(CPU *cpu,
                              RAM *ram,
                              size_t width,
                              int absolute)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_pair(cpu, ram, &destination, &source)) {
        return 0;
    }
    uint64_t bits = read_lane(&cpu->vector_registers[source], 0, width);
    uint64_t sign = width == 4
                        ? UINT64_C(0x80000000)
                        : UINT64_C(0x8000000000000000);
    bits = absolute ? bits & ~sign : bits ^ sign;
    CPUVectorRegister result = {{0}};
    write_lane(&result, 0, width, bits);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_fneg32(CPU *cpu, RAM *ram)
{
    return execute_float_sign(cpu, ram, 4, 0);
}

static int execute_fneg64(CPU *cpu, RAM *ram)
{
    return execute_float_sign(cpu, ram, 8, 0);
}

static int execute_fabs32(CPU *cpu, RAM *ram)
{
    return execute_float_sign(cpu, ram, 4, 1);
}

static int execute_fabs64(CPU *cpu, RAM *ram)
{
    return execute_float_sign(cpu, ram, 8, 1);
}

static int execute_i32tof32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_register(cpu, ram, &destination) ||
        !fetch_general_register(cpu, ram, &source)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    write_float32(&result,
                  0,
                  (float)(int32_t)cpu->registers[source]);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_i64tof64(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_register(cpu, ram, &destination) ||
        !fetch_general_register(cpu, ram, &source)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    write_float64(&result,
                  0,
                  (double)(int64_t)cpu->registers[source]);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_f32toi32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_general_register(cpu, ram, &destination) ||
        !fetch_vector_register(cpu, ram, &source)) {
        return 0;
    }
    float value = read_float32(&cpu->vector_registers[source], 0);
    int32_t result;
    if (isnan(value)) {
        result = 0;
        cpu->fp_status |= CPU_FP_INVALID;
    } else if (value >= 2147483648.0f) {
        result = INT32_MAX;
        cpu->fp_status |= CPU_FP_INVALID;
    } else if (value <= -2147483648.0f) {
        result = INT32_MIN;
        if (value < -2147483648.0f) {
            cpu->fp_status |= CPU_FP_INVALID;
        }
    } else {
        result = (int32_t)value;
    }
    cpu->registers[destination] = (uint64_t)(int64_t)result;
    return 1;
}

static int execute_f64toi64(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_general_register(cpu, ram, &destination) ||
        !fetch_vector_register(cpu, ram, &source)) {
        return 0;
    }
    double value = read_float64(&cpu->vector_registers[source], 0);
    int64_t result;
    if (isnan(value)) {
        result = 0;
        cpu->fp_status |= CPU_FP_INVALID;
    } else if (value >= 9223372036854775808.0) {
        result = INT64_MAX;
        cpu->fp_status |= CPU_FP_INVALID;
    } else if (value <= -9223372036854775808.0) {
        result = INT64_MIN;
        if (value < -9223372036854775808.0) {
            cpu->fp_status |= CPU_FP_INVALID;
        }
    } else {
        result = (int64_t)value;
    }
    cpu->registers[destination] = (uint64_t)result;
    return 1;
}

static int execute_vector_float32(CPU *cpu,
                                  RAM *ram,
                                  FloatingPointOperation operation)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    for (size_t lane = 0; lane < 4; ++lane) {
        float left = read_float32(&cpu->vector_registers[left_register], lane);
        float right = read_float32(&cpu->vector_registers[right_register], lane);
        float value = calculate_float32(left, right, operation);
        update_float32_status(cpu, left, right, value, operation);
        write_float32(&result, lane, value);
    }
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_vector_float64(CPU *cpu,
                                  RAM *ram,
                                  FloatingPointOperation operation)
{
    uint8_t destination;
    uint8_t left_register;
    uint8_t right_register;
    if (!fetch_vector_triple(cpu,
                             ram,
                             &destination,
                             &left_register,
                             &right_register)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    for (size_t lane = 0; lane < 2; ++lane) {
        double left = read_float64(&cpu->vector_registers[left_register], lane);
        double right = read_float64(&cpu->vector_registers[right_register], lane);
        double value = calculate_float64(left, right, operation);
        update_float64_status(cpu, left, right, value, operation);
        write_float64(&result, lane, value);
    }
    cpu->vector_registers[destination] = result;
    return 1;
}

#define DEFINE_VECTOR_FLOAT_HANDLERS(suffix, operation)               \
    static int execute_vf##suffix##32(CPU *cpu, RAM *ram)             \
    { return execute_vector_float32(cpu, ram, operation); }           \
    static int execute_vf##suffix##64(CPU *cpu, RAM *ram)             \
    { return execute_vector_float64(cpu, ram, operation); }

DEFINE_VECTOR_FLOAT_HANDLERS(add, FLOAT_ADD)
DEFINE_VECTOR_FLOAT_HANDLERS(sub, FLOAT_SUBTRACT)
DEFINE_VECTOR_FLOAT_HANDLERS(mul, FLOAT_MULTIPLY)
DEFINE_VECTOR_FLOAT_HANDLERS(div, FLOAT_DIVIDE)

static int execute_f32tof64(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_pair(cpu, ram, &destination, &source)) {
        return 0;
    }
    CPUVectorRegister result = {{0}};
    write_float64(&result,
                  0,
                  (double)read_float32(&cpu->vector_registers[source], 0));
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_f64tof32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_vector_pair(cpu, ram, &destination, &source)) {
        return 0;
    }
    double input = read_float64(&cpu->vector_registers[source], 0);
    float value = (float)input;
    if (isfinite(input) && isinf(value)) {
        cpu->fp_status |= CPU_FP_OVERFLOW;
    }
    if (value != 0.0f && fpclassify(value) == FP_SUBNORMAL) {
        cpu->fp_status |= CPU_FP_UNDERFLOW;
    }
    CPUVectorRegister result = {{0}};
    write_float32(&result, 0, value);
    cpu->vector_registers[destination] = result;
    return 1;
}

static int execute_getfpstatus(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_general_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->fp_status;
    return 1;
}

static int execute_clearfpstatus(CPU *cpu, RAM *ram)
{
    (void)ram;
    cpu->fp_status = 0;
    return 1;
}

typedef int (*VectorOpcodeHandler)(CPU *cpu, RAM *ram);

static const VectorOpcodeHandler VECTOR_HANDLERS[] = {
    [OP_VLOAD128 - OP_VECTOR_FIRST] = execute_vload128,
    [OP_VSTORE128 - OP_VECTOR_FIRST] = execute_vstore128,
    [OP_VMOV - OP_VECTOR_FIRST] = execute_vmov,
    [OP_VADD8 - OP_VECTOR_FIRST] = execute_vadd8,
    [OP_VADD16 - OP_VECTOR_FIRST] = execute_vadd16,
    [OP_VADD32 - OP_VECTOR_FIRST] = execute_vadd32,
    [OP_VADD64 - OP_VECTOR_FIRST] = execute_vadd64,
    [OP_VSUB8 - OP_VECTOR_FIRST] = execute_vsub8,
    [OP_VSUB16 - OP_VECTOR_FIRST] = execute_vsub16,
    [OP_VSUB32 - OP_VECTOR_FIRST] = execute_vsub32,
    [OP_VSUB64 - OP_VECTOR_FIRST] = execute_vsub64,
    [OP_VMUL8 - OP_VECTOR_FIRST] = execute_vmul8,
    [OP_VMUL16 - OP_VECTOR_FIRST] = execute_vmul16,
    [OP_VMUL32 - OP_VECTOR_FIRST] = execute_vmul32,
    [OP_VMUL64 - OP_VECTOR_FIRST] = execute_vmul64,
    [OP_VAND - OP_VECTOR_FIRST] = NULL,
    [OP_VOR - OP_VECTOR_FIRST] = NULL,
    [OP_VXOR - OP_VECTOR_FIRST] = NULL,
    [OP_FLOAD32 - OP_VECTOR_FIRST] = execute_fload32,
    [OP_FLOAD64 - OP_VECTOR_FIRST] = execute_fload64,
    [OP_FSTORE32 - OP_VECTOR_FIRST] = execute_fstore32,
    [OP_FSTORE64 - OP_VECTOR_FIRST] = execute_fstore64,
    [OP_FADD32 - OP_VECTOR_FIRST] = execute_fadd32,
    [OP_FADD64 - OP_VECTOR_FIRST] = execute_fadd64,
    [OP_FSUB32 - OP_VECTOR_FIRST] = execute_fsub32,
    [OP_FSUB64 - OP_VECTOR_FIRST] = execute_fsub64,
    [OP_FMUL32 - OP_VECTOR_FIRST] = execute_fmul32,
    [OP_FMUL64 - OP_VECTOR_FIRST] = execute_fmul64,
    [OP_FDIV32 - OP_VECTOR_FIRST] = execute_fdiv32,
    [OP_FDIV64 - OP_VECTOR_FIRST] = execute_fdiv64,
    [OP_FCMP32 - OP_VECTOR_FIRST] = execute_fcmp32,
    [OP_FCMP64 - OP_VECTOR_FIRST] = execute_fcmp64,
    [OP_FNEG32 - OP_VECTOR_FIRST] = execute_fneg32,
    [OP_FNEG64 - OP_VECTOR_FIRST] = execute_fneg64,
    [OP_FABS32 - OP_VECTOR_FIRST] = execute_fabs32,
    [OP_FABS64 - OP_VECTOR_FIRST] = execute_fabs64,
    [OP_I32TOF32 - OP_VECTOR_FIRST] = execute_i32tof32,
    [OP_I64TOF64 - OP_VECTOR_FIRST] = execute_i64tof64,
    [OP_F32TOI32 - OP_VECTOR_FIRST] = execute_f32toi32,
    [OP_F64TOI64 - OP_VECTOR_FIRST] = execute_f64toi64,
    [OP_VFADD32 - OP_VECTOR_FIRST] = execute_vfadd32,
    [OP_VFADD64 - OP_VECTOR_FIRST] = execute_vfadd64,
    [OP_VFSUB32 - OP_VECTOR_FIRST] = execute_vfsub32,
    [OP_VFSUB64 - OP_VECTOR_FIRST] = execute_vfsub64,
    [OP_VFMUL32 - OP_VECTOR_FIRST] = execute_vfmul32,
    [OP_VFMUL64 - OP_VECTOR_FIRST] = execute_vfmul64,
    [OP_VFDIV32 - OP_VECTOR_FIRST] = execute_vfdiv32,
    [OP_VFDIV64 - OP_VECTOR_FIRST] = execute_vfdiv64,
    [OP_F32TOF64 - OP_VECTOR_FIRST] = execute_f32tof64,
    [OP_F64TOF32 - OP_VECTOR_FIRST] = execute_f64tof32,
    [OP_GETFPSTATUS - OP_VECTOR_FIRST] = execute_getfpstatus,
    [OP_CLEARFPSTATUS - OP_VECTOR_FIRST] = execute_clearfpstatus
};

int cpu_vector_opcode(uint8_t opcode)
{
    return opcode >= OP_VECTOR_FIRST && opcode <= OP_VECTOR_LAST;
}

int cpu_vector_execute(CPU *cpu, RAM *ram, uint8_t opcode)
{
    if (!cpu_vector_opcode(opcode)) {
        return cpu_internal_illegal_instruction(cpu);
    }
    if (opcode == OP_VAND || opcode == OP_VOR || opcode == OP_VXOR) {
        return execute_vector_bitwise(cpu, ram, opcode);
    }
    VectorOpcodeHandler handler = VECTOR_HANDLERS[opcode - OP_VECTOR_FIRST];
    return handler != NULL
               ? handler(cpu, ram)
               : cpu_internal_illegal_instruction(cpu);
}
