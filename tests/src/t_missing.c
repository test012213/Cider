#include "../mac.h"
void NSBeep(void);

int main(void)
{
    puts("before");
    fflush(stdout);
    NSBeep();
    puts("after (should not print)");
    return 0;
}
