/*
 * tinput.c —— evdev 单点触摸 → 虚拟摇杆 + 锁定式按钮。
 *
 * ★ 为什么要「锁定式」按钮：这块屏只有单点触摸。手指按着 FIRE 就没法再滑动转向，
 *   反之亦然 —— Doom 里最要紧的「边退边打」直接做不出来。
 *   解法：FIRE / RUN 点一下锁住（再点解开），把那只手的自由度还给移动。
 *   另外保留「轻点画面 = 单发」，快速点射手感不丢。
 *
 * ★ 为什么动作要「脉冲」而不是瞬时按下+释放：Doom 的开火判定在 G_BuildTiccmd 里读
 *   gamekeydown[]（g_game.c）。按下与释放若落在同一个 35 Hz tick 内，等于没按过。
 *   所以 FIRE/USE 这类动作至少按住 pulse_ms（默认 120 ms ≈ 4 个 tick）。
 *
 * ★ 动作状态用三个来源「或」起来（锁定 / 脉冲 / 摇杆），而不是一个布尔量。
 *   否则「锁定 FIRE 时轻点一下，脉冲到期把锁定也一起松掉」这种错会潜伏到真机上。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#include "tinput.h"
#include "actions.h"

#define QLEN 64

static int	fd_ = -1;
static int	scr_w_ = 320, scr_h_ = 240;
static struct tin_cfg cfg_;
static int	ax_min_, ax_max_ = 319, ay_min_, ay_max_ = 239;

/* ── 事件队列 ─────────────────────────────────────────────────── */
static struct { int pressed, action; } q_[QLEN];
static int	q_r_, q_w_;

