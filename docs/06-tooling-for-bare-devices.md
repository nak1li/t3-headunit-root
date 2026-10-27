# 06 · 在没有工具的设备上，怎么自己造工具

前五篇都在讲「怎么拿到 root」。这一篇讲拿到 root 之后遇到的另一个问题：
**设备上根本没有工具。**

---

## 脱敏约定

本文所有内网地址均已改写为占位符：

| 占位符 | 含义 |
|---|---|
| `<GW>` | 目标设备（随身 WiFi）的管理后台地址 |
| `<CAR_IP>` | 车机自身的地址 |
| `<PHONE_IP>` | 手机地址 |
| `<HOST_IP>` | 任意被访问的主机地址 |

不出现任何真实 IP、品牌型号或访问凭据。

---

## 背景：只有 toolbox

这台车机跑的是 Android 4.4.2，`/system/bin` 下只有 AOSP 原版的 `toolbox`，
**没有 busybox，没有 toybox**。

实测**没有**这些命令：

```
head  cut  tr  sed  awk  sort  find  wc  xargs
grep 的 -o / -n / -b / -v / -A / -B / -E / -F / -r
base64  md5sum  cksum  sha1sum  xxd  od  hexdump  strings
python  perl  unzip  tar  gzip  openssl  wget  curl  nc
```

实测**有**这些：

```
sh  dd  grep  cat  ls  stat  id  getprop  pm  cp  chmod  chown
mount  umount  dmesg  logcat  ifconfig  ping  wpa_cli  busybox-less
```

这件事的后果是：**很多「空输出」其实是命令不存在，不是权限问题。**
调试时必须把 `/system/bin/sh: xxx: not found` 和 `permission denied`
当成两条完全不同的信息看待 —— 混淆这两者会浪费掉大量时间。

---

## 坑 1：toolbox 的 grep 只有 `-i` 和 `-c`

这是最坑的一个。toolbox 的 `grep` 极简：

```
$ grep --help
Usage: grep [-abcHilmnv] FILE...
```

没有 `-o`（只输出匹配部分）、没有 `-n`（行号）、没有 `-b`（字节偏移）、
没有 `-r`（递归）、没有 `-A/-B`（上下文）。

而你要找的东西往往长这样：

```
/js/set.js —— 整个文件是一整行，压缩过的 JS，几万字节
```

于是 `grep -i ap_isolated set.js` 会把**整行几十 KB** 全吐出来。
在车机终端上滚屏根本没法看，回传主机侧又会被 4G 链路拖死。

更阴的是：**`grep -c` 的输出极易被误当成「匹配数」。**

```
$ grep -c ap_isolated set.js
1
```

这里的 `1` 是**匹配到的行数**，不是匹配次数。压缩 JS 全在一行里，
哪怕 `ap_isolated` 出现了 8 次，`grep -c` 依然输出 `1`。

更进一步，`grep -c` 找不到时输出 `0`，但 shell 里 `grep -c` 的**退出码**
也是 `0`/`1`（找到/没找到），很容易和命令替换 `$(...)` 混在一起看。
**要真实的匹配次数和偏移，只能自己写工具。** → 见后面的 `findstr`。

> 顺带一提：`grep` 的 `-v`（反选）在 toolbox 里是有的，但同样没有 `-o`，
> 所以「grep 出所有不含 X 的行」这种操作在压缩文件上同样不可用。

---

## 坑 2：`psfilter | grep xxx` 会打出 SIGPIPE tombstone

```
$ psfilter | grep com.foo
--------- beginning of main
E/AndroidRuntime( 1234): *** FATAL SIGNAL 13 (SIGPIPE): Process 1234 (psfilter)
E/AndroidRuntime( 1234):     in native code
```

这个看起来像系统崩溃，**其实只是管道噪音**：

- `psfilter` 把结果往管道里写
- `grep` 匹配到第一行后如果提前退出（或者你按了 Ctrl-C），
  写端就收到 `SIGPIPE`
- toolbox 的 `psfilter` 没装 `SIGPIPE` 处理器，默认动作是终止进程
- Android 的 `debuggerd` 捕获到信号，就打了个 tombstone

