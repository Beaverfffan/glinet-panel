#!/bin/sh
#
# screen-guard.sh —— Doom 活着的时候，不让面板/媒体服务把 DRM master 抢回去。
#
# 为什么需要它（这是踩过的坑，不是预防性设计）：
#   `/etc/init.d/xwrt-panel stop` 能停掉服务，但实测 ucode 面板还会被别的
#   procd 实例或 respawn 再拉起来。一旦面板拿回 master，我们的
#   drmModeSetPlane 全部返回 EACCES —— 表现极具迷惑性：
#     **doom 进程正常运行、日志一句错都不报、屏上却纹丝不动**
#     （判据：/sys/class/spi_master/spi0/statistics/bytes 不涨）。
#
# ⚠️ 只杀 xwrt-panel 的那个 ucode（按命令行匹配），不要 `killall ucode`：
#    系统里可能有别的 ucode 用户（rpcd 的脚本等）。
#
# ⚠️ 不用 pkill：BusyBox 上可能没有这个 applet，而我们又把 stderr 重定向了，
#    会得到"悄悄什么都没做"的结果。一律 pgrep + kill。
#
PANEL_UC='xwrt-panel/panel.uc'
MAX=3600

i=0
while [ "$i" -lt "$MAX" ]; do
	for p in $(pgrep -f "$PANEL_UC" 2>/dev/null); do
		kill "$p" 2>/dev/null
	done
	for p in $(pgrep -x xwpmplay 2>/dev/null); do
		kill "$p" 2>/dev/null
	done

	# doom 不在了就收工（避免自己变成常驻进程）
	pgrep -x xwrt-doom >/dev/null 2>&1 || break

	sleep 1
	i=$((i + 1))
done
exit 0
