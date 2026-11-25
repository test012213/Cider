/*
 * A miniature macOS SDK header for Cider's test programs.
 *
 * The tests are compiled for x86_64-apple-macos without Apple's SDK, so the
 * declarations, struct layouts, symbol names ($INODE64) and inline macros
 * here mirror what the real macOS headers produce.
 */
#pragma once
typedef unsigned long size_t;
typedef long ssize_t;
typedef long long off_t;
typedef long time_t;
typedef unsigned short mode_t;
typedef struct __sFILE {
    unsigned char *_p;
    int _r, _w;
    short _flags, _file;
    struct { unsigned char *_base; int _size; } _bf;
    int _lbfsize;
    void *_cookie;
    char pad[96];
} FILE;
extern FILE *__stdinp, *__stdoutp, *__stderrp;
#define stdin __stdinp
#define stdout __stdoutp
#define stderr __stderrp
#define NULL ((void *)0)
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_END 2

int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int asprintf(char **, const char *, ...);
int sscanf(const char *, const char *, ...);
int puts(const char *);
int putchar(int);
int fputs(const char *, FILE *);
FILE *fopen(const char *, const char *) __asm("_fopen$DARWIN_EXTSN");
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
char *fgets(char *, int, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int fflush(FILE *);
ssize_t getline(char **, size_t *, FILE *);
int __srget(FILE *);
int __swbuf(int, FILE *);
static inline int __sputc(int c, FILE *p)
{
    if (--p->_w >= 0 || (p->_w >= p->_lbfsize && (char)c != '\n')) return (*p->_p++ = (unsigned char)c);
    return __swbuf(c, p);
}
#define putc_unlocked(x, fp) __sputc(x, fp)
#define getc_unlocked(p) (--(p)->_r < 0 ? __srget(p) : (int)(*(p)->_p++))

void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
int posix_memalign(void **, size_t, size_t);
void exit(int) __attribute__((noreturn));
int atexit(void (*)(void));
char *getenv(const char *);
int setenv(const char *, const char *, int);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
double strtod(const char *, char **);
int atoi(const char *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int rand(void);
void srand(unsigned);

size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *strdup(const char *);
char *strchr(const char *, int);
char *strstr(const char *, const char *);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
char *strsep(char **, const char *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
char *strerror(int);

extern int *__error(void);
#define errno (*__error())

/* <ctype.h>: inline lookups in _DefaultRuneLocale, exactly like the SDK */
typedef struct {
    char __magic[8], __encoding[32];
    void *__sgetrune, *__sputrune;
    int __invalid_rune;
    unsigned int __runetype[256];
    int __maplower[256], __mapupper[256];
} _RuneLocale;
extern _RuneLocale _DefaultRuneLocale;
unsigned long ___runetype(int);
int __maskrune(int, unsigned long);
int __toupper(int);
int __tolower(int);
static inline int __istype(int c, unsigned long f)
{
    return (c >= 0 && c < 128) ? !!(_DefaultRuneLocale.__runetype[c] & f) : !!__maskrune(c, f);
}
#define isalpha(c) __istype((c), 0x100L)
#define isdigit(c) __istype((c), 0x400L)
#define isspace(c) __istype((c), 0x4000L)
#define isupper(c) __istype((c), 0x8000L)
#define toupper(c) __toupper(c)

double sin(double);
double cos(double);
double pow(double, double);
double sqrt(double);
double floor(double);

struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; char *tm_zone; };
time_t time(time_t *);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
size_t strftime(char *, size_t, const char *, const struct tm *);
time_t timegm(struct tm *);

typedef int jmp_buf[37];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));

struct timespec { time_t tv_sec; long tv_nsec; };
struct stat {
    int st_dev;
    mode_t st_mode;
    unsigned short st_nlink;
    unsigned long long st_ino;
    unsigned st_uid, st_gid;
    int st_rdev;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    off_t st_size;
    long long st_blocks;
    int st_blksize;
    unsigned st_flags, st_gen;
    int st_lspare;
    long long st_qspare[2];
};
int stat(const char *, struct stat *) __asm("_stat$INODE64");
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)

struct dirent { unsigned long long d_ino, d_seekoff; unsigned short d_reclen, d_namlen; unsigned char d_type; char d_name[1024]; };
typedef struct DIR DIR;
DIR *opendir(const char *) __asm("_opendir$INODE64");
struct dirent *readdir(DIR *) __asm("_readdir$INODE64");
int closedir(DIR *);

int open(const char *, int, ...);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
int unlink(const char *);
int mkdir(const char *, mode_t);
int rmdir(const char *);
char *getcwd(char *, size_t);
int getopt(int, char *const[], const char *);
extern char *optarg;
extern int optind;
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_TRUNC 0x400

typedef struct _opaque_pthread_t *pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
typedef struct { long sig; char opaque[8]; } pthread_once_t;
#define PTHREAD_MUTEX_INITIALIZER { 0x32AAABA7, { 0 } }
#define PTHREAD_COND_INITIALIZER { 0x3CB0B1BB, { 0 } }
#define PTHREAD_ONCE_INIT { 0x30B1BCBA, { 0 } }
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_once(pthread_once_t *, void (*)(void));
unsigned long long mach_absolute_time(void);
