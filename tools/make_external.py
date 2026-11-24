#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 R12 固件“对外版”工程：Mac F5 / F6 功能键模块以预编译 mac_fkey.o 提供，不含源码。

在仓库根目录运行（Windows 命令行）：
    python tools\\make_external.py --armclang "D:\\Keil_v5\\ARM\\ARMCompiler6.16\\Bin\\armclang.exe"

可选参数：
    --out <目录>     输出目录，默认在仓库旁边新建 R12_对外版_<日期>_<提交号>
    --zip            另外打包成同名 zip
    --branch <分支>  把对外版提交到这个分支（如 external）；分支不存在时新建一个
                     没有任何历史的独立分支。提交后自行 git push origin <分支>

脚本做的事：
  1. 导出最近一次提交（HEAD）的全部文件，不含 .git 历史，也不含未提交的改动。
  2. 用 Keil 自带的 armclang 把 mac_fkey.c 编成机器码 mac_fkey.o。
     包含路径、宏定义从 Keil 工程 SE73K.uvprojx 读取，其他选项和 Keil 编译时相同，
     只多加 -fno-lto 和 -g0：工程开了链接时优化（LTO），Keil 编出来的 .o 里是 LLVM
     中间代码，可以还原出逻辑；关掉 LTO、不带调试信息，.o 里就只有机器码。
  3. 去掉内部资料：mac_fkey.c（F5 / F6 源码）、docs/改动对比/、
     docs/2.4G接收器_勿扰键对接说明.md、docs/对外版生成说明.md、本脚本。
  4. Keil 工程里把 mac_fkey.c 换成 mac_fkey.o，按目标文件链接（和 userdefined_key.o 一样）。
  5. 写入“对外版说明.txt”，并检查结果。
