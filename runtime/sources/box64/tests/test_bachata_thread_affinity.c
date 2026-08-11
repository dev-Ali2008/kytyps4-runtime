#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdio.h>

#include "../src/libtools/bachata_thread_affinity.h"

int main(void) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    CPU_SET(2, &allowed);
    CPU_SET(5, &allowed);
    CPU_SET(7, &allowed);

    assert(!box64_bachata_should_pin_thread(NULL, "NexusRevolution"));
    assert(!box64_bachata_should_pin_thread("0", "NexusRevolution"));
    assert(!box64_bachata_should_pin_thread("1", "Game:Main"));
    assert(box64_bachata_should_pin_thread("1", "NexusRevolution"));
    assert(box64_bachata_highest_allowed_cpu(&allowed) == 7);

    CPU_ZERO(&allowed);
    assert(box64_bachata_highest_allowed_cpu(&allowed) == -1);
    puts("Bachata thread affinity policy passed");
    return 0;
}