**排查问题时不要被这个带偏。** 它既不是崩溃、也不会导致系统不稳定，
更不代表你要找的进程有问题。判断依据应该是「你要找的进程在不在」，
而不是「有没有 tombstone」。

规避办法：把输出重定向到文件再 grep，而不是用管道。

```
$ psfilter > /data/local/tmp/ps.txt
$ grep com.foo /data/local/tmp/ps.txt
```

---

## 坑 3：`logcat -d` 的缓冲极小

```
$ logcat -d | tail
```

`-d` 是「dump 后退出」，读的是**内核 ring buffer**。
这台机器上这个 buffer 只有几 KB 的量级，
高频日志（`wpa_supplicant`、内核 wifi 驱动、`SurfaceFlinger`）
**几秒钟就能把它冲干净**。

所以：

- **抓日志必须边触发边抓**，不能「先操作，再回头看」
- 想看某类日志，优先用 `-b` 分缓冲区取，别指望一个 `-d` 拿全：

```
$ logcat -d -b main
$ logcat -d -b system
$ logcat -d -b main -b system
```

- 或者干脆用 `logcat -v time <tag>` 起一个常驻的过滤流，
  通过 root 命令通道把输出重定向到文件

> 实战中踩到的是：`wpa_supplicant` 的扫描日志几秒就没了，
> 导致错过了「为什么连不上热点」的关键几行。
> 后来改成先 `logcat` 挂后台再触发扫描，才抓到。

---

## 坑 4：Python f-string 里的 `$D` 不会被 Python 展开

这个坑和设备无关，纯属脚本写法，但杀伤力很大。

在 Python 里写：

```python
cmd = f"cp /system/bin/debuggerd $D/debuggerd.orig /system/bin/debuggerd"
```

Python **不会**展开 `$D`。它会原样送到车机，由车机上的 `sh` 展开。
如果设备上没有定义 `D` 这个变量，`$D` 就是空串，于是命令实际变成：

```
cp /system/bin/debuggerd /debuggerd.orig /system/bin/debuggerd
```

**路径静默地跑偏了。** 而且它**不会报错** —— `cp` 成功退出，
只是写到了别的地方。你以为备份好了，真要用的时候才发现备份是个空壳。

**规则：发往车机的命令，要么写死绝对路径，要么在同一条命令里先定义变量。**

```bash
# 推荐：同一条命令里先定义再引用
D=/data/local/tmp; cp $D/a $D/b

# 或者干脆全程写死绝对路径
cp /data/local/tmp/a /data/local/tmp/b
```

不要把变量定义放在**另一条**命令里 —— root 命令通道
（见 [04-root-channel.md](04-root-channel.md)）每条命令是独立的 shell 会话，
上一条定义的变量下一条就没了。

---

## 工具 1：`httpget` —— 车机上唯一的 HTTP 客户端

车机上没有 `curl`、没有 `wget`，但很多时候你**需要从车机去访问内网设备**
（因为车机是唯一同时能看见车内网络和热点盒子的节点）。

典型场景：想知道随身 WiFi 的后台到底提供了哪些功能开关。
从主机侧连不上它（它在 4G CGNAT 后面），但车机在同一个 WiFi 上。

`src/httpget.c` 是一个只依赖 libc 的最小 HTTP/1.0 客户端：

```
用法：httpget <host> <port> <path> [outfile] [maxbytes]
```

行为：

- 发一个 `GET`，带 `Host` / `User-Agent` / `Connection: close`
- **响应体写进文件**（`outfile`），**状态行打到 stderr**
- 不给 `outfile` 时，响应体直接写到 stdout，字节数打到 stderr
- 默认最多读 4 MB

车机上的用法：

```bash
$ ./httpget <GW> 80 /index.html /data/local/tmp/index.html
HTTP/1.0 302 Found
4096
```

**实现上有一个刻意的取舍**：先把整个响应读进内存，再找 `\r\n\r\n`
来切分响应头和响应体。

```c
/* 先把整个响应读到内存，找到 \r\n\r\n 之后才是 body */
char *buf = malloc(maxbytes);
...
for (long i = 0; i + 3 < total; i++) {
    if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
        hdr_end = i + 4;
        break;
    }
}
```

