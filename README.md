# glinet-panel — xwrt-panel 分支

GL-BE10000 / GL-BE14000 机身 TFT 屏幕（320x240）的**自研极简统计面板**，
按 x-wrt 习惯做统计，不复刻 GL 原厂交互（无锁屏、无 PIN、无天气）。

基于主线 `ucode-mod-lvgl` 从零实现（ucode + LVGL，DRM 显示 + evdev 触摸），
**自带 Inter 字体**（`files/usr/share/xwrt-panel/fonts/`，OFL-1.1），
**不依赖 `glinet-panel-ui`**。
启动时在屏上显示 **x-wrt 自己的 logo**（取自 `x-wrt/luci` 的 `logo.svg`，
栅格化成 `/usr/share/xwrt-panel/xwrt-logo.png`）。

## 包

- `xwrt-panel/` — 面板本体（ucode 脚本 + procd 服务 + UCI 配置 + logo）
- `luci-app-xwrt-panel/` — 配套 LuCI 设置页（服务 → Stats Panel）

## 页面

| 页面 | 内容 | 数据源 |
|---|---|---|
| Usage | WAN 上下行实时速率、累计总量、28 点滚动柱状图 | `/sys/class/net/<wan>/statistics` |
| System | CPU 占用 + 负载、内存占用、SoC 温度、风扇转速、运行时长 | `/proc/stat`、`/proc/loadavg`、`/proc/meminfo`、`/sys/class/thermal`、`/sys/class/hwmon` |
| Clients | DHCP 租约 + dhcpsnoop 客户端，**每个客户端的实时上下行速率** | `/tmp/dhcp.leases`、`dhcpsnoop`、**`/dev/natflow_userinfo_ctl`** |
| Ports | 每个 wan/lanN/sfp 口的链路状态、协商速率（10G/2.5G/1G…）与实时速率 | `/sys/class/net/*/{carrier,speed,statistics}` |
| WAN | 每个 WAN 接口的协议、地址、在线时长与在线状态（多拨/mwan3 友好） | ubus `network.interface` |
| WiFi | 每个 SSID 的频段 / 加密 / 在线状态 / 终端数，底部总站数 | ubus `network.wireless`、`hostapd.*` |

顶部右侧时钟，底部页面指示点；左右滑动翻页，可配置定时自动翻页；
无触摸超时自动关背光，触摸唤醒。

**每页可单独开关**（至少保留一个）。

## 启动 logo

服务启动时先铺一屏 **x-wrt 标记 + 主机名**，`splash` 秒后自动消失
（默认 3 秒，**触摸立即清除**，设 0 则不显示）。logo 用 PNG，
经 `lv.image_load()` 由 LVGL 的 LodePNG 解码。

## LuCI 设置项

- 服务状态显示、重启 / 停止
- 亮度滑块（10–100%）
- **启动 logo 显示时长（秒，0 = 不显示）**
- 自动翻页间隔（秒，0 = 仅滑动）
- 息屏时间（分钟，0 = 常亮）
- **六个页面各自开关**（Usage / System / Clients / Ports / WAN / WiFi）
- 保存并应用后自动重启面板服务（ucitrack）
- 自带简体中文翻译

## 关于「按客户端速率」

x-wrt 的 **natflow** 已经在维护每个客户端的计数与实时速率，
面板直接读 `/dev/natflow_userinfo_ctl`：

```text
ip,mac,auth_type,auth_status,rule_id,idle_time,
rx_pkts:rx_bytes,tx_pkts:tx_bytes,
rx_speed_pkts:rx_speed_bytes,tx_speed_pkts:tx_speed_bytes,ifname
```

**不需要装 nlbwmon，也不需要打开 conntrack accounting。**
该设备不存在时面板自动退化为只显示设备列表（不报错）。

## 依赖：自包含，不依赖 glinet-panel-ui

- **只吃通用绑定层**：`ucode` + `ucode-mod-lvgl`（提供 `lv.so`，来自
  [blogic/feed-blogic](https://github.com/blogic/feed-blogic)）
  + `ucode-mod-{ubus,uloop,uci,fs}`；内核侧要 `kmod-drm-panel-mipi-dbi`、
  `kmod-backlight-pwm`、`kmod-input-touchscreen-cst353x`。
  `ucode-mod-lvgl` 自己声明 "carries no fonts or images"，本就是给任意应用用的。
- **字体随包分发**：`files/usr/share/xwrt-panel/fonts/` 是 Inter 的 LVGL 二进制字体
  （13 个 `.bin`，含 `inter_light_45`、`inter_semibold_21` 等），
  许可 **OFL-1.1**，见同目录 `OFL.txt`。
- **不依赖 `glinet-panel-ui`**：那是一整套给 GL 原厂固件接入用的 ucode 界面
  （自带 init/config/preinit 与资源），我们不需要它的任何一部分。
- **防御性互斥仍然保留**：万一两个包都被装上，它们驱动同一块 DRM，
  只能二选一 —— 安装 `xwrt-panel` 时会停止并禁用对方，启动前也会先停掉对方。

## 编入固件

```sh
# feeds.conf 增加 blogic feed（只为 lvgl 与 ucode-mod-lvgl）：
src-git blogic https://github.com/blogic/feed-blogic.git
# 本仓库整体作为 feed 接入（或把 xwrt-panel/ 与 luci-app-xwrt-panel/ 拷进源码树 package/）：
src-link xwrtpanel /path/to/glinet-panel

./scripts/feeds update -a && ./scripts/feeds install -a

# .config：
CONFIG_PACKAGE_xwrt-panel=y
CONFIG_PACKAGE_luci-app-xwrt-panel=y
```

License: Apache-2.0（`xwrt-panel` 内置的 Inter 字体为 OFL-1.1，见 `OFL.txt`）
