#!/bin/bash
#
# tools/build-doom.sh —— 在 x-wrt / OpenWrt 的编译机上直接交叉编译 doom
#
# 这是**开发迭代**用的快捷路径：不用重编整个包、不用刷固件，改一行 src/*.c
# 几十秒就能得到新的 aarch64 二进制，scp 到路由器上跑。
# 正式发版还是走 `make package/xwrt-doom/compile`。
#
# 用法：
#   tools/build-doom.sh <x-wrt 源码树> [工作目录]
#
#   例：
#     tools/build-doom.sh /home/beaver/xwrt-master
#     tools/build-doom.sh /home/beaver/xwrt-master /tmp/dbuild
#
# 需要 <x-wrt 树>/staging_dir 里有 aarch64 工具链与 libdrm（即该树已经编过
# 至少一次 target）。工作目录里会检出 doomgeneric（按 pin 的 commit），
# 然后把本仓库 package/xwrt-doom/src/ 覆盖进去编译。
#
set -u

TREE=${1:-}
WORK=${2:-/tmp/xwrt-doom-build}

SHA=dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=$REPO/package/xwrt-doom/src

if [ -z "$TREE" ] || [ ! -d "$TREE/staging_dir" ]; then
	echo "用法: $0 <x-wrt 源码树> [工作目录]" >&2
	echo "  找不到 $TREE/staging_dir —— 这个树得先编过一次 target。" >&2
	exit 1
fi

GCC=$(ls "$TREE"/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc 2>/dev/null | head -1)
TGT=$(ls -d "$TREE"/staging_dir/target-aarch64_cortex-a53_musl 2>/dev/null | head -1)
[ -x "$GCC" ] || { echo "!! 找不到 aarch64 交叉编译器（$TREE/staging_dir/toolchain-*）" >&2; exit 1; }
[ -d "$TGT" ] || { echo "!! 找不到 $TREE/staging_dir/target-aarch64_cortex-a53_musl" >&2; exit 1; }

echo "########## 工具链 ##########"
echo "  gcc    : $GCC"
echo "  target : $TGT"
echo "  工作区 : $WORK"

DG=$WORK/doomgeneric/doomgeneric
if [ ! -d "$WORK/doomgeneric" ]; then
	echo
	echo "########## 检出 doomgeneric @ ${SHA:0:12} ##########"
	mkdir -p "$WORK"
	git clone -q https://github.com/ozkl/doomgeneric.git "$WORK/doomgeneric" || exit 1
fi
(
	cd "$WORK/doomgeneric" || exit 1
	git fetch -q origin "$SHA" 2>/dev/null
	git checkout -q "$SHA" 2>/dev/null || { echo "!! 切不到 $SHA" >&2; exit 1; }
	echo "  实际 HEAD: $(git rev-parse --short=12 HEAD)"
)

echo
echo "########## 摆源码 ##########"
cp "$SRC"/*.c "$SRC"/*.h "$SRC"/Makefile.be14000 "$DG"/ || exit 1
ls "$DG"/doom_be14000.c "$DG"/panel_out.c "$DG"/ovl.c "$DG"/tinput.c \
	"$DG"/actions.h "$DG"/font5x7.h >/dev/null || { echo "!! 平台源码没拷进去" >&2; exit 1; }

echo
echo "########## 编 ##########"
# ⚠️ libdrm 的头文件分两处：xf86drm.h 在 usr/include/，
#    drm_fourcc.h 在 usr/include/libdrm/ —— 两个 -I 都要。
DRM_INC="-I$TGT/usr/include -I$TGT/usr/include/libdrm"
DRM_LIB="-L$TGT/usr/lib -ldrm"

cd "$DG" || exit 1
STAGING_DIR="$TREE/staging_dir" make -f Makefile.be14000 \
	CC="$GCC" CROSS_COMPILE="$(basename "$GCC" | sed 's/gcc$//')" \
	DRM_INC="$DRM_INC" DRM_LIB="$DRM_LIB" clean >/dev/null 2>&1
STAGING_DIR="$TREE/staging_dir" make -f Makefile.be14000 \
	CC="$GCC" CROSS_COMPILE="$(basename "$GCC" | sed 's/gcc$//')" \
	DRM_INC="$DRM_INC" DRM_LIB="$DRM_LIB" -j"$(nproc)" 2>&1 | tail -30
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || { echo "!! make rc=$rc" >&2; exit "$rc"; }

echo
echo "########## 产物 ##########"
ls -la "$DG/doom"
file "$DG/doom"
echo
echo "  拷到路由器："
echo "    scp $DG/doom root@192.168.15.1:/usr/bin/xwrt-doom.new"
echo "    ssh root@192.168.15.1 'chmod +x /usr/bin/xwrt-doom.new && \\"
echo "        /etc/init.d/xwrt-doom stop && \\"
echo "        mv /usr/bin/xwrt-doom.new /usr/bin/xwrt-doom && \\"
echo "        /etc/init.d/xwrt-doom start'"
