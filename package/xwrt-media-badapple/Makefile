#
# xwrt-media-badapple —— Bad Apple 媒体包（320x240 / 30fps / 2bpp / 219s）
#
# 只装数据，不编译。数据从本仓库的 bad-apple 分支下载（打包成 tar.gz，
# 这样走 OpenWrt 标准的下载+解包流程，不需要自定义 Build/Prepare）。
#
# 想换自己的片子：用 tools/xwpm-encode.py 生成 .xwpm.gz，改下面的
# PKG_SOURCE / PKG_HASH，或者干脆别用这个包，直接把文件塞进 /root/media/。
#
include $(TOPDIR)/rules.mk

PKG_NAME:=xwrt-media-badapple
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=Beaverfffan <1056715226@qq.com>
PKG_LICENSE:=NOASSERTION
PKG_LICENSE_FILES:=README.md

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://raw.githubusercontent.com/Beaverfffan/glinet-panel/bad-apple/media/
PKG_HASH:=fee8713fa44bae8d4dd10606bff86c66887a4223fef3874875dfea0f6953207c

include $(INCLUDE_DIR)/package.mk

define Package/xwrt-media-badapple
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=XWRT
  TITLE:=Bad Apple media pack for xwrt-media (320x240 30fps 2bpp)
  DEPENDS:=xwrt-media
  URL:=https://github.com/Beaverfffan/glinet-panel/tree/bad-apple
endef

define Package/xwrt-media-badapple/description
 The complete Bad Apple!! animation as an .xwpm pack: 320x240, 30 fps, 2 bpp
 (4 level grey with ordered dither), 6572 frames / 219 s, gzip compressed to
 about 8 MB. Plays full speed on the GL-BE14000's 320x240 MIPI-DBI panel:

     gunzip -c /usr/share/xwrt-media/badapple.xwpm.gz | xwpmplay - 30

 The original source video is kept in the repository under media/ together
 with tools/xwpm-encode.py, so the pack can be regenerated at any time.
endef

define Build/Compile
endef

define Package/xwrt-media-badapple/install
	$(INSTALL_DIR) $(1)/usr/share/xwrt-media
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/badapple.xwpm.gz \
		$(1)/usr/share/xwrt-media/badapple.xwpm.gz
endef

$(eval $(call BuildPackage,xwrt-media-badapple))
