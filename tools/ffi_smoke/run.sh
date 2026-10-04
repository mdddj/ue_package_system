#!/usr/bin/env bash
# 编译并运行 FFI 冒烟：不依赖 UE，验证 dylib 的导出符号与 C ABI 是否正常。
#
#   ./run.sh
#
# 前置：先在插件根目录跑过 `uerust build .`（Binaries/<平台>/ 里要有库）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"

case "$(uname -s)" in
    Darwin) LIB="$ROOT/Binaries/Mac/libue_package_system_dylib.dylib" ;;
    Linux)  LIB="$ROOT/Binaries/Linux/libue_package_system_dylib.so" ;;
    *)      echo "只支持 macOS / Linux；Windows 请用 cl.exe 自行编译 ffi_smoke.c" >&2; exit 1 ;;
esac

if [ ! -f "$LIB" ]; then
    echo "找不到 $LIB —— 先在插件目录跑 \`uerust build .\`" >&2
    exit 1
fi

OUT="$(mktemp -d)/ffi_smoke"
cc -O1 -o "$OUT" "$HERE/ffi_smoke.c"
"$OUT" "$LIB"
