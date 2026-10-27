# 07 · 无线 CarPlay 在这台车机上到底可行吗

一次完整的技术可行性体检。结论先放前面：

> **无线 CarPlay：不可行（蓝牙是死结）。**
> **有线 CarPlay：不可行（USB 口没电，是硬件问题）。**
> **但 CarPlay 的「网络前提」可以满足** —— 用 iPhone 个人热点做 AP，
> 车机与手机可以做到二层互通、RTT 3~5 ms、0% 丢包。

---

## 脱敏约定

| 占位符 | 含义 |
|---|---|
| `<GW>` | 随身 WiFi 的管理后台地址 |
| `<CAR_IP>` | 车机自身的地址 |
| `<PHONE_IP>` | iPhone 的地址 |
| `<PHONE_GW>` | iPhone 个人热点的网关地址（即 iPhone 自己） |

文中不出现任何真实 IP、品牌型号或访问凭据。

---

## 目标软件：`com.shihab.diplay`

车机上预装的 CarPlay 应用：

| 项 | 值 |
|---|---|
| 包名 | `com.shihab.diplay` |
| uid | `10006` |
| 体积 | 约 18 MB |

### 自带原生库透露的实现方式

它带了 6 个关键的 `.so`，每一个都对应一条实现路径：

| 库 | 说明 |
|---|---|
| `liblocal_hotspot_radio.so` | **自己开热点** —— 不是靠系统 API，是直接操作射频 |
| `liblwip_ncm_relay.so` | 把 USB NCM 隧道**搬到 IP 上**（lwIP 用户态协议栈） |
| `liblegacy_ncm_claim.so` | NCM 隧道的「认领」逻辑（老式 USB 网络） |
| `libusb_host_compat.so` | **裸 libusb 用户态驱动 HCI** —— 蓝牙走 USB dongle |
| `libxcertplay_i2c.so` | **I2C 认证**（Apple 的 MFi 硬件握手） |
| `libxcertplay_chacha.so` | ChaCha20 流加密（CarPlay 链路加密） |

从这组库能读出厂商的完整设计意图：

```
USB 有线 CarPlay  =  USB NCM 隧道  →  liblwip_ncm_relay  →  IP
无线 CarPlay      =  蓝牙 HCI     →  libusb_host_compat  →  IP
认证              =  I2C          →  libxcertplay_i2c
热点              =  本地射频      →  liblocal_hotspot_radio
```

### 关键发现：它自带 `UsbBtBluetoothAdapter`

APK 里带了一个 `UsbBtBluetoothAdapter`，参数是：

```
dongle=none hciUp=false
```

**这意味着：USB 蓝牙适配器对它完全可行。**

也就是说 —— **不要因为「内核里没有 `btusb` 驱动」就判无线 CarPlay 死刑。**
这个应用绕开了 Android 的标准蓝牙栈，用自己的 `libusb_host_compat.so`
直接通过 libusb 访问 USB 蓝牙 dongle，自己实现 HCI。

这是本次体检里最重要的一条：判断「无线 CarPlay 可行性」时，
**不能只看 Android 系统层有什么，要看目标应用自带什么。**

---

## 依赖盘点：齐备的部分

| 依赖 | 状态 | 备注 |
|---|---|---|
| `cdc_ncm` 驱动 | ✅ 在 | 有线 CarPlay 的核心（USB 上的网卡） |
| H.264 硬解 | ✅ 在 | `OMX.allwinner.video.decoder.avc` |
| `/dev/i2c-0`、`/dev/i2c-1`、`/dev/i2c-2` | ✅ 全 0666 | 认证握手要用，权限没问题 |
| 软件 MFi 证书 | ✅ 在 | `files/offline-mfi/certificate.p7b` + `identity.pk8` |
| `hostapd` | ✅ 在（约 322 KB） | 且 `wpa_supplicant` 支持 AP 模式 |

### 证书这一条值得单说

**软件 MFi 证书已经预置在 APK 里**：

```
files/offline-mfi/certificate.p7b
files/offline-mfi/identity.pk8
```

`.p7b` 是 Apple 签发的证书包，`.pk8` 是对应的私钥。

