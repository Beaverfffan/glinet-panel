# bad-apple —— 机身小屏媒体播放（xwrt-media）

在路由器**机身小屏**上播放视频 / 动画 / 图片序列。分支里自带完整的
**Bad Apple!!**（原片 + 转好的媒体包），以及把它换成你自己内容的全部工具和文档。

- 目标硬件：**GL.iNet GL-BE14000** 的 320×240 MIPI-DBI 屏（`panel-mipi-dbi` / ST7789 系），
  SPI0 @ 52 MHz
- 实测：**Bad Apple 6,572 帧 / 219.07 秒 = 30.00 fps，一帧不丢**
- 只依赖 `libdrm`；播放器 75 KB

---

## 1. 装

把 `package/` 下的三个包放进你的 OpenWrt / x-wrt 树（或作为一个 feed），然后选上：

| 包 | 内容 |
|---|---|
| `xwrt-media` | `xwpmplay` 播放器 + init 脚本 + uci 配置 + 格式文档 |
| `xwrt-media-badapple` | Bad Apple 媒体包（320×240 / 30fps / 2bpp，gzip 后约 8 MB） |
| `luci-app-xwrt-media` | LuCI 页面：状态 / 控制 / **媒体格式说明 / 一键换片** |

```sh
# 直接拷进树
cp -r package/* <你的树>/package/
# 或者当 feed 用
echo "src-link badapple /path/to/this/repo/package" >> feeds.conf
./scripts/feeds update badapple && ./scripts/feeds install -a -p badapple

# 选包（菜单在 Utilities -> XWRT）
echo "CONFIG_PACKAGE_xwrt-media=y"            >> .config
echo "CONFIG_PACKAGE_xwrt-media-badapple=y"   >> .config
echo "CONFIG_PACKAGE_luci-app-xwrt-media=y"   >> .config
make defconfig
```

> `xwrt-media-badapple` 会从本仓库的 `bad-apple` 分支下载媒体包
> （`media/xwrt-media-badapple-1.0.0.tar.gz`）。所以**构建机要能上网**，
> 或者事先把该文件放进 `dl/`。

## 2. 用

```sh
# 直接播（用容器里的 fps）
xwpmplay /root/media/movie.xwpm

# 覆盖 fps / 循环 / 只看头信息
xwpmplay /root/media/movie.xwpm 24 --loop
xwpmplay /root/media/movie.xwpm --info
xwpmplay /root/media/movie.xwpm --dump 300 frame300.ppm  # 导一帧成 PPM（不需要屏）

# 压缩包不落地，边解压边播
gunzip -c /usr/share/xwrt-media/badapple.xwpm.gz | xwpmplay - 30

# 交给服务管理（开机自启 + 播完自动把面板拉回来）
uci set xwrt_media.main.file='/usr/share/xwrt-media/badapple.xwpm.gz'
uci set xwrt_media.main.gzip='1'
uci set xwrt_media.main.enabled='1'
uci set xwrt_media.main.loop='1'
uci commit xwrt_media
/etc/init.d/xwrt-media start     # stop / restart / reload 同理
```

⚠️ **播放前必须停掉面板服务**（`/etc/init.d/xwrt-panel stop`）：两者抢同一块 DRM plane。
init 脚本里的 `stop_panel` / `restore_panel` 就是干这个的，用手敲命令时记得自己停。
⚠️ 顺手把息屏关掉：`uci set xwrt_panel.@panel[0].blank=0`，否则播一半背光会被关。

LuCI 里在 **服务 → 媒体播放**：状态、播放/停止、设置、设备上已有媒体包列表，
以及完整的**媒体格式说明**（页面直接读设备上的 `/usr/share/xwrt-media/FORMAT.md`，
所以文档和设备上的永远一致）。

## 3. 换成你自己的图片或视频

**需要提前转码**：路由器上没有解码器（没有 ffmpeg），所以要
**在电脑上**把视频/图片转成 `.xwpm` 容器，再拷进设备。

### 3.1 依赖

- `ffmpeg`（处理视频）
- `python3` + `numpy`（必需）
- `Pillow`（只处理图片时需要）

### 3.2 转码

```bash
# 视频 → 320x240 @30fps 4 级灰（体积最小；Bad Apple 就是这么来的）
./tools/xwpm-encode.py movie.mp4 -o movie.xwpm --size 320x240 --fps 30 --bpp 2

# 视频 → 全彩 RGB565（最清晰，体积是上面的 8 倍）
./tools/xwpm-encode.py movie.mp4 -o movie.xwpm --size 320x240 --fps 24 --bpp 16

# 单张图片 → 停留 5 秒
./tools/xwpm-encode.py logo.png -o logo.xwpm --fps 30 --hold 150 --bpp 4

# 一个目录的图片 → 幻灯片（按文件名排序）
./tools/xwpm-encode.py ./frames/ -o anim.xwpm --fps 12 --bpp 8
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--size WxH` | 默认 `320x240`，**要和面板一致** |
| `--fps N` | 默认 30 |
| `--bpp 1\|2\|4\|8\|16` | 2 = 4 级灰（推荐）、16 = 全彩 |
| `--fit stretch\|fit\|crop` | 拉伸 / 加黑边 / 居中裁剪 |
| `--hold N` | 静态图重复几帧 |
| `--no-dither` | 关抖动 |
| `--start / --dur / --max-frames` | 裁剪 |

### 3.3 拷进设备并播放

```sh
gzip -9 movie.xwpm                                    # 高对比动画能压 10 倍以上
scp movie.xwpm.gz root@192.168.15.1:/root/media/
ssh root@192.168.15.1 'gunzip -c /root/media/movie.xwpm.gz | xwpmplay - 30'
```

