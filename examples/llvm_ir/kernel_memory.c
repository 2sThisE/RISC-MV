typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

struct Record {
    uint8_t tag;
    uint32_t count;
    uint64_t values[3];
};

volatile uint64_t cvm_kernel_result;

uint64_t cvm_kernel_memory_demo(void)
{
    struct Record record;
    record.tag = 7;
    record.count = 3;
    record.values[0] = 10;
    record.values[1] = 12;
    record.values[2] = 20;

    uint64_t sum = 0;
    for (uint64_t i = 0; i < record.count; ++i) {
        sum += record.values[i];
    }
    cvm_kernel_result = sum;
    return sum;
}
