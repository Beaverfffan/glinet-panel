#
# xwrt-media —— 在 DRM 小屏上播放 .xwpm 媒体包（视频 / 图片序列）
#
# 直接走 DRM（双 dumb buffer + 交替 drmModeSetPlane），不走 /dev/fb0：
# fbdev 的刷新是异步且会被内核合并的，控制不了帧率。
#
include $(TOPDIR)/rules.mk

PKG_NAME:=xwrt-media
PKG_VERSION:=1.0.0
PKG_RELEASE:=2

PKG_MAINTAINER:=Beaverfffan <1056715226@qq.com>
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/xwrt-media
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=XWRT
  TITLE:=Play .xwpm media packs on a DRM/SPI panel
  DEPENDS:=+libdrm
  URL:=https://github.com/Beaverfffan/glinet-panel/tree/bad-apple
endef

define Package/xwrt-media/description
 xwpmplay plays .xwpm media packs (video or image sequences) on a small panel
 driven through DRM, such as the GL-BE14000's 320x240 panel-mipi-dbi TFT.

 It talks to DRM directly (double buffered dumb buffers plus alternating
 drmModeSetPlane) instead of writing /dev/fb0, because the fbdev emulation
 defers and coalesces its refresh -- writing 20 frames back to back pushed
 0 to 3 of them. Packs may be read from a file or from stdin, so a
 gzip-compressed pack can be streamed straight in:

     gunzip -c /usr/share/xwrt-media/badapple.xwpm.gz | xwpmplay - 30

 See /usr/share/xwrt-media/FORMAT.txt for the container layout and for how to
 turn your own videos or images into a pack.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/xwpmplay.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -std=gnu99 \
		-I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/libdrm \
		-o $(PKG_BUILD_DIR)/xwpmplay $(PKG_BUILD_DIR)/xwpmplay.c \
		$(TARGET_LDFLAGS) -ldrm
endef

define Package/xwrt-media/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xwpmplay $(1)/usr/bin/xwpmplay

	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/etc/init.d/xwrt-media $(1)/etc/init.d/xwrt-media
	$(INSTALL_CONF) ./files/etc/config/xwrt_media $(1)/etc/config/xwrt_media

	$(INSTALL_DIR) $(1)/usr/share/xwrt-media
	$(INSTALL_DATA) ./files/FORMAT.md $(1)/usr/share/xwrt-media/FORMAT.md

	# 给用户放自己的媒体包留个位置（LuCI 页面也扫这里）
	$(INSTALL_DIR) $(1)/root/media
endef

define Package/xwrt-media/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	/etc/init.d/xwrt-media enable >/dev/null 2>&1
	echo "xwrt-media: 用 'xwpmplay <file.xwpm> [fps]' 播放；"
	echo "            或把 /etc/config/xwrt_media 的 enabled 打开交给服务管理。"
}
exit 0
endef

$(eval $(call BuildPackage,xwrt-media))