**这意味着这台机器不需要额外的硬件认证芯片**（不需要像某些车机那样外挂
安全 IC 或 SIM/eSIM 做 CarPlay 认证）。只要蓝牙/IP 层能通，认证环节
不需要额外硬件。

从「硬件准备度」角度看，这台车机其实准备得相当好 —— 卡住的不是硬件。

---

## 三个结构性短板

### 短板 1：蓝牙是厂商私有 UART 栈，不是 BlueZ ⚠️ 决定性

这是真正的死结。

| 事实 | 实测 |
|---|---|
| 蓝牙载体 | `/dev/ttyS5`（权限 0666）+ `blink` 进程 |
| Android 蓝牙服务 | `service list` 里**只有** `bluetooth_manager` |
| 缺失的标准服务 | 无 `adapter`、无 `A2DP`、无 `Headset` |
| 蓝牙状态 | **永久卡在 `11`（`STATE_TURNING_ON`）** |
| 日志 | `Fail to bind to IBluetoothHeadset` |

也就是说：

1. 这台机器的蓝牙是**厂商自己写的 UART 栈**（串口 + `blink` 守护进程），
   不是 AOSP 的 BlueZ —— `service list` 里根本没有 `bluetooth`（adapter）服务
2. 状态机永远停在 `STATE_TURNING_ON`，`IBluetoothHeadset` 绑定失败
3. 系统层没有 A2DP、没有 Headset profile

而 **iOS 的无线 CarPlay 依赖蓝牙做初始握手**（用来交换配对凭据、
协商链路能力、之后蓝牙只负责音频/控制通道，视频走 IP）。

**结论：无线 CarPlay 这条路基本断了。** 应用自带的
`UsbBtBluetoothAdapter` 走的是 USB dongle，而这台车机的车规设计
**没有留 USB 蓝牙 dongle 的口**（那个 USB 口连电都没有，见下文）。

> 补充：即使勉强让系统蓝牙栈跑起来，iOS 也要求设备通过 **MFi**
> 和 **iPhone 配对握手**，而这套私有 UART 栈从未被 Apple 认证过。

### 短板 2：Android 4.4 没有 `WifiManager$LocalOnlyHotspotCallback`

热点回调 API `WifiManager$LocalOnlyHotspotCallback` 是
**API 26（Android 8.0）** 才引入的。这台机器是 API 19。

**后果：`liblocal_hotspot_radio.so` 那套「自己开热点」的逻辑，
在系统层没有配套支撑。** 热点只能**手动开**（`wpa_cli` / 设置界面），
不能被应用在运行时按需拉起 —— 这对 CarPlay 的「连接即自动建链」
是硬伤。

### 短板 3：单射频，且没有 DHCP 服务器

**单射频**：`wlan0` 是**唯一**能用的上行。

```
wlan0   → 有 IP，能用
eth0    → 内置 4G 模块，但没拿到 IP
eth1    → 同上
```

所以：**车机一旦切到 AP 模式，自己的上行就断了。**
它没法一边做 AP 一边还能上网 —— 这与「CarPlay 需要车机作为热点、
同时又不要求车机自己联网」的场景恰好不冲突，但反过来意味着
车机一旦当 AP，就只能靠手机侧提供网络。

**没有 `dhcpd`**：系统里没有 DHCP 服务器实现。
AP 模式下的客户端**拿不到 IP**，必须在 iPhone 侧手工配静态地址：

```
iPhone → 设置 → 无线局域网 → (i) → IP 配置 → 静态
   IP 地址   <CAR_IP>
   子网掩码  255.255.255.0
   路由器    <PHONE_GW>     ← 或者填 AP 的地址
   DNS       随便填
```

这一步在每次换网络时都要手工做，体验上非常糟糕。

---

## 网络前提：同 WiFi 必须二层互通

CarPlay 走 IP 之后，设备发现靠的是 **mDNS 组播**（`224.0.0.251`，
端口 5353）。

**mDNS 组播在 AP 隔离（客户端隔离）下会被直接掐死。**
两种失败模式，都实测到过：

### 失败模式 1：随身 WiFi 的 AP 客户端隔离

