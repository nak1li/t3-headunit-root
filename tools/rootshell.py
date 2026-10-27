#!/usr/bin/env python3
"""root 命令通道客户端 —— 跟设备上那个 root 守护进程对话。

原理
----
设备上有一个以 root 常驻的命令守护进程（由 tools/make_daemon_payload.py
生成的 payload 启动），每 N 秒轮询 $D/rcmd.sh：

    有文件 → mv 成 rcmd.run（原子，避免读到半截）
           → sh rcmd.run > rout.txt 2>&1
           → 追加 ##EXIT=<code>## → 删除 rcmd.run

⚠ 坑 1：守护进程以 root 跑的 `> rout.txt` 建出来的是 0600 root:root，
   app（uid 10055）**读不到**，客户端只会超时，看起来像「命令没执行」。
   所以本工具不依赖 rout.txt：脚本头部自己 touch + chmod 666 rs_out.txt，
   然后 exec >> 重定向进去。

⚠ 坑 2：守护进程是**单线程循环**，一条没有边界的命令（例如
   `grep -r / ...`）会把整个 root 通道永久堵死 —— 连 `kill` 都发不进去，
   因为 kill 也需要 root。真出过一次，最后只能重启整机。
   所以每个脚本都必须挂看门狗：后台 `( sleep N; kill -9 $$ ) &`，
   $$ 在 POSIX sh 的子 shell 里仍指向主 shell，能准确杀掉脚本自己。

交付流程
--------
    1) 脚本头：touch + chmod 666 $D/rs_out.txt，exec >> 进去
    2) put 到 $D/rcmd.tmp
    3) mv 成 $D/rcmd.sh（原子交付）
    4) 轮询 $D/rs_out.txt 直到看见 ##EXIT=

用法
----
    python3 tools/rootshell.py 'id'
    echo 'id; getprop ro.secure' | python3 tools/rootshell.py
    python3 tools/rootshell.py --timeout 120 'mount | grep system'
    python3 tools/rootshell.py --dry-run 'id'     # 只打印将要投递的脚本

配置（环境变量，命令行优先）
----------------------------
    T3_API_HOST      relay 地址，默认 127.0.0.1
    T3_API_PORT      relay 端口，默认 28474
    T3_PUSH_DIR      本机暂存目录（放要 put 的文件），默认 ~/.t3root/push
    T3_DEVICE_DIR    设备端工作目录 $D，默认 /data/local/tmp/rootd

relay 协议（一行一条命令，读到 EOF 为止）
    exec <sh 命令>              在设备上以 relay 的身份执行
    put <设备路径> /in/<文件名>   把 $T3_PUSH_DIR/<文件名> 推到设备路径
"""
import argparse
import os
import socket
import sys
import time

DEFAULT_HOST = os.environ.get("T3_API_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("T3_API_PORT", "28474"))
DEFAULT_PUSH = os.environ.get("T3_PUSH_DIR", "~/.t3root/push")
DEFAULT_DEVICE_DIR = os.environ.get("T3_DEVICE_DIR", "/data/local/tmp/rootd")

MARK = "##EXIT="


class Relay:
    """最小的行协议客户端。"""

    def __init__(self, host, port, timeout=120):
        self.addr = (host, port)
        self.timeout = timeout

    def __call__(self, cmd, timeout=None):
        s = socket.create_connection(self.addr, timeout=timeout or self.timeout)
        s.sendall(cmd.encode() + b"\n")
        buf = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        s.close()
        return buf.decode("utf-8", "replace")


def wrap(script, dev_dir, timeout):
    """把用户脚本包成「可被守护进程安全执行」的形态：666 输出 + 看门狗。"""
    guard = max(10, timeout - 5)
    return "\n".join([
        f"touch {dev_dir}/rs_out.txt 2>/dev/null",
        f"chmod 666 {dev_dir}/rs_out.txt 2>/dev/null",
        f"exec >> {dev_dir}/rs_out.txt 2>&1",
        # 看门狗：到点写标记并 SIGKILL 掉脚本自己
        f"( sleep {guard}; echo '##WATCHDOG##' >> {dev_dir}/rs_out.txt; "
        f"kill -9 $$ ) &",
        "WD=$!",
        script.rstrip("\n"),
        "RC=$?",
        "kill -9 $WD 2>/dev/null",
        f"echo '{MARK}'$RC'##'",
    ]) + "\n"


def root_cmd(script, dev_dir, push_dir, relay, timeout=60, verbose=False):
    """以 root 执行一段 sh 脚本，返回 (输出, 退出码字符串)。"""
    wrapped = wrap(script, dev_dir, timeout)

    os.makedirs(push_dir, exist_ok=True)
    local = os.path.join(push_dir, "rcmd_payload")
    with open(local, "w") as f:
        f.write(wrapped)

    relay(f"exec rm -f {dev_dir}/rs_out.txt {dev_dir}/rcmd.sh {dev_dir}/rcmd.run")
    r = relay(f"put {dev_dir}/rcmd.tmp /in/rcmd_payload")
    if verbose:
        print("[put]", r.strip(), file=sys.stderr)
    relay(f"exec mv {dev_dir}/rcmd.tmp {dev_dir}/rcmd.sh")

    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(1.5)
        out = relay(f"exec cat {dev_dir}/rs_out.txt 2>/dev/null")
        if MARK in out:
            body, _, tail = out.rpartition(MARK)
            code = tail.split("##")[0].strip()
            return body, code
    return "__TIMEOUT__", "?"


def main():
    ap = argparse.ArgumentParser(
        description="通过设备上的 root 守护进程以 uid 0 执行命令",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("script", nargs="*", help="要执行的命令（缺省读 stdin）")
    ap.add_argument("--timeout", type=int, default=60, help="总超时秒数（默认 60）")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--push-dir", default=DEFAULT_PUSH)
    ap.add_argument("--device-dir", default=DEFAULT_DEVICE_DIR)
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印包装后的脚本，不连设备")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    script = " ".join(args.script) if args.script else sys.stdin.read()
    if not script.strip():
        ap.error("没有命令（位置参数或 stdin）")

    dev_dir = args.device_dir.rstrip("/")
    if args.dry_run:
        sys.stdout.write(wrap(script, dev_dir, args.timeout))
        return 0

    relay = Relay(args.host, args.port)
    out, code = root_cmd(script, dev_dir,
                         os.path.expanduser(args.push_dir),
                         relay, timeout=args.timeout, verbose=args.verbose)

    sys.stdout.write(out)
    if out and not out.endswith("\n"):
        sys.stdout.write("\n")
    if out == "__TIMEOUT__":
        print(f"[timeout after {args.timeout}s] 守护进程没响应 —— "
              f"检查它是否还活着、rs_out.txt 是否 666", file=sys.stderr)
        return 1
    print(f"[exit={code}]")
    return 0 if code == "0" else 1


if __name__ == "__main__":
    sys.exit(main())
