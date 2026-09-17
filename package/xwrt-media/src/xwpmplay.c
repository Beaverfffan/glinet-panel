/*
 * xwpmplay —— 在 DRM/SPI 直连小屏（如 GL.iNet GL-BE14000 的 320x240 panel-mipi-dbi/ST7789）
 * 上播放 .xwpm 媒体包。支持 bpp = 1/2/4/8/16，可从文件或 stdin（管道）读取。
 *
 * 为什么直接走 DRM 而不用 /dev/fb0：
 *   DRM 的 fbdev 模拟是「影子缓冲 + 异步脏页刷新」，连续写入会被内核合并，
 *   实测连写 20 帧只推出 0~3 帧 —— 完全控制不了帧率。
 *   这里用双 dumb buffer + 交替 drmModeSetPlane：一次 commit 必定推一整帧。
 *
 * 容器格式见 docs/FORMAT.md。用法：
 *   xwpmplay <file|-> [fps] [--loop] [--max N] [--swap] [--info] [--quiet]
 *   gunzip -c /usr/share/xwrt-media/badapple.xwpm.gz | xwpmplay - 30
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* ---------------- .xwpm 容器 ---------------- */
#define XWPM_MAGIC      "XWPM"
#define XWPM_VERSION    1
#define XWPM_HDR_SIZE   24
#define XWPM_PAL_ENTRIES 256
#define XWPM_FLAG_PALETTE 1
#define XWPM_FLAG_LOOP    2

struct xwpm_hdr {
	char     magic[4];
	uint16_t version;
	uint16_t header_size;
	uint16_t width;
	uint16_t height;
	uint8_t  bpp;
	uint8_t  flags;
	uint16_t fps;
	uint32_t frames;
	uint16_t palette_count;
	uint16_t reserved;
} __attribute__((packed));

/* ---------------- 输出尺寸上限 ---------------- */
#define MAX_W 1920
#define MAX_H 1080

static uint16_t pal[256];
static int pal_n;
static int swap_byte;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned long spi_bytes(void)
{
	int fd = open("/sys/class/spi_master/spi0/statistics/bytes", O_RDONLY);
	char b[32];
	ssize_t n;
	unsigned long v = 0;

	if (fd < 0)
		return 0;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	b[n] = 0;
	v = strtoul(b, NULL, 10);
	return v;
}

/* ---------------- 全量读取（文件可 seek；stdin 顺序读） ---------------- */
static uint8_t *src_buf;
static size_t   src_pos;
static int      src_fd = -1;
static int      src_is_file;

