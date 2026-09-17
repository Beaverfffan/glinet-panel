#!/bin/sh
#
# make-badapple.sh —— 从 media/bad_apple_video.mp4 重新生成 Bad Apple 媒体包
#
# 产出（都放在 media/）：
#   badapple.xwpm                      未压缩的容器，方便本地检查/直接播放
#   badapple.xwpm.gz                   gzip 版，设备上边解压边播用这个
#   xwrt-media-badapple-1.0.0.tar.gz   给 OpenWrt 包下载用（内含 .gz）
#
# 用法：  ./tools/make-badapple.sh [--bpp N] [--fps N] [--size WxH]
#
set -eu

cd "$(dirname "$0")/.."
ROOT=$(pwd)
SRC=media/bad_apple_video.mp4
OUT=badapple.xwpm
PKG=xwrt-media-badapple-1.0.0.tar.gz

BPP=2
FPS=30
SIZE=320x240

while [ $# -gt 0 ]; do
	case "$1" in
		--bpp)  BPP=$2;  shift 2 ;;
		--fps)  FPS=$2;  shift 2 ;;
		--size) SIZE=$2; shift 2 ;;
		*) echo "未知参数: $1" >&2; exit 2 ;;
	esac
done

[ -f "$SRC" ] || { echo "!! 找不到 $SRC" >&2; exit 1; }

command -v ffmpeg  >/dev/null || { echo "!! 需要 ffmpeg" >&2; exit 1; }
command -v python3 >/dev/null || { echo "!! 需要 python3" >&2; exit 1; }
python3 -c 'import numpy' 2>/dev/null || { echo "!! 需要 numpy（pip install numpy）" >&2; exit 1; }

echo "== 源 =="
ffprobe -v error -select_streams v:0 \
	-show_entries stream=width,height,r_frame_rate,nb_frames -of default=nw=1 "$SRC" | sed 's/^/   /'

echo
echo "== 编码（bpp=$BPP fps=$FPS size=$SIZE）=="
python3 tools/xwpm-encode.py "$SRC" -o "media/$OUT" \
	--size "$SIZE" --fps "$FPS" --bpp "$BPP"

echo
echo "== gzip =="
gzip -9 -c "media/$OUT" > "media/$OUT.gz"
ls -l "media/$OUT" "media/$OUT.gz" | awk '{printf "   %10d  %s\n", $5, $9}'

echo
echo "== 打包给 OpenWrt 包用（内含 badapple.xwpm.gz）=="
# ⚠️ 必须有一个与 PKG_NAME-PKG_VERSION 同名的**顶层目录**：
#    OpenWrt 解包时用 --strip-components=1，若 tarball 根下直接是单个文件，
#    那个文件会被一起剥掉，最后什么都解不出来。
TMP=$(mktemp -d)
mkdir -p "$TMP/$(basename "$PKG" .tar.gz)"
cp "media/$OUT.gz" "$TMP/$(basename "$PKG" .tar.gz)/badapple.xwpm.gz"
# 固定 mtime，让同内容产出同 hash
tar --sort=name --owner=0 --group=0 --numeric-owner \
	--mtime='2026-01-01 00:00:00 UTC' \
	-czf "media/$PKG" -C "$TMP" "$(basename "$PKG" .tar.gz)"
rm -rf "$TMP"
ls -l "media/$PKG" | awk '{printf "   %10d  %s\n", $5, $9}'
echo "   内容："
tar tzf "media/$PKG" | sed 's/^/     /'

echo
echo "== 把下面两行填进 package/xwrt-media-badapple/Makefile =="
echo "PKG_HASH:=$(sha256sum "media/$PKG" | cut -d' ' -f1)"
echo "（并确认 PKG_SOURCE:=$PKG）"

echo
echo "== 本地自检 =="
if [ -x "$ROOT/tools/.xwpmplay-host" ]; then
	"$ROOT/tools/.xwpmplay-host" "media/$OUT" --info | sed 's/^/   /'
else
	python3 - "media/$OUT" <<'EOS'
import struct, os, sys
p = sys.argv[1]
m, v, hs, w, h, bpp, fl, fps, nf, pc, rsv = struct.unpack('<4sHHHHBBHIHH', open(p, 'rb').read(24))
fb = ((w * bpp + 7) // 8) * h
want = 24 + (512 if fl & 1 else 0) + fb * nf
got = os.path.getsize(p)
print('   %sx%s bpp=%d fps=%d frames=%d flags=0x%x' % (w, h, bpp, fps, nf, fl))
print('   大小 %d %s' % (got, 'OK' if got == want else '!! 期望 %d' % want))
EOS
fi

echo
echo "完成。"