"""
import argparse
import datetime
import hashlib
import io
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEIL_DIR = "projects/mouse_demo/keil5"
UVPROJX = KEIL_DIR + "/SE73K.uvprojx"
UVOPTX = KEIL_DIR + "/SE73K.uvoptx"
SRC = "projects/mouse_demo/periph/mac_fkey.c"
HDR = "projects/mouse_demo/periph/mac_fkey.h"
OBJ = "projects/mouse_demo/periph/mac_fkey.o"

# 对外版里去掉的内部资料
EXCLUDE = [
    SRC,
    "docs/改动对比",
    "docs/2.4G接收器_勿扰键对接说明.md",
    "docs/对外版生成说明.md",
    "tools/make_external.py",
]

# 和 Keil 编译时相同的选项（取自 Keil 生成的 SE73K_SE73K.dep），只把 -flto 换成 -fno-lto、-gdwarf-4 换成 -g0
BASE_FLAGS = [
    "-xc", "-std=c99", "--target=arm-arm-none-eabi", "-mcpu=cortex-m0", "-c",
    "-fno-rtti", "-fno-lto", "-funsigned-char", "-fshort-enums", "-fshort-wchar",
    "-D__MICROLIB", "-g0", "-Oz", "-ffunction-sections",
    "-Wno-packed", "-Wno-missing-variable-declarations", "-Wno-missing-prototypes",
    "-Wno-missing-noreturn", "-Wno-sign-conversion", "-Wno-nonportable-include-path",
    "-Wno-reserved-id-macro", "-Wno-unused-macros", "-Wno-documentation-unknown-command",
    "-Wno-documentation", "-Wno-license-management", "-Wno-parentheses-equality",
    "-D__UVISION_VERSION=543",
]
# BASE_FLAGS 对应的 Keil 工程设置；工程设置改了要同步改 BASE_FLAGS
EXPECT_CADS = {"Optim": "7", "v6Lang": "3", "vShortEn": "1", "vShortWch": "1"}
EXPECT_CPU = 'CPUTYPE("Cortex-M0")'


def fail(msg):
    print("错误：" + msg)
    sys.exit(1)


def git(*args):
    try:
        return subprocess.run(["git", "-C", str(ROOT)] + list(args), check=True,
                              capture_output=True).stdout
    except FileNotFoundError:
        fail("找不到 git，请先安装 git 并加入 PATH")
    except subprocess.CalledProcessError as e:
        fail("git %s 失败：%s" % (" ".join(args), e.stderr.decode("utf-8", "replace")))


def gitw(wt, *args):
    """在指定工作树里执行 git"""
    r = subprocess.run(["git", "-C", str(wt)] + list(args), capture_output=True)
    if r.returncode != 0:
        fail("git %s 失败：%s" % (" ".join(args), r.stderr.decode("utf-8", "replace")))
    return r.stdout


def commit_to_branch(src, branch, head, ver, trailer=""):
    """把对外版目录 src 的内容提交到 branch；分支不存在时新建没有历史的独立分支"""
    wt = Path(tempfile.mkdtemp(prefix="r12_external_"))
    wt.rmdir()
    exists = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--verify", "--quiet",
                             "refs/heads/" + branch], capture_output=True).returncode == 0
    if exists:
        git("worktree", "add", str(wt), branch)
    else:
        git("worktree", "add", "--detach", str(wt), "HEAD")
        gitw(wt, "checkout", "-q", "--orphan", branch)
    try:
        gitw(wt, "rm", "-r", "-q", "-f", "--ignore-unmatch", ".")
        for p in wt.iterdir():
            if p.name != ".git":
                shutil.rmtree(p) if p.is_dir() else p.unlink()
        for p in src.iterdir():
            shutil.copytree(p, wt / p.name) if p.is_dir() else shutil.copy2(p, wt / p.name)
        gitw(wt, "add", "-A")
        gitw(wt, "add", "-f", OBJ)              # 防止 .o 被 .gitignore 忽略
        msg = "对外版：来源提交 %s，mac_fkey V%s" % (head, ver)
        if trailer:
            msg += "\n\n" + trailer
        gitw(wt, "commit", "-q", "-m", msg)
        new = gitw(wt, "rev-parse", "--short", "HEAD").decode().strip()
        print("已提交到分支 %s（%s，%s）" % (branch, new, "新建的独立分支" if not exists else "在原分支上追加"))
        print("推送：git push origin %s" % branch)
    finally:
        git("worktree", "remove", "--force", str(wt))


def read_keep(path):
    """原样读入文本（不转换换行符：Keil 工程文件里有单独的回车符）"""
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write_keep(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def export_head(dest):
    data = git("archive", "--format=tar", "HEAD")
    with tarfile.open(fileobj=io.BytesIO(data), encoding="utf-8") as tf:
        try:
            tf.extractall(dest, filter="data")
        except TypeError:                       # Python 3.11 及更早没有 filter 参数
            tf.extractall(dest)


def keil_settings(uvprojx_text):
    """读出 Keil 工程的包含路径、宏定义，并核对其他编译设置没变"""
    m = re.search(r"<Cads>(.*?)</Cads>", uvprojx_text, re.S)
    if not m:
        fail("Keil 工程里找不到 C 编译设置（<Cads>）")
    cads = m.group(1)

    def tag(name, text=cads):
        t = re.search(r"<%s>(.*?)</%s>" % (name, name), text, re.S)
        return t.group(1).strip() if t else None

    for k, v in EXPECT_CADS.items():
        if tag(k) != v:
            fail("Keil 工程的编译设置 %s = %s，脚本按 %s 编写；请同步修改脚本里的 BASE_FLAGS" % (k, tag(k), v))
    if EXPECT_CPU not in (tag("Cpu", uvprojx_text) or ""):
        fail("Keil 工程的芯片内核不是 Cortex-M0，请同步修改脚本里的 BASE_FLAGS")
    if tag("useUlib", uvprojx_text) != "1":
        fail("Keil 工程没有使用 MicroLIB，请同步修改脚本里的 BASE_FLAGS（-D__MICROLIB）")

    incs = []
    for p in (tag("IncludePath") or "").split(";"):
        p = p.strip().replace("\\", "/")
        if p and p not in incs:
            incs.append(p)
    defs = [d.strip() for d in (tag("Define") or "").split(",") if d.strip()]
    misc = (tag("MiscControls") or "").split()
    return incs, defs, misc


def elf_info(data):
    """解析 ELF32 目标文件：返回节名列表和全局定义的符号"""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail("mac_fkey.o 不是 32 位小端 ELF 目标文件")
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        f = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        secs.append({"name": f[0], "type": f[1], "offset": f[4], "size": f[5], "link": f[6]})

    def cstr(base, off):
        end = data.index(b"\0", base + off)
        return data[base + off:end].decode("utf-8", "replace")

    names = [cstr(secs[shstrndx]["offset"], s["name"]) for s in secs]
    syms = set()
    for s in secs:
        if s["type"] == 2:                      # SHT_SYMTAB
            strbase = secs[s["link"]]["offset"]
            for j in range(s["size"] // 16):
                st_name, _, _, st_info, _, st_shndx = struct.unpack_from("<IIIBBH", data, s["offset"] + j * 16)
                if (st_info >> 4) == 1 and st_shndx != 0:       # 全局、已定义
                    syms.add(cstr(strbase, st_name))
    return names, syms


def main():
    ap = argparse.ArgumentParser(description="生成 R12 固件对外版工程（F5 / F6 模块以 mac_fkey.o 提供）")
    ap.add_argument("--armclang", required=True, help="Keil 的 armclang.exe 路径，如 D:\\Keil_v5\\ARM\\ARMCompiler6.16\\Bin\\armclang.exe")
    ap.add_argument("--out", help="输出目录，默认在仓库旁边新建 R12_对外版_<日期>_<提交号>")
    ap.add_argument("--zip", action="store_true", help="另外打包成 zip")
    ap.add_argument("--branch", help="把对外版提交到这个分支（如 external），不存在时新建没有历史的独立分支")
    ap.add_argument("--extra-cflags", default="", help=argparse.SUPPRESS)   # 仅供脚本自测
    ap.add_argument("--commit-trailer", default="", help=argparse.SUPPRESS)
    a = ap.parse_args()

    armclang = Path(a.armclang)
    if not armclang.is_file():
        fail("找不到 armclang：%s" % armclang)
    vr = subprocess.run([str(armclang), "--version"], capture_output=True)
    vlines = [l.strip() for l in (vr.stdout + vr.stderr).decode("utf-8", "replace").splitlines() if l.strip()]
    compiler = next((l for l in vlines if "Compiler" in l or "clang version" in l), vlines[0] if vlines else "未知")
    if "ARM Compiler" not in compiler and "Arm Compiler" not in compiler:
        print("提示：用的不是 Keil 的 armclang（%s），生成后请用 Keil 编译对外版工程确认能正常链接。" % compiler)

    head = git("rev-parse", "--short", "HEAD").decode().strip()
    head_date = git("log", "-1", "--format=%cd", "--date=format:%Y-%m-%d %H:%M").decode().strip()
    if git("status", "--porcelain", "--untracked-files=no").strip():
        print("提示：工作区有未提交的改动，对外版只包含最近一次提交 %s 的内容。" % head)

    today = datetime.date.today().strftime("%Y%m%d")
    out = Path(a.out) if a.out else ROOT.parent / ("R12_对外版_%s_%s" % (today, head))
    if out.exists() and any(out.iterdir()):
        fail("输出目录已存在且不为空：%s" % out)
    out.mkdir(parents=True, exist_ok=True)

    # 1. 导出 HEAD
    print("1/5 导出提交 %s（%s）到 %s" % (head, head_date, out))
    export_head(out)
    for need in (SRC, HDR, UVPROJX):
        if not (out / need).is_file():
            fail("提交里缺少 %s" % need)

    # 2. 编译 mac_fkey.o
    uvprojx = read_keep(out / UVPROJX)
    incs, defs, misc = keil_settings(uvprojx)
    pack = armclang.resolve().parents[2] / "Packs/Onmicro/OM6239/1.0.0/Device/Onmicro/OM6239"
    cmd = [str(armclang)] + BASE_FLAGS + misc
    for p in incs:
        cmd += ["-I", p]
    if pack.is_dir():
        cmd += ["-I", str(pack)]
    cmd += ["-D" + d for d in defs] + a.extra_cflags.split()
    cmd += ["-o", "../periph/mac_fkey.o", "../periph/mac_fkey.c"]
    print("2/5 编译 mac_fkey.o（关闭 LTO，不带调试信息）")
    r = subprocess.run(cmd, cwd=str(out / KEIL_DIR), capture_output=True)
    msg = (r.stdout + r.stderr).decode("utf-8", "replace").strip()
    if r.returncode != 0:
        fail("编译失败：\n" + msg)
    if msg:
        print(msg)

    obj = (out / OBJ).read_bytes()
    secs, syms = elf_info(obj)
    if ".llvmbc" in secs:
        fail("mac_fkey.o 里有 LLVM 中间代码（.llvmbc），-fno-lto 没生效")
    if any(s.startswith(".debug") for s in secs):
        fail("mac_fkey.o 里有调试信息，-g0 没生效")
    if "RAM_CODE" not in secs:
        fail("mac_fkey.o 里没有 RAM_CODE 段，请确认用的是 Keil 的 armclang")
    api = re.findall(r"\b(mac_fkey_\w+)\s*\(", (out / HDR).read_text(encoding="utf-8"))
    missing = sorted(set(api) - syms)
    if missing:
        fail("mac_fkey.o 缺少接口函数：" + "、".join(missing))
    ver = re.search(r"#define\s+MAC_FKEY_VERSION\s+0x([0-9A-Fa-f]{2})([0-9A-Fa-f]{2})", (out / HDR).read_text(encoding="utf-8"))
    ver = "%d.%02d" % (int(ver.group(1), 16), int(ver.group(2), 16)) if ver else "未知"
    sha = hashlib.sha256(obj).hexdigest()

    # 3. 去掉内部资料
    print("3/5 去掉 F5 / F6 源码和内部资料")
    for p in EXCLUDE:
        t = out / p
        if t.is_dir():
            shutil.rmtree(t)
        elif t.exists():
            t.unlink()
        parent = t.parent                       # 删掉因此变空的目录（如 tools/）
        while parent != out and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent

    # 4. Keil 工程改为链接 mac_fkey.o
    print("4/5 Keil 工程改为链接 mac_fkey.o")
    uvprojx, n = re.subn(
        r"(<File>\s*<FileName>)mac_fkey\.c(</FileName>\s*<FileType>)1(</FileType>\s*<FilePath>[^<]*?)mac_fkey\.c(</FilePath>)",
        r"\g<1>mac_fkey.o\g<2>3\g<3>mac_fkey.o\g<4>", uvprojx)
    if n != 1:
        fail("Keil 工程里找不到 mac_fkey.c，无法替换为 mac_fkey.o")
    write_keep(out / UVPROJX, uvprojx)
    uvoptx_path = out / UVOPTX
    if uvoptx_path.is_file():
        t0 = read_keep(uvoptx_path)
        t = re.sub(
            r"(<FileType>)1(</FileType>(?:(?!</File>).)*?<PathWithFileName>[^<]*?)mac_fkey\.c"
            r"(</PathWithFileName>\s*<FilenameWithoutPath>)mac_fkey\.c(</FilenameWithoutPath>)",
            r"\g<1>3\g<2>mac_fkey.o\g<3>mac_fkey.o\g<4>", t0, flags=re.S)
        if t != t0:
            write_keep(uvoptx_path, t)

    # 5. 说明文件
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    readme = """R12 三模键盘固件 · 对外版
================================
生成时间：{now}
来源提交：{head}（{head_date}）
F5 / F6 模块：mac_fkey V{ver}
mac_fkey.o 编译器：{compiler}
mac_fkey.o SHA-256：{sha}

