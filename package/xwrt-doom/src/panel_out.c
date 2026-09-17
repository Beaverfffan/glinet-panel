/*
 * panel_out.c —— BE14000 机身屏 DRM 输出实现。
 *
 * 判据（沿用 bad-apple 那套）：
 *   一次全帧 commit = /sys/class/spi_master/spi0/statistics/bytes 增加 153,611 B
 *   （153,600 = 320*240*2 像素 + 11 B 命令）。
 * 没有这个增量就说明帧没上屏，别去怀疑面板。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "panel_out.h"

#ifndef DRM_CLIENT_CAP_UNIVERSAL_PLANES
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#endif

#define FULL_FRAME_BYTES 153611UL	/* 320x240x2 + 11 */

struct pfb {
	uint32_t handle, fb, pitch;
	uint64_t size;
	uint16_t *map;
};

static int	fd_ = -1;
static int	w_, h_;
static uint32_t	crtc_, plane_, conn_;
static drmModeModeInfo mode_;
static int	have_mode_;
static struct pfb b_[2];
static int	back_, shown_ = -1;
static unsigned long frames_;

/* ── SPI 计数器：唯一能证明「帧真的到了玻璃上」的东西 ────────────────── */
unsigned long panel_out_spi_bytes(void)
{
	int fd;
	char buf[32];
	ssize_t n;
	unsigned long v;

	fd = open("/sys/class/spi_master/spi0/statistics/bytes", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	v = strtoul(buf, NULL, 10);
	return v;
}

float panel_out_spi_frames(void)
{
	return panel_out_spi_bytes() / (float)FULL_FRAME_BYTES;
}

/* 面板（LVGL/ucode）和我们在抢同一张 DRM 卡。没有 master ⇒ legacy ioctl 全 EACCES，
 * 进程照跑、日志不喊、屏上纹丝不动。所以先抢，抢不到就等。
 *
 * ⚠️ 这里等到超时**不退出**：`panel_out_submit()` 每一帧都还会重试 drmSetMaster，
 *    所以只要面板后来被清掉（init 的 panel_stop / screen-guard），游戏自己会接上。
 *    但如果是手工前台起 doom，看到这条 warn 就得去 `/etc/init.d/xwrt-panel stop`。
 *    实测：面板 ucode 占着 master 时，SPI 仍然每秒稳定 +153,611 B ——
 *    那是**面板自己的空转刷新**，极容易误判成"doom 在跑"。
 *    真 doom 在跑是 +153,611 × 36/s。 */
static int take_master(void)
{
	int i;

	if (drmSetMaster(fd_) == 0)
		return 0;
	for (i = 0; i < 100; i++) {
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
		if (drmSetMaster(fd_) == 0) {
			fprintf(stderr, "  [drm] 等了 %.1fs 拿到 DRM master（面板刚退出）\n",
				(i + 1) / 10.0);
			return 0;
		}
		if (i == 9)
			fprintf(stderr, "  [drm] 还在等 DRM master……面板服务占着，"
				"/etc/init.d/xwrt-panel stop 一下最快\n");
	}
	fprintf(stderr, "  [drm] warn: 等 10s 没拿到 DRM master —— 面板服务（xwrt-panel，"
		"带 procd respawn）还在跑。\n"
		"       处置：/etc/init.d/xwrt-panel stop  然后 kill -9 掉残留的 ucode；\n"
		"       只 kill 不 stop 的话 procd 5 秒后会把它拉回来（respawn retry=5）。\n");
	return -1;
}

static int fb_create(struct pfb *f)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	void *m;

	memset(&creq, 0, sizeof(creq));
	creq.width = w_;
	creq.height = h_;
	creq.bpp = 16;
	if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;

	f->handle = creq.handle;
	f->pitch = creq.pitch;
	f->size = creq.size;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = f->handle;
	if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;

	m = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, mreq.offset);
	if (m == MAP_FAILED)
		return -1;
	f->map = m;
	memset(f->map, 0, f->size);

	handles[0] = f->handle;
	pitches[0] = f->pitch;
	if (drmModeAddFB2(fd_, w_, h_, DRM_FORMAT_RGB565, handles, pitches,
			  offsets, &f->fb, 0) < 0) {
		if (drmModeAddFB(fd_, w_, h_, 16, 16, f->pitch, f->handle,
				 &f->fb) < 0)
			return -1;
	}
	return 0;
}

