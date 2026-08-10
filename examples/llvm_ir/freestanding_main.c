#include <stddef.h>
#include <string.h>

int main(void)
{
    unsigned char source[4];
    unsigned char destination[4];
    source[0] = 7;
    source[1] = 11;
    source[2] = 19;
    source[3] = 42;
    memcpy(destination, source, sizeof(source));
    return destination[3] == 42 ? 0 : 1;
}
