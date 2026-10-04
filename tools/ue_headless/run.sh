#!/usr/bin/env bash
# 在真实 UE 编辑器（无头命令集）里跑背包插件的友好层行为验证。
#
#   ./run.sh
#
# 可用环境变量覆盖路径：
#   UE_CMD   默认 /Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd
#   PROJECT  默认 /Users/ldd/Documents/UEPROJ/gamedemo/gamedemo.uproject
#
# 前置：项目 C++ 已编译过；插件 Binaries/<平台>/ 里有 Rust dylib（uerust build .）。
set -euo pipefail

UE_CMD="${UE_CMD:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd}"
PROJECT="${PROJECT:-/Users/ldd/Documents/UEPROJ/gamedemo/gamedemo.uproject}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$UE_CMD" ]; then
    echo "找不到 UnrealEditor-Cmd: $UE_CMD（用 UE_CMD=... 覆盖）" >&2
    exit 1
fi

for s in inv_behavior_test.py inv_owncheck.py inv_demo_test.py inv_pipeline_test.py inv_ui_test.py inv_interaction_test.py; do
    echo "=== 跑 $s ==="
    "$UE_CMD" "$PROJECT" -run=pythonscript -script="$HERE/$s" \
        -EnablePlugins=PythonScriptPlugin -unattended -nosplash -stdout -NoLogTimes >/dev/null 2>&1 || true
    out="$HERE/${s%.py}_out.txt"
    if [ -f "$out" ]; then
        tail -n 2 "$out"
    else
        echo "（没有结果文件 $out —— 看编辑器日志找原因）"
    fi
done