int panel_out_init(int w, int h)
{
	drmModeRes *res = NULL;
	drmModePlaneRes *pres = NULL;
	int i, k;

	w_ = w;
	h_ = h;

	fd_ = open("/dev/dri/card0", O_RDWR);
	if (fd_ < 0) {
		fprintf(stderr, "  [drm] !! open /dev/dri/card0: %s\n", strerror(errno));
		return -1;
	}

	/* 没有这个 cap，drmModeGetPlaneResources() 返回空列表 */
	if (drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
		fprintf(stderr, "  [drm] warn: UNIVERSAL_PLANES: %s\n", strerror(errno));
	(void)drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1);

	take_master();

	res = drmModeGetResources(fd_);
	if (!res) {
		fprintf(stderr, "  [drm] !! drmModeGetResources: %s\n", strerror(errno));
		return -1;
	}
	if (res->count_crtcs < 1) {
		fprintf(stderr, "  [drm] !! 没有 CRTC\n");
		drmModeFreeResources(res);
		return -1;
	}
	crtc_ = res->crtcs[0];

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd_, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn_ = c->connector_id;
			mode_ = c->modes[0];
			have_mode_ = 1;
			drmModeFreeConnector(c);
			break;
		}
		drmModeFreeConnector(c);
	}

	pres = drmModeGetPlaneResources(fd_);
	if (pres) {
		for (i = 0; i < (int)pres->count_planes; i++) {
			drmModePlane *p = drmModeGetPlane(fd_, pres->planes[i]);
			int ok;
			if (!p)
				continue;
			k = -1;
			for (k = 0; k < res->count_crtcs; k++)
				if (res->crtcs[k] == crtc_)
					break;
			ok = (p->crtc_id == crtc_ || p->crtc_id == 0) &&
			     (k < res->count_crtcs) &&
			     (p->possible_crtcs & (1 << k)) && p->count_formats > 0;
			if (ok) {
				fprintf(stderr, "  [drm] plane %u  formats:", p->plane_id);
				for (k = 0; k < (int)p->count_formats && k < 8; k++)
					fprintf(stderr, " %.4s", (char *)&p->formats[k]);
				fprintf(stderr, "\n");
				plane_ = p->plane_id;
				drmModeFreePlane(p);
				break;
			}
			drmModeFreePlane(p);
		}
		drmModeFreePlaneResources(pres);
	}
	if (!plane_)
		fprintf(stderr, "  [drm] warn: 没有可用 plane ⇒ 退回 drmModeSetCrtc\n");

	for (i = 0; i < 2; i++) {
		if (fb_create(&b_[i]) < 0) {
			fprintf(stderr, "  [drm] !! 建 dumb buffer %d 失败: %s\n",
				i, strerror(errno));
			return -1;
		}
	}
	fprintf(stderr, "  [drm] %dx%d  fb=%u/%u pitch=%u  mode=%dx%d@%d\n",
		w_, h_, b_[0].fb, b_[1].fb, b_[0].pitch,
		have_mode_ ? mode_.hdisplay : -1,
		have_mode_ ? mode_.vdisplay : -1,
		have_mode_ ? mode_.vrefresh : -1);

	drmModeFreeResources(res);

	/* 先把 0 号缓冲挂上，避免中途出现未初始化画面 */
	if (have_mode_) {
		if (drmModeSetCrtc(fd_, crtc_, b_[0].fb, 0, 0, &conn_, 1, &mode_) < 0)
			fprintf(stderr, "  [drm] warn: drmModeSetCrtc: %s（继续）\n",
				strerror(errno));
	}
	back_ = 1;	/* 0 号已经在显示，先填 1 号 */
	shown_ = 0;
	return 0;
}

int panel_out_width(void)  { return w_; }
int panel_out_height(void) { return h_; }

uint16_t *panel_out_backbuf(void)
{
	return b_[back_].map;
}

int panel_out_submit(void)
{
	static unsigned long fails_;
	int k, last_e = 0;

	for (k = 0; k < 6; k++) {
		int rc = -1, e = 0;

		if (plane_) {
			rc = drmModeSetPlane(fd_, plane_, crtc_, b_[back_].fb, 0,
					     0, 0, w_, h_, 0, 0, w_ << 16, h_ << 16);
			e = rc ? errno : 0;
			/* EINVAL = 这个 plane 不吃我们的 fb（格式/CRTC 状态不满足）。
			 * 别死磕：退回整 CRTC 翻转，它吃的是同一张 fb。 */
			if (e == EINVAL) {
				fprintf(stderr, "  [drm] plane %u 提交 EINVAL ⇒ "
					"退回 drmModeSetCrtc\n", plane_);
				plane_ = 0;
				rc = -1;
			}
		}
		if (!plane_) {
			if (!have_mode_) {
				e = EINVAL;	/* 既没 plane 又没 mode：没法提交 */
			} else {
				rc = drmModeSetCrtc(fd_, crtc_, b_[back_].fb, 0, 0,
						    &conn_, 1, &mode_);
				e = rc ? errno : 0;
			}
		}
		if (e == 0) {
			shown_ = back_;
			back_ ^= 1;	/* 交替 fb ⇒ 内核一定认为「有变化」 */
			frames_++;
			return 0;
		}
		last_e = e;
		/* EACCES/EPERM = master 被别人抢了；EAGAIN/EBUSY = 上一帧还没
		 * flip 完（面板驱动在 ioctl 里同步等 SPI 吐完整帧，启动头几帧
		 * 和超速提交时都会撞上）。这两类都值得等一下再试。 */
		if (e != EACCES && e != EPERM && e != EAGAIN && e != EBUSY)
			break;
		if (e == EACCES || e == EPERM) {
			/* 面板一死，这一句就会成功，游戏自己接上 */
			drmSetMaster(fd_);
			{
				struct timespec ts = { 0, 150 * 1000 * 1000 };
				nanosleep(&ts, NULL);
			}
		} else {
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	/* ⚠️ 这里必须打 last_e。打 errno 会印出 nanosleep 留下的残留值 ——
	 * 见过"commit 失败: Resource busy"其实是 EACCES 的误报。 */
	if (fails_++ < 5)
		fprintf(stderr, "  [drm] !! commit 失败 @%lu: %s (%d)%s\n", frames_,
			strerror(last_e), last_e,
			fails_ == 5 ? "（后续同类失败不再打印）" : "");
	return -1;
}

unsigned long panel_out_frames(void)
{
	return frames_;
}

void panel_out_close(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (b_[i].map && b_[i].map != MAP_FAILED)
			munmap(b_[i].map, b_[i].size);
		if (b_[i].fb)
			drmModeRmFB(fd_, b_[i].fb);
		if (b_[i].handle) {
			struct drm_mode_destroy_dumb dreq;
			memset(&dreq, 0, sizeof(dreq));
			dreq.handle = b_[i].handle;
			drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		}
	}
	if (fd_ >= 0)
		close(fd_);
	fd_ = -1;
}
