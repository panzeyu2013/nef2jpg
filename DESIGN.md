# nef2jpg — 设计文档 (v1)

> NEF → JPEG 命令行批量转换工具。macOS arm64（尼康官方 SDK 解码）+ Linux x64（构建占位，解码后端 v2 接入 libraw）。

## 1. 定位与目标

- 命令行工具：单张 / 多张 NEF → JPEG。
- 支持并发（`-j N`），支持 JPEG 质量、目标文件大小等参数调节。
- 双平台发布：macOS arm64、Linux x64。

## 2. 平台与后端策略（v1）

| 平台 | 解码后端 | 状态 |
|---|---|---|
| macOS arm64 | 尼康 Image SDK (NkImgSDK 1.46.0) | ✅ v1 完整支持 |
| Linux x64 | —（SDK 无 Linux 版） | ⚠️ v1 仅构建占位，`decode` 返回"not supported"；v2 接入 libraw |

架构上通过 `Decoder` 抽象接口隔离后端，后续新增 libraw 后端不影响上层。

### macOS 运行前提（SDK 要求，README 中说明）
- SDK 资源：`prm.bin` 需位于可执行文件旁的 `Contents/Resources/prm.bin`（发布包自动带上）。
- SDK 动态库：`libImgSDK.dylib` + `Elm.framework` + boost/tbb/RCSigProc，位于 `lib/`（rpath `@executable_path/lib`）。
- 尼康色彩配置文件（25 个 ICC）需安装到 `/Library/Application Support/Nikon/Profiles/`（SDK `CCMSService` 初始化必需；缺失时 `OpenLibrary` 报 `0x8`）。发布包附安装脚本 `scripts/install_profiles.sh`。

## 3. 功能清单（v1）

### 输入
- 单个 / 多个 `.NEF` 文件路径、shell glob（由 shell 展开或内置展开）。
- 目录输入：`-r` 递归扫描目录内 `.NEF`。
- `--dry-run`：只列出将处理的任务，不执行。

### 处理流水线（每张图）
```
读文件 → SDK 解码(8bit RGB) → 自动旋转(EXIF orientation) → 可选 resize → JPEG 编码 → EXIF 写入 → 落盘
```
- 自动旋转：默认开启（请求 SDK 输出正向图像），`--no-auto-orient` 关闭。
- resize：`--resize <maxdim>` 保持宽高比缩到最大边 ≤ maxdim（双线性）。

### JPEG 输出（libjpeg-turbo：macOS 静态链接，Linux 动态）
| 参数 | 说明 |
|---|---|
| `-q, --quality 0-100` | 固定质量（默认 90） |
| `-s, --target-size SIZE` | 单张目标大小（`500K` / `1M` / `2.5M`），对质量做二分搜索逼近（容差 `-t`，默认 5%） |
| `--max-size SIZE` | 硬上限：超出则降质量重试直到满足或质量到 1（无法满足时告警） |
| `--progressive` | 渐进式 JPEG |
| `--subsampling 420/422/444` | 色度子采样（默认 420） |
| `--no-icc` | 不嵌入 sRGB ICC 配置（默认嵌入） |
| `--suffix STR` | 输出文件名后缀（默认 `_decoded`） |

### 并发与容错
- `-j N` 个 worker 线程；每个 worker 独立完成一张图的整条流水线（解码器/编码器均线程安全、实例独立）。
- 单张失败不中断批处理；结束输出统计（成功/失败/总耗时/总大小）。
- 退出码：`0` 全部成功；`1` 部分失败；`2` 用法错误；`130` 被中断（Ctrl-C，完成当前文件后停止）。

### 元数据
- EXIF 写入：日期时间、相机型号、镜头等（exiv2，LGPL-2.1，动态库随发布包分发）。v1 从 SDK tag 读取（Make/Model/DateTime/LensInfo）并写入输出 JPEG；自动旋转时 Orientation 写 1，避免二次旋转。

### 预留（v1 不实现，接口预留）
- 16-bit TIFF/PNG 输出（`--format` 参数预留）。
- libraw 后端（Linux 真解码）。

## 4. 架构

