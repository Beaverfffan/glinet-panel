include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for the GL.iNet TFT panel (glinet-panel-ui)
LUCI_DESCRIPTION:=Control the TFT panel on GL-BE10000 / GL-BE14000: \
 brightness, background, idle behaviour, page order and service state.
LUCI_DEPENDS:=+luci-base +glinet-panel-ui
LUCI_PKGARCH:=all

PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Beaverfffan <1056715226@qq.com>

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
