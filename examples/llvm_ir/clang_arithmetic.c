__attribute__((noinline)) long add(long left, long right)
{
    return left + right;
}

long checked_add(long left, long right)
{
    long sum = add(left, right);
    if (sum == 42) {
        return sum;
    }
    return 0;
}