static int src_read(void *dst, size_t n)
{
	size_t got = 0;

	if (src_is_file) {
		if (pread(src_fd, dst, n, (off_t)src_pos) != (ssize_t)n)
			return -1;
		src_pos += n;
		return 0;
	}
	while (got < n) {
		ssize_t r = read(src_fd, (char *)dst + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	src_pos += n;
	return 0;
}

/* ---------------- DRM ---------------- */
struct fb {
	uint32_t handle, fb, pitch;
	uint64_t size;
	uint16_t *map;
};

static int fb_create(int fd, struct fb *f, int w, int h)
{
	struct drm_mode_create_dumb creq = { 0 };
	struct drm_mode_map_dumb mreq = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	void *m;

	creq.width = w;
	creq.height = h;
	creq.bpp = 16;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;
	f->handle = creq.handle;
	f->pitch = creq.pitch;
	f->size = creq.size;

	mreq.handle = f->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;
	m = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (m == MAP_FAILED)
		return -1;
	f->map = m;
	memset(f->map, 0, f->size);

	handles[0] = f->handle;
	pitches[0] = f->pitch;
	if (drmModeAddFB2(fd, w, h, DRM_FORMAT_RGB565, handles, pitches, offsets,
			  &f->fb, 0) < 0) {
		if (drmModeAddFB(fd, w, h, 16, 16, f->pitch, f->handle, &f->fb) < 0)
			return -1;
	}
	return 0;
}

/* ---------------- 一帧解码到 RGB565 ---------------- */
static inline uint16_t put(uint16_t c)
{
	return swap_byte ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

static void decode_frame(const uint8_t *s, uint16_t *d, int w, int h, int bpp)
{
	int x, y, p;

	switch (bpp) {
	case 16:
		/* 源已是 RGB565 LE，逐像素按需换序 */
		if (!swap_byte) {
			memcpy(d, s, (size_t)w * h * 2);
		} else {
			const uint16_t *q = (const uint16_t *)s;
			for (p = 0; p < w * h; p++)
				d[p] = put(q[p]);
		}
		break;
	case 8:
		for (p = 0; p < w * h; p++)
			d[p] = put(pal[s[p]]);
		break;
	case 4:
		for (y = 0; y < h; y++) {
			const uint8_t *r = s + (size_t)y * ((w + 1) / 2);
			uint16_t *o = d + (size_t)y * w;
			for (x = 0; x < w; x += 2) {
				uint8_t b = r[x >> 1];
				o[x] = put(pal[b >> 4]);
				if (x + 1 < w)
					o[x + 1] = put(pal[b & 0xF]);
			}
		}
		break;
	case 2:
		for (y = 0; y < h; y++) {
			const uint8_t *r = s + (size_t)y * ((w + 3) / 4);
			uint16_t *o = d + (size_t)y * w;
			for (x = 0; x < w; x += 4) {
				uint8_t b = r[x >> 2];
				o[x] = put(pal[(b >> 6) & 3]);
				if (x + 1 < w) o[x + 1] = put(pal[(b >> 4) & 3]);
				if (x + 2 < w) o[x + 2] = put(pal[(b >> 2) & 3]);
				if (x + 3 < w) o[x + 3] = put(pal[b & 3]);
			}
		}
		break;
	case 1:
		for (y = 0; y < h; y++) {
			const uint8_t *r = s + (size_t)y * ((w + 7) / 8);
			uint16_t *o = d + (size_t)y * w;
			for (x = 0; x < w; x++) {
				uint8_t b = r[x >> 3];
				o[x] = put(pal[(b >> (7 - (x & 7))) & 1]);
			}
		}
		break;
	default:
		break;
	}
}


/* ---------------- 拷行到目标缓冲 ----------------
 * dst 是字节指针，dst_pitch 是目标行距（DRM dumb buffer 可能 > w*2）；
 * frame 是解码好的 RGB565 临时帧，src_w 是它每行的像素数。
 * ⚠️ frame 是 uint16_t*：行偏移按「元素」算，**不要乘 2**。
 */
static void blit(uint8_t *dst, size_t dst_pitch, const uint16_t *frame,
		 int src_w, int w, int rows)
{
	int y;

	for (y = 0; y < rows; y++)
		memcpy(dst + (size_t)y * dst_pitch, frame + (size_t)y * src_w,
		       (size_t)w * 2);
}

/* ---------------- 导出 PPM（离线自检用，不依赖 DRM） ---------------- */
static int write_ppm(const char *out, const uint16_t *buf, int w, int h)
{
	FILE *f = fopen(out, "wb");
	int x, y;

	if (!f) {
		perror(out);
		return -1;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint16_t c = buf[(size_t)y * w + x];
			unsigned char px[3];

			px[0] = (unsigned char)(((c >> 11) & 0x1f) << 3);
			px[1] = (unsigned char)(((c >> 5) & 0x3f) << 2);
			px[2] = (unsigned char)((c & 0x1f) << 3);
			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

/* 把第 n 帧按「和播放完全相同的拷行逻辑」导成 PPM，用来在没有屏的机器上核对转码 */
static int do_dump(const struct xwpm_hdr *h, uint64_t fbytes, int n, const char *out)
{
	int w = h->width, hh = h->height;
	uint16_t *frame = malloc((size_t)w * hh * sizeof(uint16_t));
	uint8_t  *dst   = malloc((size_t)w * hh * 2);
	uint8_t  *tmp   = malloc((size_t)fbytes);
	int i;

	if (!frame || !dst || !tmp) {
		fprintf(stderr, "!! malloc 失败\n");
		return 1;
	}
	for (i = 0; i <= n; i++) {
		if (src_read(tmp, (size_t)fbytes) < 0) {
			fprintf(stderr, "!! 只读到第 %d 帧（请求第 %d 帧）\n", i - 1, n);
			return 1;
		}
	}
	memset(dst, 0, (size_t)w * hh * 2);
	decode_frame(tmp, frame, w, hh, h->bpp);
	blit(dst, (size_t)w * 2, frame, w, w, hh);
	if (write_ppm(out, (const uint16_t *)dst, w, hh) < 0)
		return 1;
	fprintf(stderr, "  已导出第 %d 帧 %dx%d -> %s\n", n, w, hh, out);
	return 0;
}

/* ---------------- DRM master ----------------
 * 面板（LVGL）也在用同一张 DRM 卡；只要它还是 master，本进程的 legacy
 * ioctl（SetPlane/SetCrtc）就全部 EACCES。这里等它让出来。
 */
static void take_master(int fd)
{
	int i;

	if (drmSetMaster(fd) == 0)
		return;
	for (i = 0; i < 40; i++) {
		usleep(100 * 1000);
		if (drmSetMaster(fd) == 0) {
			fprintf(stderr, "  等了 %.1fs 才拿到 DRM master（面板刚退出）\n",
				(i + 1) / 10.0);
			return;
		}
	}
	fprintf(stderr, "  warn: 拿不到 DRM master —— 面板服务可能还在跑，"
			"先执行 /etc/init.d/xwrt-panel stop\n");
}

/* 提交一帧；EACCES 多半是 master 被别人拿回去了，抢一次再试 */
static int commit_plane(int fd, uint32_t pid_, uint32_t crtc, uint32_t fb, int w, int h)
{
	int k;

	for (k = 0; k < 6; k++) {
		if (drmModeSetPlane(fd, pid_, crtc, fb, 0,
				    0, 0, w, h, 0, 0, w << 16, h << 16) == 0)
			return 0;
		if (errno != EACCES && errno != EPERM)
			break;
		drmSetMaster(fd);
		usleep(150 * 1000);
	}
	return -1;
}

/* ---------------- 打印容器信息（key=value，供脚本/CGI 消费） ---------------- */
static void print_info(const char *path, const struct xwpm_hdr *h, uint64_t fbytes,
		       int has_pal)
{
	double dur = h->fps ? (double)h->frames / h->fps : 0;

	printf("file=%s\n", path);
	printf("format=xwpm\n");
	printf("version=%u\n", h->version);
	printf("width=%u\n", h->width);
	printf("height=%u\n", h->height);
	printf("bpp=%u\n", h->bpp);
	printf("fps=%u\n", h->fps);
	printf("frames=%u\n", h->frames);
	printf("duration=%.2f\n", dur);
	printf("palette=%s\n", has_pal ? "custom" : (h->bpp <= 8 ? "gray-ramp" : "none"));
	printf("palette_count=%u\n", has_pal ? h->palette_count : (h->bpp <= 8 ? (1u << h->bpp) : 0u));
	printf("frame_bytes=%llu\n", (unsigned long long)fbytes);
	printf("stream_bytes=%llu\n", (unsigned long long)(XWPM_HDR_SIZE + (has_pal ? 512 : 0) + fbytes * h->frames));
	if (src_is_file) {
		struct stat st;
		if (fstat(src_fd, &st) == 0)
			printf("file_bytes=%lld\n", (long long)st.st_size);
	}
}

int main(int argc, char **argv)
{
	const char *path;
	const char *dump_path = NULL;
	int fps_override = 0, loop = 0, quiet = 0, info_only = 0;
	int maxf = -1, arg, i, dump_n = -1;
	struct xwpm_hdr h;
	uint64_t fbytes;
	int has_pal = 0;
	drmModeRes *res = NULL;
	drmModePlaneRes *pres = NULL;
	uint32_t crtc_id = 0, plane_id = 0, conn_id = 0;
	drmModeModeInfo mode;
	int have_mode = 0;
	int drm_fd, f = 0;
	struct fb b[2];
	uint16_t *frame = NULL;
	double t0, t1, next;
	unsigned long s0, s1 = 0;
	int frame_w, frame_h;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <file|-> [fps] [--loop] [--max N] [--swap] [--info] [--quiet] [--dump N out.ppm]\n"
			"       gunzip -c media.xwpm.gz | %s - 30\n",
			argv[0], argv[0]);
		return 2;
	}
	path = argv[1];
	for (arg = 2; arg < argc; arg++) {
		if (!strcmp(argv[arg], "--loop")) loop = 1;
		else if (!strcmp(argv[arg], "--swap")) swap_byte = 1;
		else if (!strcmp(argv[arg], "--quiet")) quiet = 1;
		else if (!strcmp(argv[arg], "--info")) info_only = 1;
		else if (!strcmp(argv[arg], "--max") && arg + 1 < argc) maxf = atoi(argv[++arg]);
		else if (!strcmp(argv[arg], "--dump") && arg + 2 < argc) {
			dump_n = atoi(argv[++arg]);
			dump_path = argv[++arg];
		}
		else { fps_override = atoi(argv[arg]); }
	}
	memset(&mode, 0, sizeof(mode));

	/* ---- 打开源 ---- */
	if (!strcmp(path, "-")) {
		src_fd = 0;
		src_is_file = 0;
	} else {
		src_fd = open(path, O_RDONLY);
		if (src_fd < 0) { perror(path); return 1; }
		src_is_file = 1;
	}

	/* ---- 读头 ---- */
	if (src_read(&h, sizeof(h)) < 0) {
		fprintf(stderr, "!! 读不到容器头（文件太短或管道断了）\n");
		return 1;
	}
	if (memcmp(h.magic, XWPM_MAGIC, 4) != 0) {
		fprintf(stderr, "!! 不是 .xwpm 文件（magic=%02x%02x%02x%02x，应为 XWPM）\n",
			h.magic[0], h.magic[1], h.magic[2], h.magic[3]);
		return 1;
	}
	if (h.version != XWPM_VERSION) {
		fprintf(stderr, "!! 容器版本 %u 不支持（本程序只认 %u）\n", h.version, XWPM_VERSION);
		return 1;
	}
	if (h.width == 0 || h.height == 0 || h.width > MAX_W || h.height > MAX_H) {
		fprintf(stderr, "!! 尺寸不合法: %ux%u\n", h.width, h.height);
		return 1;
	}
	if (h.bpp != 1 && h.bpp != 2 && h.bpp != 4 && h.bpp != 8 && h.bpp != 16) {
		fprintf(stderr, "!! bpp=%u 不支持（只认 1/2/4/8/16）\n", h.bpp);
		return 1;
	}
	if (h.frames == 0) {
		fprintf(stderr, "!! frames=0\n");
		return 1;
	}
	if (!h.fps)
		h.fps = 30;

	fbytes = (uint64_t)(((h.width * h.bpp + 7) / 8)) * h.height;

	/* ---- 调色板 ---- */
	if (h.flags & XWPM_FLAG_PALETTE) {
		has_pal = 1;
		if (src_read(pal, sizeof(pal)) < 0) {
			fprintf(stderr, "!! 调色板读不全\n");
			return 1;
		}
		pal_n = h.palette_count ? h.palette_count : XWPM_PAL_ENTRIES;
		if (pal_n > 256)
			pal_n = 256;
	} else if (h.bpp <= 8) {
		/* 缺省：2^bpp 级灰阶（RGB565） */
		int n = 1 << h.bpp;
		for (i = 0; i < n; i++) {
			uint8_t v = (uint8_t)(n > 1 ? (255.0 * i / (n - 1) + 0.5) : 0);
			pal[i] = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
		}
		pal_n = n;
	}

	if (info_only) {
		print_info(path, &h, fbytes, has_pal);
		if (h.flags & XWPM_FLAG_LOOP)
			printf("loop_hint=1\n");
		return 0;
	}

	if (dump_n >= 0)
		return do_dump(&h, fbytes, dump_n, dump_path);

	if (fps_override > 0)
		h.fps = (uint16_t)fps_override;
	if (h.flags & XWPM_FLAG_LOOP)
		loop = 1;

	if (!quiet)
		fprintf(stderr,
			"  %s  %ux%u bpp=%u fps=%u frames=%u (%.1fs)  pal=%s\n",
			path, h.width, h.height, h.bpp, h.fps, h.frames,
			(double)h.frames / h.fps, has_pal ? "custom" : "gray");

	/* ---- DRM ---- */
	drm_fd = open("/dev/dri/card0", O_RDWR);
	if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
		fprintf(stderr, "  warn: UNIVERSAL_PLANES 设置失败: %s\n", strerror(errno));
	(void)drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

	take_master(drm_fd);

	res = drmModeGetResources(drm_fd);
	if (!res) { perror("drmModeGetResources"); return 1; }
	if (res->count_crtcs < 1) { fprintf(stderr, "!! 没有 CRTC\n"); return 1; }
	crtc_id = res->crtcs[0];

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn_id = c->connector_id;
			mode = c->modes[0];
			have_mode = 1;
			break;
		}
		drmModeFreeConnector(c);
	}

	pres = drmModeGetPlaneResources(drm_fd);
	if (pres) {
		for (i = 0; i < (int)pres->count_planes; i++) {
			drmModePlane *p = drmModeGetPlane(drm_fd, pres->planes[i]);
			int ok, k = -1;
			if (!p)
				continue;
			for (k = 0; k < res->count_crtcs; k++)
				if (res->crtcs[k] == crtc_id)
					break;
			ok = (p->crtc_id == crtc_id || p->crtc_id == 0) &&
			     (k < res->count_crtcs) &&
			     (p->possible_crtcs & (1 << k)) &&
			     p->count_formats > 0;
			if (!quiet)
				fprintf(stderr, "  plane %u: crtc=%u poss=0x%x fmt=%u%s\n",
					p->plane_id, p->crtc_id, p->possible_crtcs,
					p->count_formats, ok ? "  <== 选用" : "");
			if (ok) {
				plane_id = p->plane_id;
				drmModeFreePlane(p);
				break;
			}
			drmModeFreePlane(p);
		}
	}
	if (!plane_id)
		fprintf(stderr, "  warn: 无可用 plane ⇒ 退回 drmModeSetCrtc\n");

	/* 输出尺寸：优先用容器尺寸（面板通常正好匹配；不等时按容器尺寸建 fb 由 plane 缩放不可靠，
	 * 所以这里以容器尺寸为准，靠 panel 的 mode 决定实际显示） */
	frame_w = h.width;
	frame_h = h.height;
	if (have_mode && (mode.hdisplay < frame_w || mode.vdisplay < frame_h)) {
		fprintf(stderr, "  warn: 面板 %ux%u 小于媒体 %ux%u，右下会被裁掉\n",
			mode.hdisplay, mode.vdisplay, frame_w, frame_h);
	}
	/* 若容器尺寸与面板不一致，按面板尺寸建 fb 并在解码时只填可见区域 */
	if (have_mode) {
		frame_w = mode.hdisplay;
		frame_h = mode.vdisplay;
	}

	for (i = 0; i < 2; i++) {
		if (fb_create(drm_fd, &b[i], frame_w, frame_h) < 0) {
			perror("fb_create");
			return 1;
		}
	}
	if (!quiet)
		fprintf(stderr, "  fb %u/%u  %ux%u  pitch=%u\n",
			b[0].fb, b[1].fb, frame_w, frame_h, b[0].pitch);

	/* 缓冲一帧（解码到临时 RGB565，必要时裁剪/居中） */
	frame = malloc((size_t)h.width * h.height * sizeof(uint16_t));
	if (!frame) { perror("malloc"); return 1; }

	/* 源数据：整块读入（文件可 mmap 加速；管道顺序读） */
	if (src_is_file) {
		struct stat st;
		if (fstat(src_fd, &st) == 0 && (uint64_t)st.st_size >= fbytes) {
			src_buf = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0);
			if (src_buf == MAP_FAILED)
				src_buf = NULL;
		}
	}

	if (have_mode) {
		if (drmModeSetCrtc(drm_fd, crtc_id, b[0].fb, 0, 0, &conn_id, 1, &mode) < 0)
			fprintf(stderr, "  warn: drmModeSetCrtc: %s（继续）\n", strerror(errno));
	}

	s0 = spi_bytes();
	t0 = now_s();
	next = t0;

	while (1) {
		static uint8_t tmp[4096 * 1024];
		const uint8_t *src;
		uint16_t *dst = b[f & 1].map;

		if (maxf > 0 && f >= maxf)
			break;
		if ((uint32_t)f >= h.frames) {
			if (!loop)
				break;
			f = 0;
			if (src_is_file)
				src_pos = XWPM_HDR_SIZE + (has_pal ? 512 : 0);
			else
				break;          /* 管道不能回头 */
		}
		if (fbytes > sizeof(tmp)) {
			fprintf(stderr, "!! 单帧 %llu B 超过内部缓冲\n", (unsigned long long)fbytes);
			break;
		}

		if (src_buf) {
			src = src_buf + XWPM_HDR_SIZE + (has_pal ? 512 : 0)
			      + (size_t)f * fbytes;
		} else {
			if (src_read(tmp, (size_t)fbytes) < 0)
				break;
			src = tmp;
		}

		decode_frame(src, frame, h.width, h.height, h.bpp);

		/* 拷到 fb（尺寸一致时直接全幅；否则左上对齐 + 其余保持上一次内容） */
		{
			int w = h.width < frame_w ? h.width : frame_w;
			int rows = h.height < frame_h ? h.height : frame_h;
			blit((uint8_t *)dst, b[f & 1].pitch, frame, h.width, w, rows);
		}

		if (plane_id) {
			if (commit_plane(drm_fd, plane_id, crtc_id, b[f & 1].fb,
						 frame_w, frame_h) < 0) {
				fprintf(stderr, "!! drmModeSetPlane @%d: %s\n", f, strerror(errno));
				break;
			}
		} else {
			if (drmModeSetCrtc(drm_fd, crtc_id, b[f & 1].fb, 0, 0,
					   &conn_id, 1, &mode) < 0) {
				fprintf(stderr, "!! drmModeSetCrtc @%d: %s\n", f, strerror(errno));
				break;
			}
		}
		f++;

		if (!quiet && (f % 60) == 0) {
			double el = now_s() - t0;
			unsigned long sb = spi_bytes() - s0;
			fprintf(stderr, "  %5d/%u 帧  %.2fs  %.1f fps  SPI %.2f MiB/s  已上屏 %.1f 帧\n",
				f, h.frames, el, f / el, sb / 1048576.0 / el, sb / 153611.0);
		}

		next += 1.0 / h.fps;
		{
			double d = next - now_s();
			if (d > 0) {
				struct timespec ts;
				ts.tv_sec = (time_t)d;
				ts.tv_nsec = (long)((d - ts.tv_sec) * 1e9);
				nanosleep(&ts, NULL);
			} else if (d < -0.5) {
				next = now_s();
			}
		}
	}

	t1 = now_s();
	s1 = spi_bytes() - s0;
	if (!quiet)
		fprintf(stderr,
			"==== %d 帧 / %.2fs = %.2f fps；SPI %.2f MiB/s ⇒ 实际推上屏 %.1f 帧 (%.1f fps)\n",
			f, t1 - t0, f / (t1 - t0), s1 / 1048576.0 / (t1 - t0),
			s1 / 153611.0, s1 / 153611.0 / (t1 - t0));
	return 0;
}
