#
# xwrt-doom —— 在机身小屏上玩触屏 Doom
#
# 引擎用 doomgeneric（上游 ozkl/doomgeneric，按 commit 拉取，不 vendored 进本仓库：
# 它是 GPL-2.0 的独立项目，pin commit 比拷源码更不容易和上游漂移）。
# 平台层在 ./src/ 里，全是本仓库自己的代码：
#
#   doom_be14000.c  main() + doomgeneric 的 6 个 DG_* 钩子 + 动作→键码翻译
#   panel_out.c     DRM 双 dumb buffer + 交替 drmModeSetPlane（不走 /dev/fb0）
#   tinput.c        evdev 单点触摸 → 虚拟摇杆 + 锁定式按钮
#   ovl.c           屏幕底部 40 行的控制条（含 5x7 字库绘制）
#   actions.h       ACT_* 中立动作层（避开 linux/input-event-codes.h 与 doomkeys.h
#                   的 KEY_* 大面积重名）
#
include $(TOPDIR)/rules.mk

PKG_NAME:=xwrt-doom
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/ozkl/doomgeneric.git
PKG_SOURCE_VERSION:=dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
PKG_MIRROR_HASH:=skip
PKG_BUILD_PARALLEL:=1

PKG_MAINTAINER:=Beaverfffan <1056715226@qq.com>
PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/xwrt-doom
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=XWRT
  TITLE:=Touchscreen Doom on the GL-BE14000 body panel
  DEPENDS:=+libdrm
  URL:=https://github.com/Beaverfffan/glinet-panel/tree/doom
endef

define Package/xwrt-doom/description
 Doom you can actually play on the router's own screen.

 The engine is doomgeneric (GPL-2.0, pulled from upstream at a pinned commit);
 everything device specific lives in this package's src/ directory:

   * output  - DRM double buffering with alternating drmModeSetPlane, not
               /dev/fb0, because the fbdev emulation defers and coalesces its
               refresh. 320x240 RGB565 is committed straight to the panel at
               about 36 fps, which is the SPI limit (52 MHz x 153,611 bytes
               per full frame = 26 ms), not a CPU limit.
   * input   - the CST353X panel is single touch, so "run and gun" needs
               latching buttons: tap FIRE to hold it, tap again to release.
               Dragging outside the bar walks the virtual stick and taps fire
               once, which is how you strafe without lifting a finger.
   * overlay - a 40 pixel bar with MNU / RUN / USE / FIRE on page 0 and
               ESC / ENT / Y / N / GAM / MAP / EXIT on page 1. The EXIT button
               matters: vanilla Doom 1.9 has no "Quit Game" in its menu, so
               without it there is no way off the screen but SSH.

 The panel service and the media player fight over the same DRM plane, so the
 init script stops them first and runs a guard that keeps them from coming
 back while Doom is up.

 You need an IWAD. Run "xwrt-doom-get-wad" on the router to fetch the
 freely redistributable shareware doom1.wad, or drop your own doom1.wad /
 freedoom1.wad into /usr/share/xwrt-doom/.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/*.c ./src/*.h ./src/Makefile.be14000 $(PKG_BUILD_DIR)/doomgeneric/
endef

#
# ⚠️ 工具链参数必须走 CFLAGS_EXTRA / LDFLAGS_EXTRA：
#    make 的命令行变量优先级最高，写成 CFLAGS="$(TARGET_CFLAGS)" 会把
#    Makefile.be14000 里的 -DNORMALUNIX -DLINUX -fsigned-char
#    -DDOOMGENERIC_RESX/RESY 全部抹掉，然后上游代码编出一堆莫名其妙的错误。
#
# ⚠️ libdrm 的头文件分两处：xf86drm.h 在 usr/include/，
#    drm_fourcc.h 在 usr/include/libdrm/ —— 两个 -I 都要。
#
define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/doomgeneric -f Makefile.be14000 \
		CC="$(TARGET_CC)" \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		CFLAGS_EXTRA="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
		LDFLAGS_EXTRA="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib" \
		DRM_INC="-I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/libdrm" \
		DRM_LIB="-ldrm"
endef

define Package/xwrt-doom/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/doomgeneric/doom $(1)/usr/bin/xwrt-doom
	$(INSTALL_BIN) ./files/usr/bin/xwrt-doom-get-wad $(1)/usr/bin/xwrt-doom-get-wad

	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/etc/init.d/xwrt-doom $(1)/etc/init.d/xwrt-doom
	$(INSTALL_CONF) ./files/etc/config/xwrt_doom $(1)/etc/config/xwrt_doom

	$(INSTALL_DIR) $(1)/usr/share/xwrt-doom
	$(INSTALL_DATA) ./files/usr/share/xwrt-doom/README \
		$(1)/usr/share/xwrt-doom/README
	$(INSTALL_BIN) ./files/usr/share/xwrt-doom/screen-guard.sh \
		$(1)/usr/share/xwrt-doom/screen-guard.sh
endef

define Package/xwrt-doom/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/xwrt-doom enable >/dev/null 2>&1
	if [ ! -s /usr/share/xwrt-doom/doom1.wad ]; then
		echo "xwrt-doom: 还没有 IWAD。任选一种："
		echo "  1) 在路由器上跑  xwrt-doom-get-wad      # 取 shareware 版（约 4 MB）"
		echo "  2) 自己拷一份 doom1.wad / freedoom1.wad 到 /usr/share/xwrt-doom/"
		echo "  然后用 LuCI（服务 -> 屏幕上的 Doom）或 /etc/init.d/xwrt-doom start 启动。"
	fi
}
exit 0
endef

define Package/xwrt-doom/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/xwrt-doom stop >/dev/null 2>&1
	/etc/init.d/xwrt-doom disable >/dev/null 2>&1
}
exit 0
endef

$(eval $(call BuildPackage,xwrt-doom))
