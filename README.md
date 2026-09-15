# glinet-panel — xwrt-panel 分支

GL-BE10000 / GL-BE14000 机身 TFT 屏幕（320x240）的**自研极简统计面板**，
按 x-wrt 习惯只统计 **用量 / 客户端 / WiFi**，不复刻 GL 原厂交互
（无锁屏、无 PIN、无天气）。

基于主线 `ucode-mod-lvgl` 从零实现（ucode + LVGL，DRM 显示 + evdev 触摸），
仅借用 [glinet-panel-ui](https://github.com/blogic/feed-blogic) 的字体文件。

## 包

- `xwrt-panel/` — 面板本体（ucode 脚本 + procd 服务 + UCI 配置）
- `luci-app-xwrt-panel/` — 配套 LuCI 设置页（服务 → Stats Panel）

## 页面

| 页面 | 内容 |
|---|---|
| Usage | WAN 上下行实时速率、累计总量、28 点滚动柱状图 |
| Clients | DHCP 租约 + dhcpsnoop 客户端列表（主机名 / MAC / IP） |
| WiFi | 每个 SSID 的频段 / 加密 / 在线状态 / 终端数，底部总站数 |

顶部右侧时钟，底部页面指示点；左右滑动翻页，可配置定时自动翻页；
无触摸超时自动关背光，触摸唤醒。

## LuCI 设置项

- 服务状态显示、重启 / 停止
- 亮度滑块（10–100%）
- 自动翻页间隔（秒，0 = 仅滑动）
- 息屏时间（分钟，0 = 常亮）
- 三个页面各自开关（至少保留一个）
- 保存并应用后自动重启面板服务（ucitrack）
- 自带简体中文翻译

## 与 glinet-panel-ui 的关系

- **编译期**：`xwrt-panel` 依赖 `glinet-panel-ui` 软件包，但只取它的
  字体（`/usr/share/glinet-panel-ui/fonts/*.bin`）。
- **运行期**：两个面板驱动同一块 DRM 显示，**二选一运行**。
  安装 `xwrt-panel` 时会自动停止并禁用 `glinet-panel-ui` 服务；
  `xwrt-panel` 启动前也会先停掉对方。

## 编入固件

```sh
# feeds.conf.default 增加（字体依赖）：
src-git blogic https://github.com/blogic/feed-blogic.git

# 本仓库 xwrt-panel/ 与 luci-app-xwrt-panel/ 复制到源码树 package/ 下
# .config 增加：
CONFIG_PACKAGE_glinet-panel-ui=y
CONFIG_PACKAGE_xwrt-panel=y
CONFIG_PACKAGE_luci-app-xwrt-panel=y
```

License: Apache-2.0
