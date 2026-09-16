include $(TOPDIR)/rules.mk

PKG_NAME:=xwrt-panel
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Beaverfffan <1056715226@qq.com>

include $(INCLUDE_DIR)/package.mk

define Package/xwrt-panel
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Minimal statistics panel for the GL-BE10000/BE14000 TFT
  DEPENDS:=+ucode +ucode-mod-lvgl +ucode-mod-ubus +ucode-mod-uloop \
    +ucode-mod-uci +ucode-mod-fs +glinet-panel-ui
  PKGARCH:=all
endef

define Package/xwrt-panel/description
 X-Wrt style statistics panel for the 320x240 TFT found on the
 GL-BE10000 and GL-BE14000. Three swipeable pages: WAN usage,
 connected clients and WiFi status. No lock screen, no PIN, no
 weather. Borrows only the fonts from glinet-panel-ui; the two
 panel services are mutually exclusive at run time.
endef

define Build/Compile
endef

define Package/xwrt-panel/install
	$(INSTALL_DIR) $(1)/usr/share/ucode/xwrt-panel
	$(INSTALL_DATA) ./files/usr/share/ucode/xwrt-panel/panel.uc \
		$(1)/usr/share/ucode/xwrt-panel/panel.uc
	$(INSTALL_DIR) $(1)/usr/share/xwrt-panel
	$(INSTALL_DATA) ./files/usr/share/xwrt-panel/xwrt-logo.png \
		$(1)/usr/share/xwrt-panel/xwrt-logo.png
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/xwrt_panel $(1)/etc/config/xwrt_panel
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/xwrt-panel $(1)/etc/init.d/xwrt-panel
endef

define Package/xwrt-panel/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
# glinet-panel-ui and xwrt-panel drive the same display: never run both
if [ -x /etc/init.d/glinet-panel-ui ] && /etc/init.d/glinet-panel-ui enabled; then
	/etc/init.d/glinet-panel-ui stop
	/etc/init.d/glinet-panel-ui disable
	echo "xwrt-panel: glinet-panel-ui stopped and disabled (only one panel can own the display)"
fi
/etc/init.d/xwrt-panel enable
/etc/init.d/xwrt-panel start
exit 0
endef

$(eval $(call BuildPackage,xwrt-panel))
