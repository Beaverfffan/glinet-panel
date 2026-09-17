# glinet-panel

GL-BE10000 / GL-BE14000 机身 TFT 屏幕在 OpenWrt / X-Wrt 上的支持。

## 分支

- `main` — **luci-app-glinet-panel**：基于 blogic 的
  [glinet-panel-ui](https://github.com/blogic/glinet-panel-ui)
  （ucode + LVGL 面板 UI，见
  [feed-blogic](https://github.com/blogic/feed-blogic)）的 LuCI 控制插件。
- `xwrt-panel` — 自研极简面板（进行中）：按 x-wrt 习惯只统计
  **用量 / 客户端 / WiFi**，不复刻原厂交互。
- `bad-apple` — 把小屏当播放器用：`.xwpm` 媒体包 + `xwpmplay` 播放器（**自带上屏的
  Bad Apple 原片**），绕开 LVGL 直接推 DRM，实测 30 fps 零丢帧。
- **`doom`（本分支）** — 把小屏当游戏机用：**可游玩的触屏 Doom**，见下。

---

# Doom —— 机身小屏上的触屏 Doom（分支 `doom`）

在 GL-BE14000 自己那块 320×240 的屏上玩 Doom，用屏幕摸。

- 引擎：[doomgeneric](https://github.com/ozkl/doomgeneric)（GPL-2.0），pin 在
  `dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284`
- 硬件：320×240 `panel-mipi-dbi`（ST7789P3 系）@ SPI0 52 MHz；触摸是
  Hynitron CST353X 电容屏（**只有单点**）
- 实测：**36.3 fps 上屏**（30 秒 1087 帧，SPI 计数器逐帧吻合），
  game tic 33.7 Hz（标称 35 Hz 的 96%）
- 只依赖 `libdrm`；二进制 550 KB

## 1. 装

```sh
cp -r package/* <你的 x-wrt 树>/package/
# 或当 feed：
echo "src-link gpdoom /path/to/this/repo/package" >> feeds.conf
./scripts/feeds update gpdoom && ./scripts/feeds install -a -p gpdoom

# 选包（菜单在 Utilities -> XWRT）
echo "CONFIG_PACKAGE_xwrt-doom=y"          >> .config
echo "CONFIG_PACKAGE_luci-app-xwrt-doom=y" >> .config
make defconfig
```

`xwrt-doom` 编译时会从 GitHub 拉 doomgeneric 的上游源码（pin commit），
所以**构建机要能上网**，或者事先把 tarball 放进 `dl/`。

## 2. 弄一份 IWAD，然后玩

```sh
xwrt-doom-get-wad                      # 取 shareware 版 doom1.wad（约 4 MB）
uci set xwrt_doom.main.enabled='1'
uci commit xwrt_doom
/etc/init.d/xwrt-doom start
```

也可以直接在 **LuCI → 服务 → 屏幕上的 Doom** 里操作：状态、起停、一键取
IWAD、全部调参项，以及设备上那份玩法/排障文档（页面直接读
`/usr/share/xwrt-doom/README`，所以文档只有一份权威版本）。

**怎么退出来**：点控制条左下的 `MNU` 翻到第 2 页，点最右边红色的 `EXIT`。
原版 Doom 1.9 的菜单里没有 Quit Game，所以这个按钮是唯一的屏幕出口 ——
没有它，"可游玩"就不成立（只能 SSH 进去 kill）。

## 3. 单点屏上怎么"边跑边打"

CST353X 只上报一个触点，所以手指按着 `FIRE` 就没办法同时滑动转向。
三个设计把这个问题绕过去：

1. **锁定式按钮** —— `FIRE` / `RUN` 点一下锁住（底色变琥珀），再点解开。
2. **轻点画面 = 开一枪** —— 按住不到 220 ms 且几乎没移动就算射击。
   这是最顺手的开火方式。
3. **摇杆原点跟随** —— 手指按下的那点就是摇杆原点，推过半径后原点跟着走，
   所以手指滑到屏幕边上也不会"顶住"转不动。

画面区左半屏拖动 = 移动/平移，右半屏拖动 = 转向。

## 4. 三个踩过的坑（都写在代码注释里了）

**① `D_DoomLoop()` 不是死循环。** 它只做一次"起搏"就返回，无限循环必须由
平台层的 `main()` 自己驱动。漏掉它的症状极具迷惑性：一切初始化正常、第一帧
也上了屏，然后进程以**退出码 0 干净退出**。

**② 帧率被"拍频"腰斩。** 引擎一帧等于一个 game tic（28.57 ms / 35 Hz），
所以任何「距上次提交 ≥ 1/fps 才提交」的简单节流门都会和它共振：
`1/30 = 33.3 ms` 只比 28.57 ms 大一点，于是每个提交都要等到第 2 个 tic ⇒
**35/2 = 17.5 fps**（实测 17.43，分毫不差）。改成令牌桶（攒到 85% 预算就
放行）之后，目标 30 就是 30。

**③ DRM master 竞态。** 面板服务 / 媒体播放器和 Doom 抢同一块 DRM plane。
没拿到 master 时 legacy ioctl 全部 EACCES —— **进程照跑、日志一句错都不报、
屏上纹丝不动**。判据只有 SPI 计数器
（`/sys/class/spi_master/spi0/statistics/bytes`，一次全帧提交 = +153,611 字节）。
`/dev/fb0` 恒为全 0，是假信号。

顺带两条：`stop` 服务还不够 —— 面板会被别的 procd 实例 respawn 回来，所以
包里带了个 `screen-guard.sh` 在 Doom 运行期间盯着；以及别用 `pkill`，
BusyBox 上可能没有这个 applet，而重定向掉 stderr 之后"停止"会假装成功。

## 5. 代码在哪

```
package/xwrt-doom/
├── Makefile                  OpenWrt 包（拉 upstream + 编本仓库的平台层）
├── files/
│   ├── etc/config/xwrt_doom  uci 配置（每一项都带注释说明为什么）
│   ├── etc/init.d/xwrt-doom  procd 服务：与面板/媒体互斥、背光、状态文件
│   └── usr/
│       ├── bin/xwrt-doom-get-wad       取 shareware IWAD（校验大小 + md5）
│       └── share/xwrt-doom/
│           ├── README                  设备上的权威文档，LuCI 页面直接显示它
│           └── screen-guard.sh         防止面板服务把 DRM master 抢回去
└── src/                      设备相关的全部代码（本仓库自己的）
    ├── doom_be14000.c        main() + 6 个 DG_* 钩子 + 动作→键码翻译
    ├── panel_out.c           DRM 双 dumb buffer + 交替 drmModeSetPlane
    ├── tinput.c              evdev 单点 → 虚拟摇杆 + 锁定按钮
    ├── ovl.c / ovl.h         底部 40 行控制条：版式/绘制/命中
    ├── actions.h             ACT_* 中立动作层
    └── font5x7.h             5×7 点阵字库
package/luci-app-xwrt-doom/   LuCI 页面
tools/build-doom.sh           开发迭代用：不重编包，直接出 aarch64 二进制
```

## 6. 开发迭代

```sh
# 改完 src/*.c，几十秒拿到新二进制（不重编包、不刷固件）
tools/build-doom.sh /home/beaver/xwrt-master

# 拷上路由器换掉
scp /tmp/xwrt-doom-build/doomgeneric/doomgeneric/doom root@192.168.15.1:/usr/bin/xwrt-doom.new
ssh root@192.168.15.1 'chmod +x /usr/bin/xwrt-doom.new; /etc/init.d/xwrt-doom stop;
    mv /usr/bin/xwrt-doom.new /usr/bin/xwrt-doom; /etc/init.d/xwrt-doom start'
```

前台手测（看引擎自己的分项统计，以及触摸事件回显）：

```sh
/etc/init.d/xwrt-doom stop
xwrt-doom --wad /usr/share/xwrt-doom/doom1.wad --fps 25 --touch-echo --stats
# 收尾会打出：每次循环 / 每帧绘制+提交 / tic 频率 / gametic
```

## License

本仓库的设备相关代码：MIT。
引擎 doomgeneric 与 Doom 源码：GPL-2.0（所以编译出来的二进制是 GPL-2.0）。
本仓库**不附带任何 IWAD**；shareware 版 doom1.wad 由 `xwrt-doom-get-wad`
按需下载，也可换成完全自由的 Freedoom。