static void push(int pressed, int action)
{
	int n = (q_w_ + 1) % QLEN;

	if (action <= ACT_NONE || action >= ACT_MAX)
		return;
	if (n == q_r_) {		/* 满了就丢最老的，宁可丢事件也不能卡 */
		q_r_ = (q_r_ + 1) % QLEN;
	}
	q_[q_w_].pressed = pressed;
	q_[q_w_].action = action;
	q_w_ = n;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── 动作状态：三个来源 ───────────────────────────────────────── */
static unsigned src_latch;		/* 锁定按钮 */
static unsigned src_pulse;		/* 脉冲 */
static unsigned src_stick;		/* 摇杆方向 */
static long	pulse_until_[ACT_MAX];
static int	emitted_[ACT_MAX];	/* 上次实际发出的状态 */

static void sync_actions(void)
{
	int a;

	for (a = 1; a < ACT_MAX; a++) {
		unsigned bit = 1u << a;
		int want = (src_latch & bit) || (src_pulse & bit) ||
			   (src_stick & bit);

		if (want != emitted_[a]) {
			emitted_[a] = want;
			push(want, a);
		}
	}
}

static void pulse_act(int a)
{
	if (a <= ACT_NONE || a >= ACT_MAX)
		return;
	src_pulse |= 1u << a;
	pulse_until_[a] = now_ms() + cfg_.pulse_ms;
	sync_actions();
}

/* ── 触摸状态 ─────────────────────────────────────────────────── */
static int	t_down_, t_x_, t_y_, t_ox_, t_oy_;
static int	t_raw_x_, t_raw_y_;
static int	t_zone_;	/* 0 无 / 1 左区移动 / 2 右区转向 / 3 控制条 */
static int	t_btn_ = -1;
static int	t_act_;		/* 按下时锁定的动作（页面可能中途翻掉） */
static int	t_moved_;
static int	t_moving_;
static long	t_start_, t_last_ms_;

static int	page_;
static int	latched_[2][8];

static int	raw_x_new_, raw_y_new_;

/* 所有锁定按钮（两页都算）重新合成 src_latch —— 翻页不该丢掉已锁的 FIRE */
static void latch_resync(void)
{
	int p, i;

	src_latch = 0;
	for (p = 0; p < 2; p++) {
		int n = ovl_btn_count(p);
		for (i = 0; i < n; i++) {
			if (!ovl_btn(p, i)->latch || !latched_[p][i])
				continue;
			src_latch |= 1u << ovl_btn_action(p, i);
		}
	}
	sync_actions();
}

/* 摇杆方向 → 动作 */
static void stick_update(void)
{
	int dx, dy, dz = cfg_.deadzone;
	int radius = cfg_.radius;

	if (t_zone_ != 1 && t_zone_ != 2)
		return;

	dx = t_x_ - t_ox_;
	dy = t_y_ - t_oy_;

	/* 原点跟随：手指跑远了把原点拖过去，贴着屏边也能推出满速 */
	if (radius > 0) {
		double d = (double)dx * dx + (double)dy * dy;
		if (d > (double)radius * radius) {
			double k = radius / sqrt(d);
			t_ox_ = t_x_ - (int)(dx * k);
			t_oy_ = t_y_ - (int)(dy * k);
			dx = t_x_ - t_ox_;
			dy = t_y_ - t_oy_;
		}
	}

	src_stick = 0;
	if (t_zone_ == 1) {
		if (dy < -dz) src_stick |= 1u << ACT_FORWARD;
		if (dy > dz)  src_stick |= 1u << ACT_BACK;
		if (dx < -dz) src_stick |= 1u << ACT_STRAFE_L;
		if (dx > dz)  src_stick |= 1u << ACT_STRAFE_R;
		t_moving_ = (dx < -dz || dx > dz || dy < -dz || dy > dz);
	} else {
		if (dx < -dz) src_stick |= 1u << ACT_TURN_L;
		if (dx > dz)  src_stick |= 1u << ACT_TURN_R;
		t_moving_ = (dx < -dz || dx > dz);
	}
	sync_actions();
}

static void stick_release(void)
{
	src_stick = 0;
	t_moving_ = 0;
	sync_actions();
}

/* ── 触点归一化 ───────────────────────────────────────────────── */
static void normalize(int rx, int ry, int *sx, int *sy)
{
	int x = rx, y = ry;

	if (ax_max_ > ax_min_)
		x = (rx - ax_min_) * (scr_w_ - 1) / (ax_max_ - ax_min_);
	if (ay_max_ > ay_min_)
		y = (ry - ay_min_) * (scr_h_ - 1) / (ay_max_ - ay_min_);

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > scr_w_ - 1) x = scr_w_ - 1;
	if (y > scr_h_ - 1) y = scr_h_ - 1;

	if (cfg_.swap_xy) { int t = x; x = y; y = t; }
	if (cfg_.invert_x) x = scr_w_ - 1 - x;
	if (cfg_.invert_y) y = scr_h_ - 1 - y;

	*sx = x;
	*sy = y;
}

/* 按钮按下/滑入的公共处理 */
static void btn_enter(int b)
{
	const struct ovl_btn *btn = ovl_btn(page_, b);
	int act;

	if (!btn)
		return;
	act = ovl_btn_action(page_, b);
	t_act_ = act;

	if (act == ACT_PAGE) {
		/* 翻页在本层就地完成，不发给 Doom */
		page_ ^= 1;
		t_btn_ = -1;
		t_act_ = ACT_NONE;
		return;
	}
	if (btn->latch) {
		latched_[page_][b] = !latched_[page_][b];
		pulse_until_[act] = 0;
		src_pulse &= ~(1u << act);
		latch_resync();
	} else {
		pulse_act(act);
	}
}

static void btn_leave(int b)
{
	const struct ovl_btn *btn = ovl_btn(page_, b);
	int act;

	if (!btn || b < 0)
		return;
	act = ovl_btn_action(page_, b);
	if (btn->latch || act == ACT_PAGE)
		return;
	src_pulse &= ~(1u << act);
	pulse_until_[act] = 0;
	sync_actions();
}

