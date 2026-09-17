# .xwpm 媒体包格式（XWRT Panel Media）

给 `xwpmplay` 播放的帧序列容器。设计目标是**极简 + 自描述**：一个头 + 一串帧，
没有索引表，顺序读就能播，所以可以 `gzip` 压缩后**边解压边播**（见文末）。

所有多字节整数都是**小端**。

---

## 1. 文件布局

```
┌──────────────────────┬──────────┐
│ 头部 24 字节         │ 偏移 0   │
├──────────────────────┼──────────┤
│ 调色板 512 字节      │ 偏移 24  │  ← 仅当 flags 的 bit0 = 1
├──────────────────────┼──────────┤
│ 帧 0                 │          │
│ 帧 1                 │          │  每帧 frame_bytes 字节
│ …                    │          │
│ 帧 (frames-1)        │          │
└──────────────────────┴──────────┘
```

`frame_bytes` = `ceil(width × bpp / 8) × height`
（每行按字节对齐，所以 `width × bpp` 不是 8 的倍数时行尾会有填充位，解码时应忽略）

## 2. 头部（24 字节）

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|---|---|---|---|---|
| 0 | 4 | char[4] | `magic` | 固定 `"XWPM"` |
| 4 | 2 | u16 | `version` | 目前 `1` |
| 6 | 2 | u16 | `header_size` | `24`（为将来扩展留的） |
| 8 | 2 | u16 | `width` | 像素宽 |
| 10 | 2 | u16 | `height` | 像素高 |
| 12 | 1 | u8 | `bpp` | **只能是 1 / 2 / 4 / 8 / 16** |
| 13 | 1 | u8 | `flags` | bit0 = 有调色板；bit1 = 建议循环 |
| 14 | 2 | u16 | `fps` | 播放帧率（播放器可用命令行覆盖） |
| 16 | 4 | u32 | `frames` | 总帧数（至少 1） |
| 20 | 2 | u16 | `palette_count` | 调色板项数（通常 256） |
| 22 | 2 | u16 | `reserved` | 必须为 0 |

## 3. 调色板（仅当 flags bit0 = 1）

固定 **256 项 × u16 = 512 字节**，每项是 **RGB565**：

```
bit 15..11 = R (5 bit)
bit 10.. 5 = G (6 bit)
bit  4.. 0 = B (5 bit)
```

不足 256 项的部分补 0。实际用几项由 `bpp` 决定（`1<<bpp` 项有效）。

**没有调色板时**（bpp ≤ 8 且 flags bit0 = 0），播放器会**自动生成 2^bpp 级灰阶**：
第 i 项 = `i × 255 / (2^bpp − 1)` 转 RGB565。所以只想放黑白/灰度图的话，
连调色板都不用写。

## 4. 帧数据

**每字节内的像素都是高位在前**（左上角是第 0 个像素），行优先。

| bpp | 每字节像素数 | 每字节位分配（px0 在最高位） |
|---|---|---|
| 16 | 1 | 直接是 RGB565 小端（2 字节/像素） |
| 8 | 1 | `px0` = bit 7..0（整字节就是调色板索引） |
| 4 | 2 | `px0` = bit 7..4，`px1` = bit 3..0 |
| 2 | 4 | `px0` = bit 7..6，`px1` = 5..4，`px2` = 3..2，`px3` = 1..0 |
| 1 | 8 | `px0` = bit 7，`px1` = bit 6，… `px7` = bit 0 |

`bpp = 16` 时不带调色板，数据就是 RGB565 原始像素（和 DRM 的 `DRM_FORMAT_RGB565`
一致，小端直写）。

## 5. 用 `tools/xwpm-encode.py` 生成

依赖：`ffmpeg`（视频）、`python3` + `numpy`（必需）、`Pillow`（只处理图片时需要）。