想要**不压缩**（占 eMMC 换 CPU）：直接拷 `.xwpm`，然后
`xwpmplay /root/media/movie.xwpm 30`，并把 uci 的 `gzip` 设为 `0`。

### 3.4 从头重做 Bad Apple

```sh
./tools/make-badapple.sh                 # 用 media/bad_apple_video.mp4
./tools/make-badapple.sh --bpp 16 --fps 24   # 换参数
```
它会产出 `badapple.xwpm` / `.gz` / 给包用的 `tar.gz`，并打印该填进
`package/xwrt-media-badapple/Makefile` 的 `PKG_HASH`。

## 4. 容器格式

`.xwpm` 就一个字：**24 字节头 + （可选）512 字节调色板 + 顺序帧**，没有索引表，
所以能边解压边播。完整规范见
**[`package/xwrt-media/files/FORMAT.md`](package/xwrt-media/files/FORMAT.md)**
（设备上装在 `/usr/share/xwrt-media/FORMAT.md`，LuCI 页面就是读它渲染的）。

一图流：

```
偏移 0   24 B   头部：magic "XWPM" / version / header_size / width / height /
                       bpp(1|2|4|8|16) / flags / fps / frames / palette_count
偏移 24 512 B   调色板（仅 flags bit0=1）：256 × RGB565（小端）
偏移 536 ...    帧 0..N-1，每帧 ceil(width*bpp/8)*height 字节，行优先、高位在前
```

单帧字节数：`bpp=2` 时 320×240 → **19,200 B**；`bpp=16` → 153,600 B。

## 5. 为什么不用 `/dev/fb0`

`/dev/fb0` 在（`panel-mipi-dbid`，320×240/bpp16），**写它也确实能推帧** —— 实测写一次触发
`+460,833 B` = 正好 3 整帧。但 DRM 的 fbdev 模拟是**影子缓冲 + 异步脏页刷新**，
连写 20 帧只会推出 **0~3 帧**，你决定不了"这一帧什么时候出去"，做不了视频。

所以 `xwpmplay` 直接走 DRM：**两个 dumb buffer + 交替 `drmModeSetPlane`**
（交替 fb 才能让内核认为"有变化"，每次 commit 都真推一帧）。
另外必须设 `DRM_CLIENT_CAP_UNIVERSAL_PLANES`，否则 `drmModeGetPlaneResources()`
返回空列表，会误判成"没有可用 plane"。

判据永远是 SPI 计数器 —— `/sys/class/spi_master/spi0/statistics/bytes`，
一次全帧 commit = `width*height*2 + 11` 字节。播放器会把
"提交帧数 vs 实际上屏帧数"打出来，**两者相等才算没丢帧**。

## 6. 仓库结构

```
package/xwrt-media/src/xwpmplay.c  播放器（DRM 直驱，读文件或 stdin）
package/xwrt-media/files/FORMAT.md .xwpm 格式规范（设备上也装一份，LuCI 页面读它）
tools/xwpm-encode.py               转码器（视频 / 图片 / 图片目录 → .xwpm）
tools/make-badapple.sh             重做 Bad Apple 包
media/bad_apple_video.mp4          Bad Apple 原片（480x360 30fps h264，7.3 MB）
media/xwrt-media-badapple-1.0.0.tar.gz   Bad Apple 媒体包（内含 badapple.xwpm.gz，约 8 MB）
                                         想单独拿到 .gz：tar xzf 它就出来了
package/xwrt-media/                播放器包
package/xwrt-media-badapple/       媒体数据包
package/luci-app-xwrt-media/       LuCI 页面
```

> 源码放在**包目录内**（`package/xwrt-media/src/`）而不是仓库根的 `src/`：
> OpenWrt 把 feed 装进 `package/feeds/<feed>/` 时可能是复制而不是软链，
> 包 Makefile 里写 `../../src` 会找不到文件。

> `media/badapple.xwpm`（126 MB 未压缩）与 `media/badapple.xwpm.gz` 不进 git ——
> 它们是 `tools/make-badapple.sh` 的产物，随时可以重做。
> 仓库里那个 8 MB 的 `tar.gz` 就是给 OpenWrt 包下载用的，解开即是 `badapple.xwpm.gz`。

## 7. 版权

- **代码**（`src/`、`tools/`、`package/` 里的脚本）：MIT，见 [`LICENSE`](LICENSE)。
- **`media/bad_apple_video.mp4` 与由它生成的媒体包**：**不属于** MIT。
  Bad Apple!! 的原始词曲与影像版权归其原作者（ZUN / Alstroemeria Records 及
  该动画的原作者）所有。这里仅为个人设备上的技术演示而附带，
  请勿用于商业用途。要用在公开场合，请自行确认授权，或换成你有权使用的素材
  （用 §3 的工具转码即可）。

## 排障

| 现象 | 原因 / 处理 |
|---|---|
| `! drmModeSetPlane: Permission denied`，日志 `0 帧` | 面板（LVGL）还占着 DRM master。播放器现在会等最多 4 s 抢 master；仍失败就先 `/etc/init.d/xwrt-panel stop`。正常启动服务（`stop_panel=1`）会自动停面板 |
| 画面被压在上半屏 / 出现重复条带 | 旧版（1.0.0-r1）的行寻址 bug，已在 r2 修掉。用 `xwpmplay <file> --dump <n> out.ppm` 导出单帧自检 |
| 屏全白、SPI 计数不动 | 面板没被初始化：换 `/lib/firmware/<compatible>.bin` 后 `echo spi0.0 > /sys/bus/spi/drivers/panel-mipi-dbi-spi/{unbind,bind}` |
| 播完面板没回来 | `restore_panel=0`。恢复：`/etc/init.d/xwrt-panel start` |
