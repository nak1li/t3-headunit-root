/*
 * findstr —— 在文件里找子串，打印字节偏移和上下文窗口。
 *
 * 为什么需要它：Android 4.4 自带的 toolbox grep 只有 -i/-c，不支持 -o/-n/-b，
 * 而目标文件常常是「一整行几十 KB 的压缩 JS」，于是 grep 一命中就把整行吐出来，
 * 完全没法定位。这个工具只输出命中点前后 N 字节，省流量也省眼睛。
 *
 * 用法：
 *   findstr <file> <needle> [context=200] [maxhits=10]
 *
 * 输出：
 *   HIT <offset>
 *   <上下文，命中处用 >>> <<< 标出>
 *
 * 退出码：0 = 至少一处命中，1 = 没命中，2 = 用法/IO 错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *readfile(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = 0;
    fclose(f);
    *out_len = (long)n;
    return buf;
}

/* 把不可打印字符（\r \n \t 除外）换成 '.'，避免把二进制糊满终端 */
static void print_window(const char *buf, long start, long end, long hit_at, long hit_len)
{
    for (long i = start; i < end; i++) {
        if (i == hit_at) {
            fputs(">>>", stdout);
        }
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') { fputs("\\r", stdout); }
        else if (c == '\n') { fputs("\\n", stdout); }
        else if (c == '\t') { fputs("\\t", stdout); }
        else if (c < 32 || c > 126) { putchar('.'); }
        else { putchar(c); }
        if (i == hit_at + hit_len - 1) {
            fputs("<<<", stdout);
        }
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file> <needle> [context=200] [maxhits=10]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *needle = argv[2];
    long ctx = (argc > 3) ? atol(argv[3]) : 200;
    int maxhits = (argc > 4) ? atoi(argv[4]) : 10;
    long nlen = (long)strlen(needle);
    if (nlen == 0) { fprintf(stderr, "findstr: needle 为空\n"); return 2; }

    long len = 0;
    char *buf = readfile(path, &len);
    if (!buf) {
        fprintf(stderr, "findstr: 打不开 %s\n", path);
        return 2;
    }

    int hits = 0;
    for (long i = 0; i + nlen <= len; i++) {
        if (buf[i] != needle[0]) continue;
        if (memcmp(buf + i, needle, nlen) != 0) continue;

        printf("HIT %ld\n", i);
        long s = (i - ctx > 0) ? i - ctx : 0;
        long e = (i + nlen + ctx < len) ? i + nlen + ctx : len;
        print_window(buf, s, e, i, nlen);
        printf("\n");

        if (++hits >= maxhits) break;
        i += nlen - 1;               /* 不重复计数 */
    }

    free(buf);
    if (hits == 0) {
        printf("NO_MATCH (文件 %ld 字节)\n", len);
        return 1;
    }
    return 0;
}