症状非常有代表性：

```
$ ping <PHONE_IP>
ping: unknown host            ← 或者
Destination Host Unreachable
```

而 **ARP 表里的目标 MAC 全是零**：

```
$ cat /proc/net/arp
IP address       HW type     Flags       HW address            Mask     Device
<PHONE_IP>       0x1         0x0         00:00:00:00:00:00     *        wlan0
```

**`Flags = 0x0` + 全零 MAC = ARP 请求发出去了，没人应答。**
因为 AP 隔离禁止客户端之间直接二层通信，
所以手机的 ARP 请求**根本到不了车机**（或者到了也被丢弃）。

> 顺带说：这个「ARP 全零 + flags 0x0」的判据，比「ping 不通」精确得多。
> ping 不通可能是路由问题、防火墙、ICMP 被禁；
> 而 ARP 表全零直接指向二层不通。这台车机正好能直接读 `/proc/net/arp`。
>
> 这也是 [06-tooling-for-bare-devices.md](06-tooling-for-bare-devices.md)
> 那个 `findstr` 工具的用途之一 —— 直接在 `/proc/net/arp` 里
> 定位目标行并打出上下文。

### 失败模式 2：Wi-Fi Direct 对 iOS 无效

有人会想：让车机开 Wi-Fi Direct（`p2p0`），不就不需要 AP 了？

**不行。** iOS **从不实现 Wi-Fi Direct** —— 那是 Android 设备之间
（以及 Android Auto 无线方案）用的，iPhone 用的是自己的
**AWDL（Apple Wireless Direct Link）** 私有协议。

实测在车机上选 Wi-Fi Direct 模式：

```
p2p0: Cannot assign requested address
wpa_state=INACTIVE
```

`p2p0` 根本起不来，因为 `wpa_supplicant` 需要一个对端来完成
P2P 的 GO/Client 协商，而**没有 iPhone 会来应门**。

---

## 可行替代：用 iPhone 个人热点做 AP ✅

这是目前实测**唯一能跑通网络前提**的方案。

### 为什么它可以

**iOS 不会隔离「热点主人」和「接入热点的人」。**

这一点和普通路由器的「AP 隔离」不同：iPhone 的个人热点是**手机自己
当 AP**，而手机**自己就是那台设备**，它不会把自己和接入者互相隔离。
这正好符合 CarPlay 需要的「车机和 iPhone 在同一二层网络里互访」。

### 实测结果

车机连上 iPhone 个人热点后：

| 项 | 实测值 |
|---|---|
| 车机地址 | `<CAR_IP>` |
| 手机（网关）地址 | `<PHONE_GW>` |
| `ping` 手机 | **3/3 通** |
| RTT | **3.4 ~ 4.8 ms** |
| 丢包 | **0%** |

**二层互通达成。** ARP 表里能看到手机的真实 MAC，`flags = 0x2`（已解析）。

### 但仍然不够

网络通了，只解决了 CarPlay 的一半。剩下的卡点依然是：

1. **蓝牙握手**（决定性短板 1）—— 无线 CarPlay 必须有
2. **热点自动拉起**（短板 2）—— API 26+ 才有
3. **DHCP**（短板 3）—— 车机侧没有 `dhcpd`，
   如果由**车机**当 AP（`liblocal_hotspot_radio.so` 的用法），
   手机就得手工配静态地址

如果换用 **有线 CarPlay**，前两个短板自动消失（不需要蓝牙、不需要车机开热点），
但又撞上最后一个 —— USB 口的问题。

---

## 车机 USB 口的真相

这是最后一个必须讲清楚的事实：**车上那个给手机用的 USB 口，
连 VBUS 都没供电。**

实测：

- iPhone 插上去**不提示充电**
- **内核完全不枚举**这个 USB 设备（`/sys/bus/usb/devices` 里没有新条目）
- USB bus 上只有**两个常驻设备**：

```
19d2:0199    ← 一个 Modem（内置 4G 模块）
3322:6655    ← 一个串口/转换设备
```

### 硬件本身是有 USB host 能力的

这不是「车机没有 USB host」，而是**这个口的电路设计有问题**：

