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

完整帮助见 `nef2jpg -h`，要点如下：

```
nef2jpg [选项] <输入...>

输入
  <input...>          一个或多个 .NEF 文件 / glob / 目录
  -r, --recursive     递归扫描目录内的 .NEF
      --dry-run       只预览任务，不执行

输出
  -o, --out-dir DIR   输出目录（默认：与输入同目录，不存在自动创建）
      --overwrite     覆盖已存在输出（默认：跳过）
      --suffix STR    输出文件名后缀（默认: _decoded）

并发
  -j, --jobs N        并发 worker 数（默认: min(CPU核数,8)，并按内存自动封顶；上限 64）

JPEG 质量与大小（优先级: --max-size > --target-size > --quality）
  -q, --quality N     JPEG 质量 1-100（默认: 90）。无 -s 时作为最终质量；有 -s 时仅作二分起点
  -s, --target-size S 单张目标大小，如 500K / 1M / 2.5M，以 -q 为起点二分逼近
      --max-size S    硬上限（无法满足时告警）
  -t, --tolerance PCT 目标大小容差 %（默认: 5，上限 100）

JPEG 编码
      --progressive   渐进式编码（边下载边显示；默认基线式，兼容性最好）
      --subsampling N 色度子采样 420 / 422 / 444（默认: 420）。420 体积最小但精细
                      彩色边缘可能轻微渗色；444 细节最锐体积最大；422 折中
      --icc FILE      嵌入自定义 ICC（如 Adobe RGB/Display P3，1-65519 字节；与 --no-icc 互斥）
      --no-icc        不嵌入 ICC（默认嵌入 sRGB）

图像
  -w, --resize N      最大边缩到 N 像素（保持宽高比，只缩小）
      --no-auto-orient 不按 EXIF 自动转正（默认自动转正）

其他
      --backend NAME  解码后端：auto | sdk（默认: auto）
  -v, --verbose       详细输出（含 open/decode/encode/exif 分阶段耗时）
  -h, --help          显示帮助
```

**参数优先级**：`--max-size`（硬上限）> `--target-size`（自动选质量）> `--quality`。
即：给了 `-s` 时 `-q` 只作为二分起点；`--max-size` 会在前两者结果之上继续压质量。

退出码：`0` 全部成功 / `1` 部分失败 / `2` 用法错误 / `130` (Ctrl-C) / `143` (SIGTERM)（中断时 worker 完成当前文件后停止，已完成的输出完整落盘）。

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
# 自定义 ICC + 渐进式
nef2jpg --icc DisplayP3.icc --progressive photo.NEF
# 预览任务（不执行）
nef2jpg --dry-run -r photos/
```

## 发布说明（面向分发者）

- **自包含**：macOS 发布包解压即用（`lib/` 已打包全部动态库、`Contents/Resources/prm.bin`、`Profiles/`），无需目标机 Homebrew。
- **系统要求**：当前发布包要求 **macOS 26+**（brew 依赖 libexiv2/brotli 按构建机系统编译，其 minos 为 26.0；若需支持更早系统，需自行以更低部署目标构建这些依赖）。
- **手动发布**：本地 `cmake --build build` 后，打包 `build/nef2jpg + build/lib + build/Contents/Resources/prm.bin + Profiles + scripts + README` 为 tar.gz，手动上传到 GitHub Release 即可（无需 CI）。
- **签名/公证**：本地产物为 adhoc 签名。正式分发建议用 Developer ID 签名并公证（`codesign` + `notarytool`），否则用户需右键打开或 `xattr -d com.apple.quarantine`。
- **许可**：尼康 Image SDK 为授权产品，随包再分发前请核对 SDK 许可协议；exiv2 为 LGPL-2.1；libjpeg-turbo 为 IJG/BSD 类许可。源码发布不受影响。

## 已知限制

- 单张全尺寸解码约 8s（45MP）：尼康 SDK 单线程固定成本，批量场景用 `-j` 并行（实测 -j8 ≈ 3.9x）。
- 输出写入为"临时文件 + rename"原子落盘；EXIF 重写基于 exiv2。
- `--max-size` 作用于编码后、EXIF 写入前的 JPEG，最终文件可能略超上限（EXIF 附加的几百字节）。
