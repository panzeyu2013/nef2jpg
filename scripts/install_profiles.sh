#!/usr/bin/env bash
# 安装尼康 Image SDK 必需的 25 个色彩配置文件到系统目录（需管理员权限）。
# 缺失时 SDK 初始化失败（OpenLibrary 0x8）。
set -euo pipefail

# 默认从本仓库的 SDK 目录取；可用 NIKON_SDK_DIR 环境变量覆盖
SDK_DIR="${NIKON_SDK_DIR:-$(cd "$(dirname "$0")/.." && pwd)/../Nikon_image/Library/Mac}"
SRC="$SDK_DIR/Profiles"
DST="/Library/Application Support/Nikon/Profiles"

if [ ! -d "$SRC" ]; then
  echo "error: profiles source not found: $SRC (set NIKON_SDK_DIR)" >&2
  exit 1
fi

sudo mkdir -p "$DST"
sudo cp "$SRC"/* "$DST/"
COUNT=$(ls "$DST" | wc -l | tr -d ' ')
echo "installed $COUNT profile(s) to $DST"
if [ "$COUNT" -lt 19 ]; then
  echo "warning: expected >= 19 .icm profiles; check $SRC" >&2
  exit 1
fi
