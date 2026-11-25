#include "../mac.h"
const char *greet(const char *);
extern int greet_count;

int main(void)
{
    puts(greet("Windows"));
    puts(greet("again"));
    printf("greet_count=%d\n", greet_count);
    return greet_count == 2 ? 0 : 1;
}
