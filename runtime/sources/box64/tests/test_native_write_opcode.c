#include <stdint.h>
#include <stdio.h>

#include "../src/libtools/decopcode_native.h"

typedef struct {
    uint32_t opcode;
    int expected_write;
    const char* name;
} opcode_case_t;

int main(void)
{
    static const opcode_case_t cases[] = {
        {0xad018c62, 1, "stp q2, q3, [x3, #0x30]"},
        {0x3d800000, 1, "str q0, [x0]"},
        {0xad428420, 0, "ldp q0, q1, [x1, #0x50]"},
        {0x3dc00023, 0, "ldr q3, [x1]"},
        {0xd50b7423, 1, "dc zva, x3"},
        {0xd50b7a20, 1, "dc cvac, x0"},
        {0xd50b7a27, 1, "dc cvac, x7"},
        {0xd50b7e20, 1, "dc civac, x0"},
        {0xd50b7e33, 1, "dc civac, x19"},
        {0xd5087640, 0, "dc isw, x0"},
        {0xd65f03c0, 0, "ret"},
    };

    int failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const int actual = is_native_write_opcode(cases[i].opcode);
        if (actual != cases[i].expected_write) {
            fprintf(stderr, "%s: expected write=%d, got %d (opcode=%#x)\n", cases[i].name,
                    cases[i].expected_write, actual, cases[i].opcode);
            ++failures;
        }
    }
    return failures != 0;
}
