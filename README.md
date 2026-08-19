# nef2jpg

NEF → JPEG 批量转换命令行工具。
- **macOS arm64**：尼康官方 Image SDK (NkImgSDK 1.46.0) 解码，官方画质（同 NX Studio 引擎）。
- **Linux x64**：v1 构建占位（解码后端未就绪，v2 计划接入 libraw）。

## 依赖与构建

依赖统一由 Homebrew 管理：

```bash
brew install libjpeg-turbo exiv2 cmake
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

尼康 SDK 位置通过 `-DNIKON_SDK_DIR=<path>` 覆盖（默认 `../Nikon_image/Library/Mac`）。

macOS 构建后 `build/` 下即自包含：`nef2jpg` + `lib/`（SDK 动态库、Elm.framework、exiv2 及其依赖，install name 已改写为 `@rpath`）+ `Contents/Resources/prm.bin`。运行时**不需要**目标机器装有 Homebrew。

## 用法

```
nef2jpg [选项] <输入...>

  输入                 .NEF 文件 / glob / 目录（-r 递归）
  -o, --out-dir DIR    输出目录
  -j, --jobs N         并发数（默认 min(CPU核数,8)，并按内存自动封顶；上限 64）
  -q, --quality N      JPEG 质量 1-100（默认 90）
  -s, --target-size S  单张目标大小（500K / 1M / 2.5M），质量二分逼近
      --max-size S     硬上限（无法满足时告警）
  -t, --tolerance PCT  目标大小容差 %（默认 5，上限 100）
      --progressive    渐进式 JPEG
      --subsampling N  420 / 422 / 444
      --no-icc         不嵌入 sRGB ICC
  -w, --resize N       最大边缩到 N 像素
      --no-auto-orient 不按 EXIF 自动旋转
      --dry-run        只预览任务
      --overwrite      覆盖已存在输出
  -v, --verbose        详细输出（含分阶段耗时）
```

退出码：`0` 全部成功 / `1` 部分失败 / `2` 用法错误 / `130` 被中断（Ctrl-C，worker 完成当前文件后停止，输出已完成的文件完整落盘）。

## macOS 运行前提

尼康 SDK 需要 25 个色彩配置文件（19 个 `.icm` 等；`CCMSService` 初始化必需，缺失时报 `OpenLibrary 0x8`）。发布包自带 `Profiles/` 目录，安装脚本：

```bash
scripts/install_profiles.sh        # 需要管理员权限（sudo）
```

注意：每次运行启动时，SDK 会在 stdout 打印两行
`NOT FOUND ".../Contents/Resources/enum_string.csv"` 之类的提示（SDK 内部查找可选的
枚举名映射表），**无害**，可忽略。

## 示例

```bash
# 单张
nef2jpg photo.NEF
# 多张 + 并发 4 + 目标大小
nef2jpg -j 4 -s 1M -o out/ *.NEF
# 目录递归 + 缩图
nef2jpg -r -w 2000 -o out/ photos/
```

## 发布说明（面向分发者）

- **自包含**：macOS 发布包解压即用（`lib/` 已打包全部动态库、`Contents/Resources/prm.bin`、`Profiles/`），无需目标机 Homebrew。
- **手动发布**：本地 `cmake --build build` 后，打包 `build/nef2jpg + build/lib + build/Contents/Resources/prm.bin + Profiles + scripts + README` 为 tar.gz，手动上传到 GitHub Release 即可（无需 CI）。
- **签名/公证**：本地产物为 adhoc 签名。正式分发建议用 Developer ID 签名并公证（`codesign` + `notarytool`），否则用户需右键打开或 `xattr -d com.apple.quarantine`。
- **许可**：尼康 Image SDK 为授权产品，随包再分发前请核对 SDK 许可协议；exiv2 为 LGPL-2.1；libjpeg-turbo 为 IJG/BSD 类许可。源码发布不受影响。

## 已知限制

- 单张全尺寸解码约 8s（45MP）：尼康 SDK 单线程固定成本，批量场景用 `-j` 并行（实测 -j8 ≈ 3.9x）。
- 输出写入为"临时文件 + rename"原子落盘；EXIF 重写基于 exiv2。
- `--max-size` 作用于编码后、EXIF 写入前的 JPEG，最终文件可能略超上限（EXIF 附加的几百字节）。