static void on_down(int x, int y)
{
	t_down_ = 1;
	t_x_ = t_ox_ = x;
	t_y_ = t_oy_ = y;
	t_moved_ = 0;
	t_moving_ = 0;
	t_start_ = now_ms();
	t_last_ms_ = t_start_;
	t_btn_ = -1;
	t_act_ = ACT_NONE;

	if (ovl_in_bar(scr_h_, y)) {
		int b;

		t_zone_ = 3;
		b = ovl_hit(scr_h_, page_, x, y);
		t_btn_ = b;
		if (b >= 0) {
			btn_enter(b);
			if (t_btn_ < 0)		/* 翻页了 */
				return;
		}
		return;
	}

	t_zone_ = (x < OVL_ZONE_SPLIT) ? 1 : 2;
	stick_update();
}

static void on_move(int x, int y)
{
	t_x_ = x;
	t_y_ = y;
	t_last_ms_ = now_ms();
	if (abs(x - t_ox_) > cfg_.tap_slop || abs(y - t_oy_) > cfg_.tap_slop)
		t_moved_ = 1;

	if (t_zone_ == 3) {
		int b;

		if (!ovl_in_bar(scr_h_, y)) {
			/* 滑出控制条：松开手里的按钮 */
			btn_leave(t_btn_);
			t_btn_ = -1;
			t_act_ = ACT_NONE;
			return;
		}
		b = ovl_hit(scr_h_, page_, x, y);
		if (b != t_btn_) {
			btn_leave(t_btn_);
			t_btn_ = b;
			if (b >= 0)
				btn_enter(b);
		}
		return;
	}

	stick_update();
}

static void on_up(void)
{
	long dur = now_ms() - t_start_;

	if (t_zone_ != 3) {
		/* 画面区轻点 = 单发 */
		if (!t_moved_ && dur <= cfg_.tap_ms)
			pulse_act(ACT_FIRE);
	} else if (t_btn_ >= 0) {
		btn_leave(t_btn_);
	}

	stick_release();
	t_down_ = 0;
	t_zone_ = 0;
	t_btn_ = -1;
	t_act_ = ACT_NONE;
	t_moved_ = 0;
}

/* ── evdev ───────────────────────────────────────────────────── */
static int try_open_touch(const char *path)
{
	int fd, has_touch;
	unsigned long absbits = 0;
	unsigned char keybits[(KEY_MAX / 8) + 1];
	char name[64] = { 0 };

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), &absbits) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
		close(fd);
		return -1;
	}
	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

	/* 要 ABS_X + ABS_Y。（BTN_TOUCH 只用于打印，不作为判据） */
	if (!(absbits & (1UL << ABS_X)) || !(absbits & (1UL << ABS_Y))) {
		close(fd);
		return -1;
	}
	has_touch = (keybits[BTN_TOUCH / 8] >> (BTN_TOUCH % 8)) & 1;
	fprintf(stderr, "  [tin] %s = \"%s\"  abs=0x%lx%s\n", path, name, absbits,
		has_touch ? "  (BTN_TOUCH)" : "");
	return fd;
}

