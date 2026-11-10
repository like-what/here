#include "custom_ops.h"
#include "io.h"

#include <stdint.h>

static uint64_t pack_lanes(const uint8_t lane[8]) {
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        result |= (uint64_t)lane[i] << (8 * i);
    }
    return result;
}

static uint64_t golden_padd8(uint64_t a, uint64_t b) {
    uint8_t lane[8];
    for (int i = 0; i < 8; i++) {
        lane[i] = (uint8_t)((a >> (8 * i)) + (b >> (8 * i)));
    }
    return pack_lanes(lane);
}

static uint64_t golden_psub8(uint64_t a, uint64_t b) {
    uint8_t lane[8];
    for (int i = 0; i < 8; i++) {
        lane[i] = (uint8_t)((a >> (8 * i)) - (b >> (8 * i)));
    }
    return pack_lanes(lane);
}

static uint64_t golden_abs64(uint64_t a) {
    int64_t signed_value = (int64_t)a;
    if (signed_value >= 0) {
        return (uint64_t)signed_value;
    }
    return (uint64_t)(-(signed_value + 1)) + 1;
}

typedef struct {
    uint64_t a;
    uint64_t b;
} PairCase;

static const PairCase padd8_cases[] = {
    {0x01020304, 0x05060708},
    {0xFF01FE02, 0x01FF02FE},
    {0x80808080, 0x80808080},
    {0x00000000, 0x00000000},
    {0xFFFFFFFF, 0xFFFFFFFF},
    {0x12345678, 0x87654321},
    {0xFFFFFFFFFFFFFFFFULL, 0x0101010101010101ULL},
    {0xFF01FE02FF01FE02ULL, 0x01FF02FE01FF02FEULL},
};

static const PairCase psub8_cases[] = {
    {0x01020304, 0x00010101},
    {0x00000100, 0x00000001},
    {0x00000000, 0x00000000},
    {0xFFFFFFFFFFFFFFFFULL, 0x0101010101010101ULL},
    {0x0102030405060708ULL, 0x0001010100010101ULL},
};

static const uint64_t abs_cases[] = {
    0,
    42,
    (uint64_t)(int64_t)-1,
    (uint64_t)(int64_t)-42,
    (uint64_t)INT64_MIN,
    0x7FFFFFFFFFFFFFFFULL,
};

static int check_pair(const char *name, const PairCase *cases, int total,
                      uint64_t (*hw)(uint64_t, uint64_t),
                      uint64_t (*golden)(uint64_t, uint64_t)) {
    int passed = 0;
    for (int i = 0; i < total; i++) {
        uint64_t hw_result = hw(cases[i].a, cases[i].b);
        uint64_t sw_result = golden(cases[i].a, cases[i].b);
        if (hw_result == sw_result) {
            printf("[PASS] %s %d: a=%llx, b=%llx -> res=%llx\n", name, i, cases[i].a,
                   cases[i].b, hw_result);
            passed++;
        } else {
            printf("[FAIL] %s %d: a=%llx, b=%llx -> HW=%llx, SW=%llx\n", name, i,
                   cases[i].a, cases[i].b, hw_result, sw_result);
        }
    }
    return passed;
}

int main(void) {
    printf("=== Starting Custom Instruction Backend Verification ===\n");

    int padd8_total = (int)(sizeof(padd8_cases) / sizeof(padd8_cases[0]));
    int psub8_total = (int)(sizeof(psub8_cases) / sizeof(psub8_cases[0]));
    int abs_total = (int)(sizeof(abs_cases) / sizeof(abs_cases[0]));
    int passed = 0;

    passed += check_pair("padd8", padd8_cases, padd8_total, padd8, golden_padd8);
    passed += check_pair("psub8", psub8_cases, psub8_total, psub8, golden_psub8);

    for (int i = 0; i < abs_total; i++) {
        uint64_t hw_result = abs64(abs_cases[i]);
        uint64_t sw_result = golden_abs64(abs_cases[i]);
        if (hw_result == sw_result) {
            printf("[PASS] abs %d: a=%llx -> res=%llx\n", i, abs_cases[i], hw_result);
            passed++;
        } else {
            printf("[FAIL] abs %d: a=%llx -> HW=%llx, SW=%llx\n", i, abs_cases[i],
                   hw_result, sw_result);
        }
    }

    int total = padd8_total + psub8_total + abs_total;
    if (passed == total) {
        printf("=== ALL %d TESTS PASSED SUCCESSFULLY! ===\n", total);
        pass();
    } else {
        printf("=== VERIFICATION FAILED! (%d/%d passed) ===\n", passed, total);
        fail();
    }
    return 0;
}
