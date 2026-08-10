long compiler_demo_result;

long sum_to(long value)
{
    long sum = 0;
    while (value > 0) {
        sum = sum + value;
        value = value - 1;
    }
    return sum;
}

long compiler_demo_main(void)
{
    long result = sum_to(10);
    if (result == 55) {
        compiler_demo_result = result;
        return 0;
    }
    return 1;
}
