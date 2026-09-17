#!/usr/bin/env python3
"""xwpm-encode —— 把视频 / 图片 / 图片序列转成 .xwpm 媒体包（给 xwpmplay 播放）。

容器格式见 docs/FORMAT.md。

例：
  # 视频 -> 320x240 @30fps 4级灰（Bad Apple 就是这么来的）
  ./xwpm-encode.py bad_apple_video.mp4 -o badapple.xwpm --size 320x240 --fps 30 --bpp 2

  # 单张图片，显示 5 秒（30fps => 150 帧）
  ./xwpm-encode.py logo.png -o logo.xwpm --fps 30 --hold 150 --bpp 4

  # 一个目录里的图片当动画（按文件名排序，每张 1 帧）
  ./xwpm-encode.py frames/ -o anim.xwpm --fps 12 --bpp 8

  # 全彩（不量化，直接 RGB565）
  ./xwpm-encode.py clip.mp4 -o clip.xwpm --fps 24 --bpp 16
"""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

MAGIC = b'XWPM'
VERSION = 1
HDR_SIZE = 24
FLAG_PALETTE = 1
FLAG_LOOP = 2

BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32)

VIDEO_EXT = ('.mp4', '.mkv', '.webm', '.avi', '.mov', '.m4v', '.ts', '.gif', '.flv', '.wmv')
IMAGE_EXT = ('.png', '.jpg', '.jpeg', '.bmp', '.webp', '.tif', '.tiff', '.gif')


def rgb_to_565(a):
    """a: (...,3) uint8 -> (...,) uint16 little-endian value"""
    r = a[..., 0].astype(np.uint16)
    g = a[..., 1].astype(np.uint16)
    b = a[..., 2].astype(np.uint16)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def gray_palette(n):
    """n 级灰阶的 RGB565 调色板（0..255 等距）"""
    out = []
    for i in range(n):
        v = int(round(255.0 * i / (n - 1))) if n > 1 else 0
        out.append(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3))
    return out


