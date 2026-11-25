#include "../mac.h"
int main(void)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/cider-test-dir", getenv("TMPDIR"));
    mkdir(dir, 0755);
    char path[1100];
    snprintf(path, sizeof path, "%s/file.txt", dir);
    FILE *f = fopen(path, "w");
    fputs("line one\nline two\n", f);
    fprintf(f, "number %d\n", 3);
    for (const char *p = "unlocked\n"; *p; p++) putc_unlocked(*p, f);
    fclose(f);
    f = fopen(path, "r");
    fseek(f, 0, SEEK_END);
    printf("size via ftell: %ld\n", ftell(f));
    fseek(f, 0, SEEK_SET);
    char *line = 0;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) printf("getline(%zd): %s", len, line);
    fseek(f, 0, SEEK_SET);
    int c, count = 0;
    while ((c = getc_unlocked(f)) != EOF) count++;
    printf("getc_unlocked count: %d\n", count);
    fclose(f);
    free(line);
    struct stat st;
    printf("stat file: %d size=%lld reg=%d\n", stat(path, &st), st.st_size, S_ISREG(st.st_mode));
    printf("stat dir: %d dir=%d\n", stat(dir, &st), S_ISDIR(st.st_mode));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "raw\n", 4);
    close(fd);
    fd = open(path, O_RDONLY);
    char buf[16] = { 0 };
    printf("read: %zd [%.3s]\n", read(fd, buf, sizeof buf), buf);
    close(fd);
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') {
            files++;
            printf("readdir: %s type=%d\n", e->d_name, e->d_type);
        }
    closedir(d);
    printf("unlink: %d rmdir: %d\n", unlink(path), rmdir(dir));
    printf("stat gone: %d\n", stat(path, &st));
    FILE *n = fopen("/dev/null", "w");
    printf("/dev/null: %s\n", n ? "ok" : "fail");
    if (n) fclose(n);
    return files == 1 ? 0 : 1;
}
