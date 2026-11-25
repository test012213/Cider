#include "../mac.h"
int main(void)
{
    printf("[%d] [%i] [%u] [%x] [%X] [%o]\n", -42, 42, 3000000000u, 0xbeef, 0xBEEF, 8);
    printf("[%ld] [%lld] [%zu] [%lu]\n", -1234567890123L, 9223372036854775807LL, (size_t)18446744073709551615UL, 4294967296UL);
    printf("[%hhd] [%hd] [%hu]\n", 300, 70000, 70000);
    printf("[%s] [%10s] [%-10s] [%.3s] [%c]\n", "cider", "right", "left", "truncate", 'Z');
    printf("[%f] [%.2f] [%8.3f] [%-8.1f] [%e] [%g] [%g]\n", 3.14159265, 2.71828, -1.5, 9.25, 12345.678, 0.0001, 1e20);
    printf("[%5d] [%-5d] [%05d] [%+d] [% d] [%#x] [%#o]\n", 42, 42, 42, 42, 42, 255, 8);
    printf("[%*d] [%-*d] [%.*f]\n", 6, 7, 6, 7, 3, 1.23456);
    printf("[%p] [%p]\n", (void *)0x1234abcd, (void *)0);
    printf("[%d %f %d %f %d %f %d %f %d %f]\n", 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5);
    printf("[%f %f %f %f %f %f %f %f %f %f]\n", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);
    printf("[%s %s %s %s %s %s %s %s]\n", "a", "b", "c", "d", "e", "f", "g", "h");
    printf("100%%\n");
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%s-%d-%.1f", "snp", 7, 0.25);
    printf("[%s] %d\n", buf, n);
    n = snprintf(buf, 5, "%s", "overflowing");
    printf("[%s] %d\n", buf, n);
    char *dyn = 0;
    n = asprintf(&dyn, "%s has %ld bytes", "asprintf", 123456789012L);
    printf("[%s] %d\n", dyn, n);
    free(dyn);
    int a = 0;
    long b = 0;
    double d = 0;
    char w[16];
    n = sscanf("17 9876543210 2.5 word", "%d %ld %lf %15s", &a, &b, &d, w);
    printf("sscanf=%d %d %ld %.1f %s\n", n, a, b, d, w);
    fprintf(stdout, "fprintf to stdout %d\n", 1);
    return 0;
}