为什么不做流式？因为要判断 body 边界必须先看到分隔空行，
而空行在哪取决于服务器（有的发 `Content-Length`，有的直接断连）。
一次性读进内存虽然吃 RAM（车机 RAM 够用），但逻辑简单且不会因为
「服务器用 chunked 编码」而算错长度。对这种「抓静态资源看内容」
的用途，完全够用。

### 一个静态链接的坑

```bash
$ arm-linux-gnueabihf-gcc -static -O2 -o httpget src/httpget.c
... ld: warning: Using 'gethostbyname' in statically linked applications
    requires at runtime the shared libraries from the glibc version used for linking
```

这是 glibc 的**正常提示，不是错误**。但它提醒了一件事：
**静态链接的二进制在运行时仍然可能需要 glibc 的动态库**。

解法很简单 —— 优先传点分十进制 IP：

```c
/* 先当点分十进制解析，失败再走 DNS */
if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
    struct hostent *he = gethostbyname(host);
    ...
}
```

传 IP 走 `inet_pton`，完全绕开 DNS，也就绕开了那个运行时依赖。
只有在必须用域名时，才需要在设备上放一份版本匹配的 libc。

---

## 工具 2：`findstr` —— 定位 JS / 二进制里的配置项

坑 1 说「要真实的匹配次数和偏移，只能自己写工具」。`src/findstr.c` 就是那个工具。

```
用法：findstr <file> <needle> [context=200] [maxhits=10]
```

输出：

```
HIT 48213
<...前面 200 字节...>>>ap_isolated<<<...后面 200 字节...>
```

设计要点：

- **打印字节偏移**（`HIT <offset>`），不是行号 —— 压缩 JS 只有一行，行号没意义
- **只打印命中点前后 N 字节**（默认各 200），不会被一行几万字节撑爆
- 不可打印字符转成 `.`（`\r` `\n` `\t` 保留并转义），
  这样二进制文件也能直接看，不会把终端糊满
- 命中处用 `>>> <<<` 包起来
- 退出码：`0` = 至少一处命中，`1` = 没命中，`2` = 用法/IO 错误
  → 可以直接写进 shell 条件判断

车机上的用法：

```bash
$ ./findstr /data/local/tmp/set.js ap_isolated 120
HIT 48213
...>>>SHOW_WIFI_AP_ISOLATED:false<<<...
```

---

## 实战案例：用这两个工具读出随身 WiFi 的固件能力开关表

下面是一次完整的实战复盘。目标是搞清楚「AP 隔离（客户端隔离）
这个功能到底有没有、能不能开」。

**前提**：这个随身 WiFi 的后台地址是 `<GW>`（默认管理地址）。
它自己的静态资源**不需要登录态**就能抓。

### 第 1 步：抓入口页

```bash
$ ./httpget <GW> 80 /index.html /data/local/tmp/index.html
HTTP/1.0 302 Found
```

**注意状态码是 302。** `Location` 头会指回 `/index.html` 本身。
这不是错误，是设备的固件在「没带 Cookie」时统一重定向到入口。
用 `httpget` 的话状态行只打到 stderr，重定向目标得自己从响应里找。

### 第 2 步：抓 main.js 拿路径映射表

```bash
$ ./httpget <GW> 80 /js/main.js /data/local/tmp/main.js
HTTP/1.0 200 OK
```

`main.js` 是 require.js 的配置，里面是**整个后台的路径映射表**：

```js
require.config({
  baseUrl: "/js",
  paths: {
    "com": "com",
    "wifi": "wifi",
    "set": "set",
    ...
  }
});
```

有了这张表就知道后面该抓什么。

### 第 3 步：抓 set.js —— 这就是能力开关表

```bash
$ ./httpget <GW> 80 /js/set.js /data/local/tmp/set.js
HTTP/1.0 200 OK
```

用 `findstr` 定位关键 flag：

```
$ ./findstr set.js SHOW_WIFI_AP_ISOLATED 80
HIT 12045
...>>>SHOW_WIFI_AP_ISOLATED:false<<<...
HIT 20188
...>>>HAS_MULTI_SSID:false<<<...
HIT 30011
...>>>WIFI_SWITCH_SUPPORT:true<<<...
HIT 44870
...>>>MAX_STATION_NUMBER:32<<<...
```

