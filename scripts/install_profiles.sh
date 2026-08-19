#!/usr/bin/env bash
# 安装尼康 Image SDK 必需的 25 个色彩配置文件到系统目录（需管理员权限）。
# 缺失时 SDK 初始化失败（OpenLibrary 0x8）。
set -euo pipefail

# 优先使用发布包内自带的 Profiles/；其次 NIKON_SDK_DIR；最后仓库 SDK 目录
PACKAGE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -d "$PACKAGE_ROOT/Profiles" ]; then
  SRC="$PACKAGE_ROOT/Profiles"
elif [ -n "${NIKON_SDK_DIR:-}" ] && [ -d "$NIKON_SDK_DIR/Profiles" ]; then
  SRC="$NIKON_SDK_DIR/Profiles"
else
  SRC="${PACKAGE_ROOT}/../Nikon_image/Library/Mac/Profiles"
fi

DST="/Library/Application Support/Nikon/Profiles"

if [ ! -d "$SRC" ]; then
  echo "error: profiles source not found: $SRC" >&2
  echo "  (release package should contain a Profiles/ folder; or set NIKON_SDK_DIR)" >&2
  exit 1
fi

sudo mkdir -p "$DST" || { echo "error: sudo mkdir failed for $DST" >&2; exit 1; }
shopt -s nullglob
sudo cp "$SRC"/* "$DST/" || { echo "error: failed copying profiles to $DST" >&2; exit 1; }
COUNT=$(ls "$DST" | wc -l | tr -d ' ')
echo "installed $COUNT profile(s) to $DST"
if [ "$COUNT" -lt 19 ]; then
  echo "warning: expected >= 19 .icm profiles; check $SRC" >&2
  exit 1
fi
