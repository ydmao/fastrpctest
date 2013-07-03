#ifndef CPU_HH
#define CPU_HH 1

#include "rpc_common/compiler.hh"
#include <sched.h>
#include <unistd.h>

inline void pin(int n) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(n, &cs);
    int r = sched_setaffinity(0, sizeof(cs), &cs);
    mandatory_assert(r == 0);
}

inline int ncore() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

#endif
