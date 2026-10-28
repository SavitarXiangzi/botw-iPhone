#include "botw_core.h"
#include <cstdio>
#include <cstring>

struct botw_core {
    botw_core_desc desc{};
    uint64_t frame = 0;
    char error[256] = "not running";
};

extern "C" botw_core* botw_core_create(const botw_core_desc* d) {
    if (!d || !d->metal_device) return nullptr;
    auto* c = new botw_core();
    c->desc = *d;
    std::snprintf(c->error, sizeof(c->error), "core stub: no title data linked yet");
    return c;
}

extern "C" void botw_core_destroy(botw_core* c) { delete c; }

extern "C" int botw_core_frame(botw_core* c, double) {
    if (!c) return -1;
    c->frame++;
    return 0;
}

extern "C" uint64_t botw_core_frame_index(const botw_core* c) { return c ? c->frame : 0; }
extern "C" const char* botw_core_last_error(const botw_core* c) { return c ? c->error : "null"; }
