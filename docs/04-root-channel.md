# 04 · 不依赖 adb / setuid 的 root 命令通道

拿到 root 只是一瞬间的事。真正的工程问题是：

> **怎么让一个 uid 10055 的 app，反复地、稳定地以 root 执行命令？**

这台机器上所有常规方案都不成立（原因见 [05-dead-ends.md](05-dead-ends.md)）：

- `su` —— 原版实现，无 setuid 位
- setuid 位 —— `/system` `/data` 都是 nosuid
- 连 rootfs 上的 setuid —— 被 `no_new_privs` 挡掉
- adb —— 厂商没编译 `ALLOW_ADBD_ROOT`，adbd 只有 uid 2000

所以只能用「init 托管一个 root 进程」这个唯一剩下的能力。

---

## 设计

把 Dirty COW 拿到的那个 root 执行机会，变成一个**常驻的命令守护进程**：

```
agent (uid 10055)                 root 守护进程 (uid 0，init 托管)
      │                                     │
      │  写 $B/rcmd.sh                       │  每 2 秒轮询
      ├────────────────────────────────────►│
      │                                     │  mv rcmd.sh → rcmd.run （原子）
      │                                     │  sh rcmd.run > 输出
      │                                     │  追加 ##EXIT=<code>##
      │                                     │  rm rcmd.run
      │  轮询 $B/rs_out.txt 直到看见 ##EXIT= │
      ◄─────────────────────────────────────┤
```

守护进程的形态就是 payload 里 `sh -c` 的最后一段：

```sh
while true; do
  if [ -f $B/rcmd.sh ]; then
    mv $B/rcmd.sh $B/rcmd.run 2>/dev/null
    if [ -f $B/rcmd.run ]; then
      sh $B/rcmd.run > $B/rout.txt 2>&1
      rm -f $B/rcmd.run
    fi
  fi
  sleep 2
done
```

因为它**永不退出**，init 认为服务正常运行，不会重启它，也不会干扰。

---

## 三个必须处理的细节

### 1. `mv` 交付，不要直接写目标文件

如果 agent 直接写 `rcmd.sh`，守护进程可能在文件只写了一半时就读到它，
执行一段截断的脚本。用「写临时文件 + `mv`」保证原子性
（同一文件系统内 `rename` 是原子的）。

### 2. 输出文件权限：`> rout.txt` 建出来是 `0600 root:root`

这是第一次实测失败的原因：守护进程确实执行了命令
（`rout.txt` 存在、217 字节、12:26 由 root 创建），
但 **agent（uid 10055）读不到**，客户端只能超时。

两种解法，推荐第二种：

```sh
# 方案 A：执行后 chmod（但要注意 $? 会被 chmod 覆盖）
sh rcmd.run > rout.txt 2>&1; X=$?; chmod 666 rout.txt; echo '##EXIT='$X'##' >> rout.txt

# 方案 B（推荐）：让脚本自己写进一个预先 chmod 666 的文件
touch $B/rs_out.txt; chmod 666 $B/rs_out.txt
exec >> $B/rs_out.txt 2>&1
<用户的命令>
echo '##EXIT='$?'##'
```

### 3. 看门狗 —— 这是必须的，不是可选

守护进程是**单线程循环**。一条没有边界的命令就能把整个 root 通道永久堵死。

真实事故：

```bash
# 想找 isomountmanager 的 rc 定义在哪
grep -rln "isomount" / --include="*.rc"
```

这条命令在守护进程里跑了起来，然后：
- 满盘扫描，CPU 100%，跑了十几分钟没完
- 守护进程卡在 `waitpid` 里
- **新的 `rcmd.sh` 完全没人处理**
- 而 kill 需要 root —— root 通道被自己堵死了，`kill` 发不进去
- 唯一出路：**重启整机**

所以客户端必须给每个脚本挂一个后台定时器：

```sh
( sleep <guard>; kill -9 $$ ) &
WD=$!
<用户的命令>
RC=$?
kill -9 $WD 2>/dev/null
```

`$$` 在 POSIX sh 的子 shell 里仍然指向主 shell 的 pid，所以能准确杀掉脚本自己。

实现见 `tools/rootshell.py`。

---

## 用法

```bash
$ python3 tools/rootshell.py 'id'
uid=0(root) gid=0(root) groups=1003(graphics),1005(audio),1006(camera),...

$ python3 tools/rootshell.py <<'EOF'
mount -o rw,remount /system
dd if=/data/local/tmp/new.bin of=/system/bin/xxx
sync
mount -o ro,remount /system
EOF
[exit=0]
```

## 为什么这个通道比 adb 更好

| | root 命令通道 | adb TCP |
|---|---|---|
| 权限 | **uid 0** | uid 2000（本机） |
| 需要入向连接 | 否 | 是（车机在 CGNAT 后面，根本连不进来） |
| 需要改 APK | 否 | 是 |
| 重启后 | 需要持久化（见 03） | adbd 不会自启 |

对一个在 CGNAT 后面的车机来说，**「车机主动出向」是唯一的通道形态**，
所以这个方案天然合适。
