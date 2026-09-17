#!/bin/sh
#
# screen-guard.sh —— Doom 活着的时候，不让面板/媒体服务把 DRM master 抢回去。
#
# 为什么需要它（这是踩出来的，不是预防性设计）：
#   init 里的 panel_stop() 已经把 procd 实例注销了，但现场还有两条路会让面板回来：
#     ① xwrt-panel 带 `procd_set_param respawn`（retry=5）—— 只要实例还在，
#        杀掉 ucode 5 秒后必被拉回来；
#     ② /etc/init.d/xwrt-media 的收尾动作会顺手 `/etc/init.d/xwrt-panel start`。
#   一旦面板拿回 master，我们的 drmModeSetPlane 全部 EACCES/EBUSY ——
#   表现极具迷惑性：
#     **doom 进程正常运行、日志只有几行 commit 失败、屏上纹丝不动**
#
#   ⚠️ 判据要小心：面板空转刷新恰好也是 **每秒 +153,611 B**（一次全帧），
#      很容易被误读成"Doom 在跑"。真 Doom 是 +153,611 × 36/s。
#      量法：/sys/class/spi_master/spi0/statistics/bytes 隔 1 秒取两次。
#
# ⚠️ 认 doom 必须用锚定的绝对路径，不能用 `pgrep -x xwrt-doom`：
#    BusyBox 把 `/bin/sh /etc/rc.common /etc/init.d/xwrt-doom <动作>` 这种进程的
#    comm 也报成 "xwrt-doom"，会让守卫把 init 包装误判成 doom（真机实测）。
#
# ⚠️ 不用 pkill：BusyBox 上可能没有这个 applet，而我们又把 stderr 重定向了，
#    会得到"悄悄什么都没做"的结果。一律 pgrep + kill。
#
# ⚠️ 杀面板一律 kill -9：panel.uc 在 procd 的 term_timeout(5s) 内不退，
#    procd 日志里那句 "not stopped on SIGTERM, sending SIGKILL instead" 就是证据。
#
DOOM_RE='^/usr/bin/xwrt-doom'
PANEL_UC='xwrt-panel/panel.uc'
MAX=3600

i=0
while [ "$i" -lt "$MAX" ]; do
	# doom 不在了就收工（别把自己变成常驻进程，也别去杀刚被还原的面板）
	pgrep -f "$DOOM_RE" >/dev/null 2>&1 || break

	if pgrep -f "$PANEL_UC" >/dev/null 2>&1 || pgrep -x xwpmplay >/dev/null 2>&1; then
		# 再确认一次 doom 还在，避免恰好在它退出的瞬间把面板又杀一遍
		pgrep -f "$DOOM_RE" >/dev/null 2>&1 || break

		# 先注销 procd 实例（否则 respawn 会无限拉回来），再补 SIGKILL
		/etc/init.d/xwrt-panel stop >/dev/null 2>&1
		for p in $(pgrep -f "$PANEL_UC" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
		for p in $(pgrep -x xwpmplay 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
	fi

	sleep 1
	i=$((i + 1))
done
exit 0
