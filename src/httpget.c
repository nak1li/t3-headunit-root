/*
 * httpget —— 最小的 HTTP/1.0 抓取工具（静态链接，车机上没有 curl/wget）。
 *
 * 用法：
 *   httpget <host> <port> <path> [outfile] [maxbytes]
 *
 * 只做一件事：TCP 连上去，发一个 GET，把响应体写进文件。
 * 响应头里的状态码打到 stderr。
 *
 * 用途：从车机上去抓随身 WiFi / 路由器 / 摄像头这类内网设备的管理页面，
 *      好让主机侧能分析它的功能菜单（车机上没有任何 HTTP 客户端）。
 *
 * 编译（静态链接，设备上 glibc 版本不一定配得上）：
 *   arm-linux-gnueabihf-gcc -static -O2 -o httpget src/httpget.c
 *
 * 注意：静态链接下用 gethostbyname 会有一个 ld warning
 *   "Using 'gethostbyname' in statically linked applications requires at
 *    runtime the shared libraries from the glibc version used for linking"
 * 这是 glibc 的正常提示，不是错误。实际使用时**优先传点分十进制 IP**，
 * 那样会走 inet_pton 完全绕开 DNS，静态二进制也就真的自包含了。
 * 只有必须用域名时才需要在设备上放一份匹配版本的 libc。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int write_all(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return off;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <path> [outfile] [maxbytes]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *path = argv[3];
    const char *outfile = (argc > 4) ? argv[4] : NULL;
    long maxbytes = (argc > 5) ? atol(argv[5]) : (4L * 1024 * 1024);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);

    /* 先当点分十进制解析，失败再走 DNS */
    if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "httpget: 无法解析 %s\n", host);
            return 3;
        }
        memcpy(&sin.sin_addr, he->h_addr, 4);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 3; }

    struct timeval tv;
    tv.tv_sec = 8; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        fprintf(stderr, "httpget: connect %s:%d 失败: %s\n", host, port, strerror(errno));
        close(fd);
        return 4;
    }

    char req[1024];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Mozilla/5.0 (httpget)\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);
    if (write_all(fd, req, rl) < 0) { perror("write"); close(fd); return 5; }

    /* 先把整个响应读到内存，找到 \r\n\r\n 之后才是 body */
    char *buf = malloc(maxbytes);
    if (!buf) { fprintf(stderr, "httpget: 内存不足\n"); close(fd); return 6; }

    long total = 0;
    for (;;) {
        if (total >= maxbytes) break;
        int n = (int)read(fd, buf + total, maxbytes - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    if (total <= 0) { fprintf(stderr, "httpget: 没有收到任何数据\n"); free(buf); return 7; }

    /* 打印状态行 */
    char *nl = memchr(buf, '\n', total);
    if (nl) {
        int len = (int)(nl - buf);
        fprintf(stderr, "%-*.*s\n", len, len, buf);
    }

    /* 找分隔空行 */
    long hdr_end = -1;
    for (long i = 0; i + 3 < total; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end < 0) hdr_end = 0;         /* 没有头就全当 body */

    long body_len = total - hdr_end;

    if (outfile) {
        FILE *out = fopen(outfile, "wb");
        if (!out) { perror("fopen"); free(buf); return 8; }
        fwrite(buf + hdr_end, 1, body_len, out);
        fclose(out);
        printf("%ld\n", body_len);
    } else {
        fwrite(buf + hdr_end, 1, body_len, stdout);
        fprintf(stderr, "\n[httpget] body %ld 字节\n", body_len);
    }

    free(buf);
    return 0;
}
