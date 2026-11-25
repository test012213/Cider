#include "../mac.h"
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf jb;
static void jumper(int v) { longjmp(jb, v); }
static void bye1(void) { puts("atexit 1 (runs last)"); }
static void bye2(void) { puts("atexit 2 (runs first)"); }

int main(int argc, char **argv)
{
    atexit(bye1);
    atexit(bye2);
    char buf[16];
    size_t n = strlcpy(buf, "hello, world!!!!!!", sizeof buf);
    printf("strlcpy: %s %zu\n", buf, n);
    strlcpy(buf, "ab", sizeof buf);
    strlcat(buf, "cd", sizeof buf);
    printf("strlcat: %s\n", buf);
    char *s = strdup("one,two,,three"), *orig = s, *tok;
    while ((tok = strsep(&s, ",")) != NULL) printf("tok[%s] ", tok);
    printf("\n");
    free(orig);
    printf("strtol: %ld %ld %lu\n", strtol("-9000000000", 0, 10), strtol("ff", 0, 16), strtoul("18000000000000000000", 0, 10));
    int arr[] = { 5, 3, 9, 1, 7, 2, 8, 6, 4, 0 };
    qsort(arr, 10, sizeof(int), cmp);
    for (int i = 0; i < 10; i++) printf("%d", arr[i]);
    int key = 7;
    int *hit = bsearch(&key, arr, 10, sizeof(int), cmp);
    printf(" bsearch(7)=%d\n", hit ? (int)(hit - arr) : -1);
    const char *t = "Cider 2 Go";
    for (const char *p = t; *p; p++) putchar(isalpha(*p) ? toupper(*p) : isdigit(*p) ? '#' : '_');
    printf(" space=%d upper=%d\n", isspace(' '), isupper('Q'));
    errno = 0;
    FILE *f = fopen("/definitely/not/here", "r");
    printf("fopen missing: %s errno=%d\n", f ? "opened?!" : "NULL", errno);
    printf("math: %.4f %.4f %.1f %.3f %.1f\n", sin(1.0), cos(1.0), pow(2, 10), sqrt(2), floor(-2.5));
    int r = setjmp(jb);
    if (r == 0) jumper(42);
    printf("longjmp returned %d\n", r);
    time_t epoch = 1700000000;
    struct tm *g = gmtime(&epoch);
    strftime(buf, sizeof buf, "%Y-%m-%d", g);
    printf("gmtime: %s %02d:%02d zone=%s\n", buf, g->tm_hour, g->tm_min, g->tm_zone);
    printf("timegm: %ld\n", (long)timegm(g));
    printf("HOME set: %s\n", getenv("HOME") ? "yes" : "no");
    setenv("CIDER_TEST", "works", 1);
    printf("setenv: %s\n", getenv("CIDER_TEST"));
    int opt, flag = 0;
    char *val = 0;
    char *av[] = { "prog", "-f", "-o", "out.txt", "rest", 0 };
    while ((opt = getopt(5, av, "fo:")) != -1) {
        if (opt == 'f') flag = 1;
        if (opt == 'o') val = optarg;
    }
    printf("getopt: f=%d o=%s next=%s\n", flag, val, av[optind]);
    void *al = 0;
    printf("posix_memalign: %d aligned=%d\n", posix_memalign(&al, 256, 1000), ((unsigned long)al & 255) == 0);
    free(al);
    char *grow = 0;
    for (int i = 1; i <= 5; i++) {
        grow = realloc(grow, i * 1000);
        memset(grow, 'x', i * 1000);
    }
    printf("realloc ok %d\n", grow[4999] == 'x');
    free(grow);
    unsigned long long t0 = mach_absolute_time(), t1 = mach_absolute_time();
    printf("mach_absolute_time monotonic: %d\n", t1 >= t0);
    printf("args:");
    for (int i = 1; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");
    return 3;
}