和内部版的区别
--------------
1. Mac 布局 F5（听写）、F6（勿扰模式）功能键模块不提供源码，只提供：
     projects/mouse_demo/periph/mac_fkey.h   接口声明
     projects/mouse_demo/periph/mac_fkey.o   预编译目标文件（机器码，不含调试信息）
   Keil 工程已按目标文件链接 mac_fkey.o，用法和 userdefined_key.o 相同。
2. 不含 git 历史和内部文档。

编译
----
用 Keil MDK（ARM Compiler 6.16）打开 projects/mouse_demo/keil5/SE73K.uvprojx，直接编译。

注意：以下内容改动后，mac_fkey.o 需要由固件提供方重新生成，否则 F5 / F6 会失效
------------------------------------------------------------------------------
- mac_fkey.o 直接使用这些全局变量，不能改名或改类型：
    device_type、m_data_flag、multi_data_tmp_l、multi_data_tmp_h
- 这些定义的取值已经编进 mac_fkey.o，不能改：
    bsys_data_send（m_data_flag 第 5 位）、iDEVICE_TYPE_24G、
    iKEY_MAC_F5、iKEY_MAC_F6、iRF_K_TYPE_KB、iRF_K_OTHER_SYS、iRF_K_LENGTH_OTHER
- 编译设置保持 Cortex-M0、Short enums/wchar。
- mac_fkey.h 里的函数和各处调用不要删改。
""".format(now=now, head=head, head_date=head_date, ver=ver, sha=sha, compiler=compiler)
    (out / "对外版说明.txt").write_text(readme.replace("\n", "\r\n"), encoding="utf-8-sig", newline="")

    # 检查
    print("5/5 检查")
    left = [str(p.relative_to(out)) for p in out.rglob("mac_fkey.c")]
    if left:
        fail("对外版里还有 mac_fkey.c：" + "、".join(left))
    t = read_keep(out / UVPROJX)
    if "mac_fkey.c" in t or "<FilePath>..\\periph\\mac_fkey.o</FilePath>" not in t:
        fail("Keil 工程没有正确改为链接 mac_fkey.o")
    for p in EXCLUDE:
        if (out / p).exists():
            fail("内部资料没有删掉：" + p)

    if a.zip:
        z = shutil.make_archive(str(out), "zip", root_dir=str(out.parent), base_dir=out.name)
        print("已打包：" + z)
    if a.branch:
        commit_to_branch(out, a.branch, head, ver, a.commit_trailer)
    print("\n完成。对外版工程：%s" % out)
    print("mac_fkey.o：%d 字节，接口函数 %d 个，SHA-256 %s" % (len(obj), len(api), sha))
    print("下一步：用 Keil 打开对外版工程编译，烧录后在 USB、蓝牙、2.4G 下各测一次 F5、F6。")


if __name__ == "__main__":
    main()