def quantize(rgb, bpp, dither=True):
    """rgb: (H,W,3) uint8 -> (H,W) uint8 索引，配合 gray_palette(2^bpp)"""
    n = 1 << bpp
    gray = (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2])
    t = gray * ((n - 1) / 255.0)
    if dither and n > 1:
        h, w = gray.shape
        tile = np.tile(BAYER8 / 64.0 - 0.5, (h // 8 + 1, w // 8 + 1))[:h, :w]
        t = t + tile * (1.0 - 1.0 / n) * 1.1
    return np.rint(t).clip(0, n - 1).astype(np.uint8)


def pack_indices(idx, bpp):
    """(H,W) uint8 索引 -> 打包字节（高位在前）"""
    h, w = idx.shape
    if bpp == 8:
        return idx.tobytes()
    if bpp == 4:
        pad = (2 - w % 2) % 2
        if pad:
            idx = np.pad(idx, ((0, 0), (0, pad)))
        a = idx.reshape(h, -1, 2)
        return ((a[:, :, 0] << 4) | a[:, :, 1]).astype(np.uint8).tobytes()
    if bpp == 2:
        pad = (4 - w % 4) % 4
        if pad:
            idx = np.pad(idx, ((0, 0), (0, pad)))
        a = idx.reshape(h, -1, 4)
        return ((a[:, :, 0] << 6) | (a[:, :, 1] << 4) |
                (a[:, :, 2] << 2) | a[:, :, 3]).astype(np.uint8).tobytes()
    if bpp == 1:
        pad = (8 - w % 8) % 8
        if pad:
            idx = np.pad(idx, ((0, 0), (0, pad)))
        a = idx.reshape(h, -1, 8)
        out = np.zeros((h, a.shape[1]), dtype=np.uint8)
        for k in range(8):
            out |= (a[:, :, k].astype(np.uint8) << (7 - k))
        return out.tobytes()
    raise ValueError('bpp %d' % bpp)


def encode_frame(rgb, w, h, bpp, dither):
    """rgb: (H,W,3) uint8 -> (payload bytes, palette or None)"""
    if rgb.shape[0] != h or rgb.shape[1] != w:
        raise ValueError('frame %r != %dx%d' % (rgb.shape[:2], w, h))
    if bpp == 16:
        return rgb_to_565(rgb).astype('<u2').tobytes(), None
    idx = quantize(rgb, bpp, dither)
    return pack_indices(idx, bpp), gray_palette(1 << bpp)


def iter_video_frames(path, w, h, fps, fit, start, dur):
    """用 ffmpeg 解码，产出 (H,W,3) uint8"""
    if fit == 'stretch':
        vf = 'fps=%s,scale=%d:%d:flags=lanczos' % (fps, w, h)
    elif fit == 'crop':
        vf = ('fps=%s,scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,'
              'crop=%d:%d' % (fps, w, h, w, h))
    else:  # fit -> letterbox
        vf = ('fps=%s,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,'
              'pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black' % (fps, w, h, w, h))
    cmd = ['ffmpeg', '-v', 'error']
    if start:
        cmd += ['-ss', str(start)]
    cmd += ['-i', path]
    if dur:
        cmd += ['-t', str(dur)]
    cmd += ['-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    n = w * h * 3
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        yield np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)
    p.stdout.close()
    err = p.stderr.read().decode('utf-8', 'replace').strip()
    p.stderr.close()
    if p.wait() != 0 and err:
        sys.stderr.write('  ffmpeg: %s\n' % err[:400])


def load_image(path, w, h, fit):
    from PIL import Image
    im = Image.open(path).convert('RGB')
    if fit == 'stretch':
        im = im.resize((w, h), Image.LANCZOS)
    elif fit == 'crop':
        sw, sh = im.size
        s = max(w / sw, h / sh)
        im = im.resize((max(1, int(sw * s + .5)), max(1, int(sh * s + .5))), Image.LANCZOS)
        l = (im.size[0] - w) // 2
        t = (im.size[1] - h) // 2
        im = im.crop((l, t, l + w, t + h))
    else:
        sw, sh = im.size
        s = min(w / sw, h / sh)
        nw, nh = max(1, int(sw * s + .5)), max(1, int(sh * s + .5))
        im = im.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new('RGB', (w, h), (0, 0, 0))
        canvas.paste(im, ((w - nw) // 2, (h - nh) // 2))
        im = canvas
    return np.asarray(im, dtype=np.uint8)


def main():
    ap = argparse.ArgumentParser(description='视频/图片 -> .xwpm 媒体包')
    ap.add_argument('input', help='视频 / 图片 / 目录')
    ap.add_argument('-o', '--out', required=True, help='输出 .xwpm')
    ap.add_argument('--size', default='320x240', help='WxH，默认 320x240')
    ap.add_argument('--fps', type=float, default=30.0)
    ap.add_argument('--bpp', type=int, default=2, choices=[1, 2, 4, 8, 16])
    ap.add_argument('--fit', default='stretch', choices=['stretch', 'fit', 'crop'])
    ap.add_argument('--no-dither', action='store_true')
    ap.add_argument('--hold', type=int, default=1, help='静态图/序列每张重复多少帧')
    ap.add_argument('--start', type=float, default=0.0, help='视频起始秒')
    ap.add_argument('--dur', type=float, default=0.0, help='视频时长秒')
    ap.add_argument('--max-frames', type=int, default=0)
    ap.add_argument('--loop-hint', action='store_true', help='在头里标记建议循环')
    args = ap.parse_args()

    try:
        w, h = [int(v) for v in args.size.lower().split('x')]
    except Exception:
        sys.exit('!! --size 格式应为 WxH，例如 320x240')

    fps = max(1, int(round(args.fps)))
    src = args.input
    is_dir = os.path.isdir(src)
    ext = os.path.splitext(src)[1].lower()
    tmp = args.out + '.tmp'
    nframes = 0
    pal_out = None

    with open(tmp, 'wb') as f:
        # 头 + 调色板占位（bpp<=8 一定有调色板）。★ 必须预留 512 B，
        # 否则最后 seek(0) 回填时会把第 0 帧的前 512 字节覆盖掉。
        has_pal_file = args.bpp <= 8
        f.write(b'\0' * (HDR_SIZE + (512 if has_pal_file else 0)))
        if is_dir or (ext in IMAGE_EXT and ext != '.gif'):
            if is_dir:
                files = sorted(os.path.join(src, x) for x in os.listdir(src)
                               if os.path.splitext(x)[1].lower() in IMAGE_EXT)
                if not files:
                    sys.exit('!! 目录里没有图片: %s' % src)
            else:
                files = [src]
            print('  图片 %d 张，每张 %d 帧' % (len(files), args.hold))
            for p in files:
                rgb = load_image(p, w, h, args.fit)
                payload, pal = encode_frame(rgb, w, h, args.bpp, not args.no_dither)
                pal_out = pal
                for _ in range(args.hold):
                    f.write(payload)
                    nframes += 1
                    if args.max_frames and nframes >= args.max_frames:
                        break
                if args.max_frames and nframes >= args.max_frames:
                    break
        else:
            print('  视频解码中…')
            for rgb in iter_video_frames(src, w, h, fps, args.fit, args.start, args.dur):
                payload, pal = encode_frame(rgb, w, h, args.bpp, not args.no_dither)
                pal_out = pal
                f.write(payload)
                nframes += 1
                if nframes % 500 == 0:
                    print('    ... %d 帧' % nframes)
                if args.max_frames and nframes >= args.max_frames:
                    break

        if nframes == 0:
            os.unlink(tmp)
            sys.exit('!! 一帧都没编出来（输入不支持？）')

        flags = (FLAG_PALETTE if pal_out is not None else 0)
        if args.loop_hint:
            flags |= FLAG_LOOP
        if pal_out is not None:
            pal = np.array(pal_out, dtype='<u2').tobytes()
            pal = pal + b'\0' * (512 - len(pal))
        else:
            pal = b''
        assert len(pal) in (0, 512), len(pal)
        f.seek(0)
        f.write(struct.pack('<4sHHHHBBHIHH', MAGIC, VERSION, HDR_SIZE, w, h,
                            args.bpp, flags, fps, nframes,
                            256 if pal_out is not None else 0, 0))
        f.write(pal)

    os.replace(tmp, args.out)
    sz = os.path.getsize(args.out)
    fbytes = ((w * args.bpp + 7) // 8) * h
    want = HDR_SIZE + (512 if args.bpp <= 8 else 0) + fbytes * nframes
    if sz != want:
        sys.exit('!! 产物大小 %d != 期望 %d（容器结构不对）' % (sz, want))
    print('  ✓ %s  %dx%d bpp=%d fps=%d frames=%d (%.1fs)  %.1f MB'
          % (args.out, w, h, args.bpp, fps, nframes, nframes / fps, sz / 1048576))


if __name__ == '__main__':
    main()
