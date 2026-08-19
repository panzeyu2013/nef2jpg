#!/usr/bin/env bash
# 将 macOS 二进制及其 Homebrew 动态依赖收集进 <libdir>，并把 install name 改写为 @rpath，
# 使发布包自包含（不依赖目标机器的 Homebrew）。
#
# 用法: fixup_macos.sh <binary> <libdir>
# 说明: 仅处理 /opt/homebrew 与 /usr/local 下的依赖；系统库(/usr/lib、/System)不动。
set -euo pipefail

BIN=${1:?usage: fixup_macos.sh <binary> <libdir>}
LIBDIR=${2:?}

mkdir -p "$LIBDIR"

copy_dep() { # <abs-path>；返回 basename（非 brew 依赖返回空）
  local dep="$1"
  case "$dep" in
    /opt/homebrew/*|/usr/local/*) ;;
    *) return 0 ;;
  esac
  local base
  base=$(basename "$dep")
  if [ ! -f "$LIBDIR/$base" ]; then
    cp "$dep" "$LIBDIR/$base" 2>/dev/null || return 0
  fi
  printf '%s' "$base"
}

fix_file() { # <file>：把 brew 绝对路径依赖改写为 @rpath/<basename>
  local f="$1"
  local dep base
  while IFS= read -r dep; do
    base=$(basename "$dep")
    install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null || true
  done < <(otool -L "$f" | awk '/\/opt\/homebrew\/|\/usr\/local\//{print $1}')
}

# 1) 二进制直接依赖
while IFS= read -r dep; do
  copy_dep "$dep" >/dev/null
done < <(otool -L "$BIN" | awk '/\/opt\/homebrew\/|\/usr\/local\//{print $1}')

# 2) 传递依赖（深度 ≤3 足够：exiv2 → brotli/intl/inih）
for _ in 1 2 3; do
  for f in "$LIBDIR"/*.dylib; do
    [ -e "$f" ] || continue
    while IFS= read -r dep; do
      base=$(copy_dep "$dep")
      if [ -n "$base" ] && [ "$base" != "$(basename "$f")" ]; then
        install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null || true
      fi
    done < <(otool -L "$f" | awk '/\/opt\/homebrew\/|\/usr\/local\//{print $1}')
  done
done

# 3) 统一 install name 为 @rpath，并修复二进制
for f in "$LIBDIR"/*.dylib; do
  [ -e "$f" ] || continue
  install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null || true
done
fix_file "$BIN"

# 4) install_name_tool 使旧签名失效：adhoc 重签
codesign --force --sign - "$BIN" 2>/dev/null || true
for f in "$LIBDIR"/*.dylib; do
  [ -e "$f" ] || continue
  codesign --force --sign - "$f" 2>/dev/null || true
done

echo "fixup_macos: bundled $(ls "$LIBDIR" | wc -l | tr -d ' ') dylib(s) into $LIBDIR"