| 控制器 | 说明 |
|---|---|
| EHCI `usb1`、`usb2`、`usb5` | 3 个高速 host 控制器 |
| OHCI `usb3`、`usb4`、`usb6` | 3 个全速/低速 host 控制器 |
| `bus5` | 挂着 Mass Storage → 块设备 `sda` |

**一共 6 个 host 控制器**，`bus5` 上的 U 盘能被正常识别成 `sda`。
所以硬件能力是有的。

### 结论

> **是这个 USB 口的电路设计问题，不是软件能救的。**
>
> 有线 CarPlay 无望 —— 除非**改硬件**（给 VBUS 供电、确认 D+/D- 连通）
> 或者**换线**（有些线只有数据没有电源针脚，或反过来）。

同样地，这也解释了为什么**USB 蓝牙 dongle 这条路也走不通**：
`libusb_host_compat.so` 需要一个真正被枚举的 USB 设备，
而车上的口连 5V 都没有。

---

## 总结

| 方案 | 可行性 | 卡点 |
|---|---|---|
| **无线 CarPlay**（车机开热点） | ❌ 基本不可行 | ① 蓝牙私有 UART 栈，无 BlueZ，卡在 `STATE_TURNING_ON`（决定性）<br>② 无 API 26+ 热点回调，热点只能手动开<br>③ 单射频 + 无 `dhcpd` |
| **无线 CarPlay**（Wi-Fi Direct） | ❌ 不可行 | iOS 不实现 Wi-Fi Direct；实测 `p2p0` 起不来 |
| **无线 CarPlay**（车机连 iPhone 热点） | ⚠️ 网络层可行 | 网络前提已满足（`<CAR_IP>` ↔ `<PHONE_GW>`，RTT 3~5 ms，0% 丢包），但**仍卡在蓝牙** |
| **有线 CarPlay** | ❌ 不可行 | USB 口 VBUS 无供电，内核不枚举；是硬件电路问题 |

### 如果一定要做

按投入产出排序：

1. **先确认蓝牙能不能救**。`/dev/ttyS5` 是 0666，权限不是问题；
   但「状态卡在 `STATE_TURNING_ON`」+「`IBluetoothHeadset` 绑定失败」
   说明是厂商栈本身没实现完。**这不是配置问题，是缺功能。**
2. **有线方案需要动硬件** —— 检查那个 USB 口的 VBUS 是否真的接到电源，
   以及有没有被保险丝/限流电阻断掉。如果只是断电，加个供电就能救活
   整条有线 CarPlay 路线。
3. **网络层已经准备好了** —— 一旦蓝牙或 USB 任一通了，
   二层互通用 iPhone 热点直接就有，不需要额外做任何网络工程。

---

## 附：这次体检里学到的通用方法

1. **看 APK 自带的 `.so` 列表**。比读文档准 —— 它直接告诉你厂商的实现路径
   （`liblwip_ncm_relay` = USB NCM over IP，`libusb_host_compat` = 裸 libusb HCI）
2. **看 APK 里有没有自带服务实现**。有 `UsbBtBluetoothAdapter`
   就说明它不依赖系统蓝牙栈，**不能拿系统缺 `btusb` 来判死**
3. **区分「系统没有」和「系统坏了」**。`service list` 里没有 adapter 服务
   ⇒ 没实现；状态卡在 `STATE_TURNING_ON` ⇒ 实现了但起不来。两者结论完全不同
4. **失败要分层定位**。ping 不通 → 查 ARP 表（`/proc/net/arp`）→
   ARP 全零 ⇒ 二层不通；ARP 有 MAC 但 ping 不通 ⇒ 三层/防火墙问题
5. **先确认协议对方支持什么**。iOS 不支持 Wi-Fi Direct，
   这种事在动手之前就该查清楚，而不是配了半天 P2P 才发现在等一个不会来的 GO

相关文档：
[01-device-recon.md](01-device-recon.md)（设备侦察）、
[05-dead-ends.md](05-dead-ends.md)（死路清单）、
[06-tooling-for-bare-devices.md](06-tooling-for-bare-devices.md)（造工具）。