```bash
# 视频 → 320x240 @30fps 4 级灰，带 4x4 Bayer 抖动（体积最小，适合 Bad Apple 这类高对比动画）
./tools/xwpm-encode.py movie.mp4 -o movie.xwpm --size 320x240 --fps 30 --bpp 2

# 视频 → 全彩 RGB565（最清晰，体积是上面的 8 倍）
./tools/xwpm-encode.py movie.mp4 -o movie.xwpm --size 320x240 --fps 24 --bpp 16

# 单张图片 → 停留 5 秒（30fps × 5 = 150 帧）
./tools/xwpm-encode.py logo.png -o logo.xwpm --fps 30 --hold 150 --bpp 4

# 一个目录里的图片 → 幻灯片（按文件名排序，每张 1 帧）
./tools/xwpm-encode.py ./frames/ -o anim.xwpm --fps 12 --bpp 8
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--size WxH` | 输出尺寸，默认 `320x240`。**务必与面板一致**（见 §6） |
| `--fps N` | 帧率，默认 30 |
| `--bpp 1\|2\|4\|8\|16` | 位深。2 = 4 级灰（推荐）、16 = 全彩 |
| `--fit stretch\|fit\|crop` | 宽高比不符时：拉伸 / 加黑边 / 居中裁剪。默认 `stretch` |
| `--hold N` | 静态图（或图片序列的每张）重复几帧 |
| `--no-dither` | 关掉抖动（大色块素材可能更干净） |
| `--start S` `--dur S` | 视频裁剪 |
| `--max-frames N` | 只取前 N 帧 |
| `--loop-hint` | 在头里标记"建议循环" |
| `--dump N out.ppm` | 导出第 N 帧为 PPM（P6），**不碰 DRM**，可在没有屏的机器上跑，
  用来核对转码结果是否被正确解码 |

体积速查（320×240，1 秒 = fps 帧）：

| bpp | 每帧 | 30fps 每秒 | 219 秒（Bad Apple 全长） |
|---|---|---|---|
| 1 | 9,600 B | 288 KB | 63 MB |
| 2 | 19,200 B | 576 KB | **126 MB** |
| 4 | 38,400 B | 1.15 MB | 252 MB |
| 8 | 76,800 B | 2.3 MB | 505 MB |
| 16 | 153,600 B | 4.6 MB | 1.0 GB |

> 别忘了 **gzip**：Bad Apple 的 2bpp 包 126 MB 压完只有 **8.4 MB**（约 15:1），
> 高对比动画的压缩率非常高。

## 6. 分辨率、帧率与硬件上限

- **分辨率**：面板是 320×240。拍得比它大，右下会被裁掉；比它小，只填左上角。
  播放器以**面板的 mode**建 framebuffer，媒体内容按左上对齐贴进去。
- **帧率上限**由 SPI 时钟决定：
  `SPI_HZ / 8 ÷ (width × height × 2 + 11)`。
  BE14000 的屏是 **52 MHz ⇒ 约 42 fps**。30fps 时 SPI 占用约 71%，很稳。
- 判据（不是"看着像在动"）：`/sys/class/spi_master/spi0/statistics/bytes`。
  一次 commit = `width × height × 2 + 11` 字节。播放器会把
  "提交帧数 vs 实际上屏帧数"打出来，两者相等才算没丢帧。

## 7. 在设备上播放

```sh
# 直接播
xwpmplay /root/media/movie.xwpm            # 用容器里的 fps
xwpmplay /root/media/movie.xwpm 24 --loop  # 覆盖 fps + 循环
xwpmplay /root/media/movie.xwpm --info     # 只看头信息
xwpmplay /root/media/movie.xwpm --dump 300 frame300.ppm
                                           # 导第 300 帧为 PPM，核对转码结果

# 压缩包边解压边播（不落地，省存储）
gunzip -c /usr/share/xwrt-media/badapple.xwpm.gz | xwpmplay - 30

# 交给服务管理（开机自启 / 页面控制）
uci set xwrt_media.main.file='/root/media/movie.xwpm'
uci set xwrt_media.main.gzip='0'
uci set xwrt_media.main.enabled='1'
uci commit xwrt_media
/etc/init.d/xwrt-media start
```

⚠️ **播放前必须停掉面板服务**（`/etc/init.d/xwrt-panel stop`）：两者抢同一块 DRM plane。
init 脚本里的 `stop_panel` / `restore_panel` 就是干这个的。

⚠️ 关掉息屏（`uci set xwrt_panel.@panel[0].blank=0`），否则播到一半背光会被关。

## 8. 参考实现

| 文件 | 说明 |
|---|---|
| `src/xwpmplay.c` | 播放器（DRM 双缓冲 + 交替 `drmModeSetPlane`；读文件或 stdin） |
| `tools/xwpm-encode.py` | 编码器（视频 / 图片 / 图片目录 → `.xwpm`） |
| `tools/make-badapple.sh` | 从 `media/bad_apple_video.mp4` 重做 Bad Apple 包 |