```
src/
├── main.cpp            入口：参数解析 → 任务收集 → 流水线 → 统计
├── cli.h/.cpp          CLI 解析（零依赖手写）
├── decoder.h           Decoder 抽象接口
├── decoder_sdk.cpp     macOS 尼康 SDK 后端（__APPLE__ 编译）
├── decoder.cpp         后端选择（Linux 占位：提示无可用后端）
├── jpeg_encoder.h/.cpp libjpeg-turbo 封装（setjmp 错误处理/质量/目标大小二分/渐进/子采样/ICC）
├── pipeline.h/.cpp     线程池 + 每图流水线 + 统计 + 输出冲突去重 + 原子落盘
├── resize.h/.cpp       双线性缩图
├── exif.h/.cpp         EXIF 写入（exiv2）；tag 读取在 decoder_sdk.cpp
└── util.h/.cpp         大小解析、glob、目录扫描等
```

## 5. CLI（草案）

```
nef2jpg [选项] <输入...>

  输入
    <input...>            一个或多个 .NEF 文件 / glob / 目录
    -r, --recursive       递归扫描目录
        --dry-run         只预览任务不执行

  输出
    -o, --out-dir DIR     输出目录（默认与输入同目录）
        --overwrite       覆盖已存在文件（默认跳过）
        --suffix STR      输出后缀（默认 _decoded）

  并发
    -j, --jobs N          并发 worker 数（默认 min(CPU核数,8)，内存自动封顶，上限 64）

  JPEG
    -q, --quality 0-100   JPEG 质量（默认 90）
    -s, --target-size S   单张目标大小，如 500K / 1M
        --max-size S      硬上限
    -t, --tolerance PCT   目标大小容差 %（默认 5，上限 100）
        --progressive     渐进式 JPEG
        --subsampling N   420 / 422 / 444（默认 420）
        --no-icc          不嵌入 ICC 配置

  图像
    -w, --resize MAXDIM   最大边缩到 MAXDIM 像素（保持宽高比）
        --no-auto-orient  不按 EXIF 自动旋转

  其他
        --backend NAME    解码后端（v1: sdk | auto）
    -v, --verbose         详细输出
    -h, --help            帮助
```

## 6. 构建与发布

- CMake ≥ 3.24，C++17（`-Wall -Wextra`，macOS 固定 deployment target 13.0）。
- 依赖统一由 **Homebrew** 管理（不 clone、不 FetchContent 下载源码）：
  - `brew install libjpeg-turbo exiv2`
  - CMake 通过 `find_package(JPEG)` / `find_package(exiv2 CONFIG)` 查找。
  - macOS 上 libjpeg 静态链接（`libjpeg.a`）；exiv2 为动态库，随发布包分发。
- 尼康 SDK：路径通过 `NIKON_SDK_DIR` 指定（默认 `../Nikon_image/Library/Mac`），仅 macOS 编译；目录缺失时 CMake 直接报错。
- **自包含打包**：`scripts/fixup_macos.sh` 把 Homebrew 动态依赖（exiv2、brotli、inih、gettext 等）收集进 `lib/` 并把 install name 改写为 `@rpath`，发布包解压即用、不依赖目标机 Homebrew。
- 发布布局（tar.gz，本地构建后手动上传 GitHub Release，无 CI）：
  ```
  nef2jpg-<os>-<arch>/
  ├── nef2jpg
  ├── lib/                    SDK 动态库 + Elm.framework + exiv2 及依赖
  ├── Contents/Resources/     prm.bin
  ├── Profiles/               尼康色彩配置文件（25 个，供 install_profiles.sh）
  ├── scripts/install_profiles.sh
  └── README.md
  ```
- 发布流程：`cmake --build build` → 按上述布局打包 → 手动创建 GitHub Release 上传。

## 7. 里程碑

| # | 内容 | 状态 |
|---|---|---|
| M1 | 项目骨架 + CLI + 单张/多张 NEF→JPEG（Mac SDK）+ 并发 | ✅ |
| M2 | 目标大小二分 + max-size + 渐进/子采样/ICC | ✅ |
| M3 | resize + 自动旋转 + EXIF | ✅ |
| M4 | 统计/退出码/递归/dry-run + Linux 占位构建 + CI 发布 + README | ✅ |
| M5 | 审查修复：setjmp 错误处理/尺寸校验/输出冲突/原子写入/信号/自包含打包 | ✅ |
