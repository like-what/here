#include "custom_ops.h"
#include "io.h"

int main(void) {
    int failed = 0;
    uint64_t add = padd8(0x01020304, 0x05060708);
    uint64_t sub = psub8(0x01020304, 0x00010101);
    uint64_t neg = abs64((uint64_t)(int64_t)-42);

    printf("padd8(0x01020304, 0x05060708) = %llx\n", add);
    printf("psub8(0x01020304, 0x00010101) = %llx\n", sub);
    printf("abs(-42) = %lld\n", neg);

    if (add != 0x06080A0C) {
        printf("padd8 mismatch\n");
        failed = 1;
    }
    if (sub != 0x01010203) {
        printf("psub8 mismatch\n");
        failed = 1;
    }
    if (neg != 42) {
        printf("abs mismatch\n");
        failed = 1;
    }

    if (failed) {
        fail();
    }
    pass();
    return 0;
}
