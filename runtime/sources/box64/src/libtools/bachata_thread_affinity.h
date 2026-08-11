#ifndef BOX64_BACHATA_THREAD_AFFINITY_H
#define BOX64_BACHATA_THREAD_AFFINITY_H

#include <sched.h>
#include <string.h>

static inline int box64_bachata_should_pin_thread(const char* enabled, const char* name) {
    return enabled && strcmp(enabled, "1") == 0 && name && strcmp(name, "NexusRevolution") == 0;
}

static inline int box64_bachata_highest_allowed_cpu(const cpu_set_t* allowed) {
    if (!allowed) return -1;
    for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu) {
        if (CPU_ISSET(cpu, allowed)) return cpu;
    }
    return -1;
}

#endif
