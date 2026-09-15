# glinet-panel

GL-BE10000 / GL-BE14000 机身 TFT 屏幕在 OpenWrt / X-Wrt 上的支持。

## 分支

- `main` — **luci-app-glinet-panel**：基于 blogic 的
  [glinet-panel-ui](https://github.com/blogic/glinet-panel-ui)
  （ucode + LVGL 面板 UI，见
  [feed-blogic](https://github.com/blogic/feed-blogic)）的 LuCI 控制插件。
- `xwrt-panel` — 自研极简面板（进行中）：按 x-wrt 习惯只统计
  **用量 / 客户端 / WiFi**，不复刻原厂交互。

## luci-app-glinet-panel

菜单位置：服务 → Panel Screen（屏幕面板）。

- 服务状态显示、重启 / 停止
- 亮度滑块（10–100%）、背景渐变（纯黑 + 8 种）、卡片不透明度
- 自动锁屏时间、锁屏表现（时钟 / 息屏）、6 位解锁 PIN、测试页
- 天气页经纬度
- 页面顺序（流量 / 天气 / WiFi / 二维码 / 客户端 / 系统 / 重启等 12 页）
- 保存并应用后自动重启面板服务（ucitrack）
- 自带简体中文翻译

### 编入固件

```sh
# feeds.conf.default 增加：
src-git blogic https://github.com/blogic/feed-blogic.git

# 本仓库 luci-app-glinet-panel/ 复制到源码树 package/ 下
# .config 增加：
CONFIG_PACKAGE_glinet-panel-ui=y
CONFIG_PACKAGE_luci-app-glinet-panel=y
```

### 路由器直装（测试用）

前提是已安装 glinet-panel-ui。把
`luci-app-glinet-panel/{htdocs,root}` 的内容拷到 `/www` 和 `/`，
然后：

```sh
rm -f /tmp/luci-indexcache /tmp/luci-modulecache/*
/etc/init.d/rpcd restart
```

License: Apache-2.0
