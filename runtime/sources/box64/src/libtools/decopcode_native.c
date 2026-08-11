#include "decopcode_native.h"

int is_native_write_opcode(uint32_t opcode)
{
    // Load/store pair: STNP/STP have L (bit 22) clear; LDNP/LDP have it set.
    if ((opcode & 0x3a000000u) == 0x28000000u)
        return (opcode & 0x00400000u) == 0;

    // Single-register load/store, including unscaled, pre/post-indexed,
    // register-offset, and unsigned-immediate forms. Stores have L clear.
    const uint32_t single_class = opcode & 0x3b000000u;
    if (single_class == 0x38000000u || single_class == 0x39000000u)
        return (opcode & 0x00400000u) == 0;

    // Cache clean/invalidate by virtual address must take shadPS4's write-side
    // invalidation path when Turnip touches a protected GPU-tracked page.
    const uint32_t cache_maintenance = opcode & 0xffffffe0u;
    if (cache_maintenance == 0xd50b7a20u || // DC CVAC, Xt
        cache_maintenance == 0xd50b7e20u)   // DC CIVAC, Xt
        return 1;

    // DC ZVA zeroes the cache block containing Xt and therefore writes memory.
    if ((opcode & 0xffffffe0u) == 0xd50b7420u)
        return 1;

    return 0;
}
