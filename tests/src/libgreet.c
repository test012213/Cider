#include "../mac.h"
static int initialized;
__attribute__((constructor)) static void setup(void) { initialized = 1; }
int greet_count = 0;

const char *greet(const char *who)
{
    static char buf[64];
    greet_count++;
    snprintf(buf, sizeof buf, "hello %s from a dylib (ctor ran: %d)", who, initialized);
    return buf;
}