这张表就是**固件自己声明的能力矩阵**：

| Flag | 值 | 含义 |
|---|---|---|
| `SHOW_WIFI_AP_ISOLATED` | `false` | **UI 不显示 AP 隔离开关** |
| `HAS_MULTI_SSID` | `false` | 不支持多 SSID |
| `WIFI_SWITCH_SUPPORT` | `true` | 支持 WiFi 开关 |
| `MAX_STATION_NUMBER` | `32` | 最大接入终端数 32 |

### 第 4 步：抓 wifi.js 看 UI 逻辑

```
$ ./findstr wifi.js apIsolatedCheckbox 100
HIT 8421
...>>>$('#apIsolatedCheckbox')<<<...
HIT 9990
...>>>function showIsolated(...)<<<...
```

UI 层确实有 `#apIsolatedCheckbox` 这个元素的**处理逻辑**，
但它被 `SHOW_WIFI_AP_ISOLATED` 这个 flag 挡在门外。

### 第 5 步：抓 com.js 看 CGI 参数名

```
$ ./findstr com.js NoForwarding 100
HIT 5521
...>>>NoForwarding<<<...
HIT 6677
...>>>SET_WIFI_SSID1_SETTINGS<<<...
```

**这是最关键的一条。** 底层参数叫 `NoForwarding`
（注意语义是反的：`NoForwarding=1` 表示**禁止**转发，即**开启**隔离）。

### 第 6 步：抓模板，验证「不是没找到，是压根没渲染」

```bash
$ ./httpget <GW> 80 /subpg/wifi_main.html /data/local/tmp/wifi_main.html
HTTP/1.0 200 OK
```

```
$ ./findstr wifi_main.html apIsolated 100
NO_MATCH (文件 18422 字节)
```

模板里**根本没有**隔离那个元素。也就是说服务端在渲染时
**根据 flag 直接不输出这一段 DOM**。

### 结论

> **AP 隔离在芯片层是支持的**（底层参数 `NoForwarding` 就在 CGI 里），
> 但产品层 `SHOW_WIFI_AP_ISOLATED:false` 把 UI 关掉了，
> 服务端连模板片段都不渲染。
>
> **所以「界面上找不到开关」不是用户没找到，是压根没渲染。**

要改它，需要：

1. **登录态**（会话 Cookie）
2. **完整字段** —— 只发一个字段，后端会把 SSID/密码等其它字段写空，
   结果是**直接断网**，配置全部丢失

### 免登录 ≠ 无鉴权

这台设备上还有一类 CGI 读接口是免登录的：

```
/goform/goform_get_cmd_process?cmd=Language
```

可以随便读。但是 —— **WiFi 配置项读不到**，请求会返回空串。

所以规律是：

| 类型 | 免登录 | 说明 |
|---|---|---|
| 静态资源（html/js/css/img） | ✅ | 页面骨架和前端逻辑 |
| `goform_get_cmd_process?cmd=<非配置项>` | ✅ | 语言、状态这类 |
| **WiFi 配置项（SSID/密码/信道/隔离）** | ❌ | 返回空串，需要登录态 |
| `goform_set_cmd_process`（写） | ❌ | 需要登录态 + 完整字段 |

**「静态资源能匿名抓」不代表「这个设备没有鉴权」。**
配置类参数一律要登录态。

---

## 小结：给裸设备造工具的通用套路

1. **先确认命令存不存在**，别把 `not found` 当成 `permission denied`
2. **需要 `grep -o/-n/-b` 就自己写**，几十行 C，比在设备上凑合快得多
3. **需要 HTTP 就自己写**，静态链接 + 传 IP，绕开 DNS 的运行时依赖
4. **管道注意 SIGPIPE**，重定向到文件比管道稳
5. **日志边触发边抓**，`-d` 是事后dump，缓冲小得超乎想象
6. **变量在同一条命令里定义**，别指望跨命令保留

工具本身都很小（`httpget` 静态后约 550 KB，`findstr` 约 420 KB），
用 root 通道推过去就能用 —— 详见
[04-root-channel.md](04-root-channel.md)。
