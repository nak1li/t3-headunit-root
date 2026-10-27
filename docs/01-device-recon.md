# 01 · 设备侦察

所有数据都是实测输出，不是猜测。

## 起点

一个普通第三方 app，`uid 10055`，跑在车内。没有 USB 数据线、没有 U 盘、没有串口。
唯一的通道是 app 主动出向建立到家里的一条 WebSocket 长连接。

先要回答的问题是：**这个 uid 到底能碰什么？**

## 挂载表

```
rootfs  /        rootfs  ro                       ← 注意：没有 nosuid
/dev/block/by-name/system  /system  ext4  ro,nosuid,nodev,relatime
/dev/block/by-name/data    /data    ext4  rw,nosuid,nodev,noatime
```

关键点：

- `/system` 和 `/data` 都是 **nosuid** ⇒ 放在里面的 setuid 位一律失效
- `rootfs /` 是 `ro` 但**没有 nosuid** ⇒ 唯一能让 setuid 生效的地方
- `/data` 的权限是 `0771 system system` ⇒ 普通 app **不能**在 `/data` 下创建文件
  （AOSP 4.4.2 `init.rc` 里 `mkdir /data 0771 system system` + `chmod 0771 /data`）

只有一处可写：app 自己的私有目录 `/data/data/<pkg>/files`。

## 属性

```
ro.secure      = 0
ro.debuggable  = 1
ro.adb.secure  = 1
kptr_restrict  = 2
SELinux        = Disabled
```

`ro.secure=0` + eng 构建，看上去「adbd 应该就是 root」。
**这是个陷阱** —— 见 [05-dead-ends.md](05-dead-ends.md)。

## 进程表（`/proc/*/cmdline` + `/proc/*/status` 枚举）

机器上有 60 多个进程。筛出 `uid=0` 且由 init 托管的：

| 进程 | pid | uid | 说明 |
|---|---|---|---|
| `/sbin/ueventd` | 1128 | 0 | critical |
| `/sbin/healthd` | 1616 | 0 | critical（AOSP 4.4 里 healthd 在 `/sbin`，不在 `/system/bin`） |
| `/system/bin/vold` | 1618 | 0 | 存储卷管理 |
| `/system/bin/loadu2` | 1620 | 0 | oneshot |
| `/system/bin/dtupdate` | 1621 | 0 | oneshot |
| `/system/bin/netd` | 1624 | 0 | 网络 |
| `zygote` | 1627 | 0 | 所有 app 的爹 |
| `/system/bin/gpioservice` | 1632 | 0 | GPIO |
| `/system/bin/securefileserver` | 3134 | 0 | |
| `/system/bin/systemmixservice` | 3135 | 0 | |
| `/system/bin/isomountmanagerservice` | 3137 | 0 | USB ISO 挂载 |
| `/system/bin/debuggerd` | — | 0 | 崩溃处理 |

## 一个决定性的测量：这些服务有多忙？

用 `/proc/<pid>/stat` 的 utime+stime 采样 6 秒：

```
服务                        ticks/6s
vold                             0
udvrservice                      0
systemmixservice                 0
securefileserver                 0
netd                             0
loadu2                           0
isomountmanagerservice           0
healthd                          0
gpioservice                      0
dtupdate                         0
diy12s                           0
debuggerd                        0
```

**全部为 0。** 所有 root 服务都阻塞在 `poll`/`epoll` 里，不执行任何代码。

这个测量直接决定了后面攻击方案的可行性 —— 见
[03-dirtycow-attack.md](03-dirtycow-attack.md)。

## Binder 服务（`service list`，85 个）

厂商自己的几个值得注意：

```
0   softwinner.systemmix   : com.softwinner.ISystemMixService
1   softwinner.isomountmanager : com.softwinner.IISOMountManagerService
2   softwinner.securefile  : com.softwinner.ISecureFileService
74  softwinner.gpio        : com.softwinner.IGpioService
75  com.anwensoft.cardvr.service.IUVCService
76  wceui.m2m.Diy12        : wceui.m2m.IDiy12
84  wceui.m2m.LibU201      : wceui.m2m.ILibU201
```

**注意**：Binder 服务名 ≠ init 服务名。
`isomountmanager` 是 init 名，`softwinner.isomountmanager` 才是 `service call` 用的名字。

## 设备上有什么工具

这台机器只有 `toolbox`，**没有** `head` / `cut` / `tr` / `sed` / `awk` / `find` /
`wc` / `base64` / `xxd` / `od` / `strings` / `unzip` / `openssl`。

可用的只有：`dd` `pm` `getprop` `ls` `cat` `mount` `grep` `cp` `chmod` `stat` `id`。

这件事的后果：**很多「空输出」其实是命令不存在，不是权限问题。**
调试时要注意 `/system/bin/sh: xxx: not found` 这条信息和 `permission denied` 是两回事。

## 结论

从侦察阶段能确定的：

1. **没有现成的 setuid 可用**（`/system` `/data` 都 nosuid）
2. **`su` 是死路**（原版 AOSP 实现 + 无 setuid 位）
3. **必须找内核层或 init 层的入口**
4. 内核是 **3.10.65**，需要查它有没有已知的本地提权漏洞
