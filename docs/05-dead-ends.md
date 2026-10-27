# 05 · 走过的死路

这一页的价值可能比前面几页都大 —— 已经替你花掉了几个小时。

---

## `su` —— 原版实现，无解

```
$ su -c id
uid 10055 not allowed to su
```

`su` 的判定在 `platform/system/extras/su/su.c`（**注意不是** `system/core/su`），
全文只有一处检查：

```c
if (getuid() != AID_ROOT && getuid() != AID_SHELL) {
    fprintf(stderr, "su: uid %d not allowed to su\n", getuid());
    return 1;
}
```

**没有** `su.d/`、**没有** `su.allowlist`、**没有**编译期宏。
车机上的报错与该格式串逐字吻合 ⇒ 这就是原版，没被加白名单。

而且即使是 uid 2000 也只能「过检查」，过不了提权：
`/system/xbin/su` 是 `0755`（**没有 setuid 位**）+ `/system` nosuid
⇒ `setuid(0)` 必然 `EPERM`。

**`su` 对这台机器上的任何 uid 都是死路。**

---

## setuid —— 被 `no_new_privs` 挡掉

`rootfs /` 是 `ro` 但**没有 nosuid**，所以理论上
`/sbin/rootsh`（4755 root）应该能给 root。实测：

```
$ /sbin/rootsh -c id
uid=10055(u0_a55) ...        ← 还是 10055！
```

原因：Android 给 app 进程设了 **`PR_SET_NO_NEW_PRIVS`**，
内核在 `execve` 时直接忽略 setuid 位。

> 3.10 内核的 `/proc/<pid>/status` 里**没有** `NoNewPrivs` 字段
> （那是 4.9 才加的），所以看不到，只能靠行为判定。

**但它不是全局的**：从 adbd 的 shell（由 init 启动，无 `no_new_privs`）
执行 `/sbin/rootsh` 就能拿到 root。如果哪天能拿到一个 uid 2000 的 shell，
这条路是通的。

---

## `adbd` —— 厂商没编译 `ALLOW_ADBD_ROOT`

`ro.secure=0` + eng 构建，看上去 `adbd` 就该以 root 跑。AOSP 源码：

```c
static int should_drop_privileges() {
#ifndef ALLOW_ADBD_ROOT
    return 1;                       // ← 本机走的是这条
#else
    property_get("ro.secure", value, "1");
    if (strcmp(value, "1") == 0) { ... }
    return secure;
#endif
}
```

实测启动后：

```
$ ps | grep adbd
3875  uid=2000  /sbin/adbd          ← shell，不是 root
```

所以 `setprop service.adb.root 1`、`ro.secure=0` 都不起作用。
**这台机器的 adbd 只能给 uid 2000。**

### 附带：`/system/xbin/adb` 客户端认证失败

机器上居然有真的 adb 客户端（141 KB）。
用 PKCS#1 私钥放 `$HOME/.android/adbkey` 后：

```
$ adb connect 127.0.0.1:5555
connected to 127.0.0.1:5555
$ adb devices
emulator-5554     offline        ← 端口 5555 被当成模拟器扫描到
127.0.0.1:5555    offline
```

握手停在 `offline`，`ADB_TRACE=all` 只给出一个 `FAIL`。
**没有继续钻。** 如果 `ALLOW_ADBD_ROOT` 没开，即使认证成功也只有 uid 2000，
价值不足以继续投入。

---

## `run-as` —— 三道关全过不去

`/system/bin/run-as` 是 `-rwsr-x--x root system`（有 setuid），但：

```c
// run-as.c:120
if (myuid != AID_SHELL && myuid != AID_ROOT)
    panic("only 'shell' or 'root' users can run this program\n");

// run-as.c:132
if (info.uid < AID_APP) panic(...);          // 目标必须是普通 app

// run-as.c:138
if (!info.isDebuggable) panic(...);          // 目标必须 debuggable
```

我们是 uid 10055（过不了第一关），而且 `/system` nosuid 让它的 setuid 位失效。

---

## `/dev/console` —— 实测 0600

AOSP 4.4.2 里 `console` 服务是 `user shell`（uid 2000），
`on property:ro.debuggable=1 → start console`，而本机 `ro.debuggable=1`
且 `init.svc.console=running`。