static int find_touch(const char *want)
{
	DIR *d;
	struct dirent *e;
	char p[300];

	if (want && *want && strcmp(want, "auto") != 0) {
		int fd = try_open_touch(want);
		if (fd < 0)
			fprintf(stderr, "  [tin] !! 打不开 %s: %s\n", want,
				strerror(errno));
		return fd;
	}

	d = opendir("/dev/input");
	if (!d) {
		fprintf(stderr, "  [tin] !! 打不开 /dev/input: %s\n", strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		int fd;

		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		fd = try_open_touch(p);
		if (fd >= 0) {
			closedir(d);
			return fd;
		}
	}
	closedir(d);
	fprintf(stderr, "  [tin] !! 没找到触摸设备（要 ABS_X+ABS_Y，如 CST353X）\n");
	return -1;
}

static void read_absinfo(void)
{
	struct input_absinfo ai;

	if (ioctl(fd_, EVIOCGABS(ABS_X), &ai) == 0) {
		ax_min_ = ai.minimum;
		ax_max_ = ai.maximum;
	}
	if (ioctl(fd_, EVIOCGABS(ABS_Y), &ai) == 0) {
		ay_min_ = ai.minimum;
		ay_max_ = ai.maximum;
	}
	fprintf(stderr, "  [tin] absinfo  X: %d..%d   Y: %d..%d   （屏幕 %dx%d）\n",
		ax_min_, ax_max_, ay_min_, ay_max_, scr_w_, scr_h_);
}

int tin_init(const struct tin_cfg *cfg, int scr_w, int scr_h)
{
	scr_w_ = scr_w;
	scr_h_ = scr_h;
	cfg_ = *cfg;
	if (cfg_.deadzone <= 0) cfg_.deadzone = OVL_JOY_DZ;
	if (cfg_.radius <= 0)   cfg_.radius = OVL_JOY_R;
	if (cfg_.tap_ms <= 0)   cfg_.tap_ms = 220;
	if (cfg_.tap_slop <= 0) cfg_.tap_slop = 14;
	if (cfg_.pulse_ms <= 0) cfg_.pulse_ms = 120;

	fd_ = find_touch(cfg_.dev);
	if (fd_ < 0)
		return -1;
	read_absinfo();
	return 0;
}

void tin_close(void)
{
	if (fd_ >= 0)
		close(fd_);
	fd_ = -1;
}

int tin_dev_present(void) { return fd_ >= 0; }

void tin_pump(void)
{
	struct input_event ev;
	long t = now_ms();
	int a;

	for (a = 1; a < ACT_MAX; a++) {
		if (pulse_until_[a] && t >= pulse_until_[a]) {
			pulse_until_[a] = 0;
			src_pulse &= ~(1u << a);
			sync_actions();
		}
	}
	if (fd_ < 0)
		return;

	while (read(fd_, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X)
				raw_x_new_ = ev.value;
			else if (ev.code == ABS_Y)
				raw_y_new_ = ev.value;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			t_raw_x_ = raw_x_new_;
			t_raw_y_ = raw_y_new_;
			if (ev.value) {
				int x, y;
				normalize(raw_x_new_, raw_y_new_, &x, &y);
				on_down(x, y);
			} else {
				on_up();
			}
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (t_down_) {
				int x, y;
				t_raw_x_ = raw_x_new_;
				t_raw_y_ = raw_y_new_;
				normalize(raw_x_new_, raw_y_new_, &x, &y);
				if (x != t_x_ || y != t_y_)
					on_move(x, y);
			}
		}
	}
}

int tin_next_event(int *pressed, int *action)
{
	if (q_r_ == q_w_)
		return 0;
	*pressed = q_[q_r_].pressed;
	*action = q_[q_r_].action;
	q_r_ = (q_r_ + 1) % QLEN;
	return 1;
}

int tin_down(void)   { return t_down_; }
int tin_x(void)      { return t_x_; }
int tin_y(void)      { return t_y_; }
int tin_raw_x(void)  { return t_raw_x_; }
int tin_raw_y(void)  { return t_raw_y_; }
int tin_moving(void) { return t_moving_; }

void tin_fill_ovl(struct ovl_state *st)
{
	int n, i;

	st->page = page_;
	st->press = (t_zone_ == 3) ? t_btn_ : -1;
	n = ovl_btn_count(page_);
	for (i = 0; i < n && i < 8; i++)
		st->latched[i] = latched_[page_][i];
	st->joy = (t_down_ && t_zone_ != 3) ? t_zone_ : 0;
	st->joy_x = t_x_;
	st->joy_y = t_y_;
	st->joy_ox = t_ox_;
	st->joy_oy = t_oy_;
}