看上去很美 —— 但实测：

```
$ ls -l /dev/console
crw------- root root 5, 1        ← 0600
```

uid 2000 打不开。**死。**

---

## `/data/local.prop` —— `/data` 是 0771

AOSP 4.4.2 `init.rc`：

```
mkdir /data 0771 system system
chmod 0771 /data
```

普通 app 在 `/data` 下**不能创建文件**。只有自己的私有目录可写。

---

## `property_service` 写属性 —— 按 uid 鉴权，不是按文件权限

`/dev/socket/property_service` 是 `0666`，但服务端会检查 peer uid：

```c
if (!strncmp(name, "ro.", 3)) name += 3;
if (uid == 0) return check_mac_perms(name, sctx);
app_id = multiuser_get_app_id(uid);
for (i = 0; property_perms[i].prefix; i++)
    if (strncmp(prefix, name, strlen(prefix)) == 0)
        if (uid == perms[i].uid || gid == perms[i].gid) return ...;
return 0;                        // 没有前缀匹配 ⇒ 拒绝
```

`check_mac_perms()` 第一行：

```c
if (is_selinux_enabled() <= 0) return 1;    // 本机 SELinux 关闭 ⇒ MAC 全放行
```

所以只剩 uid 表。结论：

| 属性 | 需要 | 我们 |
|---|---|---|
| `service.adb.tcp.port` | uid 2000 | ❌ |
| `persist.adb.tcp.port` | **无任何前缀匹配 ⇒ 谁都写不了**（非 root） | ❌ |
| `ctl.start <svc>` | uid 1000 或 0 | ❌ |

`/data/property/persist.*` 那条路也死：property_service 强制要求
`uid==0 && gid==0 && (mode & 077)==0 && nlink==1`。

---

## zygote socket —— 双死

`/dev/socket/zygote` 是 `0660 root system`，且 zygote 会对 peer 做 uid 校验。
就算能连上，协议层也不接受任意命令。

---

## `udvrservice` —— 看着像命令执行器，其实不是

这个 root 进程（`/system/bin/udvrservice`，42 KB）里有一个
`@udvrservice_is_the_best_one.sock` 抽象 socket，还有 `system()` 调用。

REA + Ghidra 反汇编后：

- `system()` 的格式串是 `chmod 666 /dev/%s`，`%s` 来自 `readdir()` —— **不是命令执行**
- `objdump` 里**没有任何 `BBinder::onTransact`**，只有 `BpUVCService` 客户端代理
  ⇒ 它不提供 Binder 服务端，wire 层无从下手

**线路关闭。**

---

## `gocsdk` —— oneshot，崩溃不重启

第一版攻击目标。改完全量 567,924 字节后：

```
init.svc.gocsdk = stopped
```

原因在 `init/signal_handler.c`：

```c
if ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_RESTART)) {
    svc->flags |= SVC_DISABLED;          // ⇒ 永不重启
}
```

它的 `init.rc` 定义确实是 `oneshot`（8.1 固件的
`init.device.rc` 里明确写着 `service gocsdk … oneshot`）。

**教训**：动手前先 `getprop | grep init.svc` 反查，只挑 `running` 且不是 oneshot 的。

---

## 其它被静态排除的内核 CVE

拿 3.10.65 和修好的 3.10.104 逐文件 diff：

| CVE | 结论 |
|---|---|
| CVE-2016-5195（Dirty COW） | ✅ 存在，实机验证通过 |
| CVE-2015-1805（pipe） | ✅ 存在，零特权可触发，但需要堆布局，工程量高一个数量级 |
| CVE-2014-3153（towelroot） | ❌ `lookup_pi_state` 有 owner 校验，已修 |
| CVE-2015-3636（ping） | ❌ 需要 `CAP_NET_RAW` |
| CVE-2016-0728（keyring） | ⚠️ 存在，但需 `CONFIG_KEYS`，未确认 |

---

## 一句话总结

在这台机器上，**唯一干净的路是 `/dev/tw`**。
`su`、setuid、`run-as`、`/dev/console`、属性服务、zygote socket —— 每一条都
被「nosuid + `no_new_privs` + uid 鉴权」这三件事里的至少一件堵死。
