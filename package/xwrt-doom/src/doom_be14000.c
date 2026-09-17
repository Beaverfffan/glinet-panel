/*
 * doom_be14000.c —— doomgeneric 在 GL.iNet GL-BE14000 机身屏上的平台层。
 *
 *   屏幕：panel-mipi-dbi（ST7789P3 系）320x240，SPI0 @ 52 MHz
 *   触摸：Hynitron CST3533/CST353X 电容屏，evdev，**单点**
 *   输出：DRM 双 dumbbuffer + drmModeSetPlane（不用 /dev/fb0，理由见 panel_out.c）
 *
 * Doom 原生 320x200，而面板是 4:3 的 320x240 —— 把 200 行纵向拉伸 1.2 倍
 * 恰好就是当年 CRT 上的正确比例（Doom 像素本是 1.2:1 的）。所以「拉伸」不是凑合，
 * 是还原。默认视图模式 view=reserve（把底部 40 行让给控制条，1:1 不经插值，
 * 观感是横向拉宽 20%），可在 uci 里改成 view=stretch（比例正确 + 控制条半透明压在画面下缘）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/prctl.h>
#ifndef PR_SET_TIMERSLACK
#define PR_SET_TIMERSLACK 29
#endif

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "doomstat.h"	/* gametic：用来判定游戏速度对不对 */

#include "panel_out.h"
#include "ovl.h"
#include "tinput.h"
#include "actions.h"

#define SCR_W 320
#define SCR_H 240
#define VIEW_W 320
#define VIEW_H 200

/* init 脚本 / screen-guard.sh 靠这个文件精确认人。
 * ⚠️ 绝对不要用 `pgrep -x xwrt-doom` 认自己：BusyBox 的 procps 会把
 *    `/bin/sh /etc/rc.common /etc/init.d/xwrt-doom start` 这种进程的 comm
 *    也报成 "xwrt-doom"，于是 init 脚本会把自己当 doom —— 实测后果是
 *    init 里的 kill_doom() 拿 SIGTERM 打中正在跑 start_service 的那个 shell，
 *    procd_open_instance 根本没执行（"服务说启动了，屏幕上什么都没有"）。 */
#define PIDFILE "/var/run/xwrt-doom.pid"

/* ── 配置 ─────────────────────────────────────────────────────── */
struct opts {
	const char *wad;
	const char *touch_dev;
	int fps;
	int view;		/* 0 = reserve（底部 40 行给控制条）1 = stretch（拉伸铺满） */
	int stats;
	int swap_xy, inv_x, inv_y;
	int touch_test;
	int deadzone, radius, tap_ms, pulse_ms;
	int no_submit;		/* 只画不提交：测「引擎+绘制」的天花板 */
	int spin_sleep;		/* 1 = 短睡眠改用自旋（默认 nanosleep，便于观察实睡时长） */
	int touch_echo;		/* 把每个触摸事件连同坐标印出来（远程标定用） */
};

static struct opts o = {
	.wad = "/usr/share/xwrt-doom/doom1.wad",
	.touch_dev = "auto",
	/* 0 = 不设上限。理由见 DG_DrawFrame 里的注释：引擎每帧只烧 1.1 ms CPU，
	 * 真正定帧率的是 drmModeSetPlane 里同步等 SPI 吐完整帧的那 ~26 ms，
	 * 也就是这块屏的物理上限（约 38 fps，Doom 引擎是 35 Hz，正好够）。
	 * 想省 CPU/降 SPI 负载再显式给 --fps 25 之类。 */
	.fps = 0,
	.view = 0,
	.stats = 0,
	.tap_ms = 220,
};

static struct ovl_state st_;
static volatile sig_atomic_t g_quit;
static unsigned long drawn_;
static double t_start_;
static long ms_start_;
static double t_last_frame_;
/* SPI 计数器是**自开机累计**的绝对值，判"本次上了几帧"必须减掉起始快照。
 * 不减的话会印出 1.8e10 B ⇒ 122100 帧 (4686 fps) 这种荒唐数字（踩过）。 */
static unsigned long spi_start_;

/* ── 性能分项计时 ──────────────────────────────────────────────
 * 只印一次平均值的"体检报告"。分三项是为了分清瓶颈到底在谁身上：
 *   loop   = 两次 DG_DrawFrame 之间的全部时间（= 1/fps，含引擎 tick 和 I_Sleep）
 *   draw   = blit_doom + ovl_draw（纯内存/CPU）
 *   commit = drmModeSetPlane（等 SPI 把整帧吐出去）
 *   sleep  = Doom 的 TryRunTics 等下一个 35 Hz tic 所花的时间
 *           req vs act 如果差得离谱，说明 1ms 的 nanosleep 睡成了十几毫秒。
 */
static double pf_loop, pf_draw, pf_commit, pf_sleep_req, pf_sleep_act;
static unsigned long pf_calls, pf_frames, pf_sleeps;

/* 节流门的令牌桶状态（含义见 DG_DrawFrame） */
static double gate_t_, gate_credit_;
static int gate_init_;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

/* ── 动作 → Doom 键码（只在这一处做翻译） ─────────────────────── */
/* actions.h 里声明的是全局符号（tinput / ovl 也想用它印日志），所以这里不能加 static */
const char *act_name(int a)
{
	static const char *n[ACT_MAX] = {
		[ACT_FORWARD]  = "FWD",
		[ACT_BACK]     = "BACK",
		[ACT_TURN_L]   = "TURN-L",
		[ACT_TURN_R]   = "TURN-R",
		[ACT_STRAFE_L] = "STR-L",
		[ACT_STRAFE_R] = "STR-R",
		[ACT_FIRE]     = "FIRE",
		[ACT_USE]      = "USE",
		[ACT_RUN]      = "RUN",
		[ACT_ESCAPE]   = "ESC",
		[ACT_ENTER]    = "ENTER",
		[ACT_YES]      = "YES",
		[ACT_NO]       = "NO",
		[ACT_MAP]      = "MAP",
		[ACT_GAMMA]    = "GAMMA",
		[ACT_QUIT]     = "QUIT",
		[ACT_PAGE]     = "PAGE",
	};

	return (a > 0 && a < ACT_MAX && n[a]) ? n[a] : "?";
}

static unsigned char act_to_doom(int a)
{
	switch (a) {
	case ACT_FORWARD:  return KEY_UPARROW;
	case ACT_BACK:     return KEY_DOWNARROW;
	case ACT_TURN_L:   return KEY_LEFTARROW;
	case ACT_TURN_R:   return KEY_RIGHTARROW;
	case ACT_STRAFE_L: return KEY_STRAFE_L;
	case ACT_STRAFE_R: return KEY_STRAFE_R;
	case ACT_FIRE:     return KEY_FIRE;
	case ACT_USE:      return KEY_USE;
	case ACT_RUN:      return KEY_RSHIFT;
	case ACT_ESCAPE:   return KEY_ESCAPE;
	case ACT_ENTER:    return KEY_ENTER;
	case ACT_YES:      return 'y';
	case ACT_NO:       return 'n';
	case ACT_MAP:      return KEY_TAB;
	case ACT_GAMMA:    return KEY_F11;
	default:           return 0;
	}
}

/* ── 画面 ─────────────────────────────────────────────────────── */
/* Doom 320x200 RGB565（-gfxmode rgb565 直出，无需逐像素转换）→ 面板缓冲 */
static void blit_doom(uint16_t *dst, int stride)
{
	const uint16_t *src = (const uint16_t *)DG_ScreenBuffer;
	int y;

	if (o.view == 0) {
		for (y = 0; y < VIEW_H; y++)
			memcpy(dst + (size_t)y * stride, src + (size_t)y * VIEW_W,
			       VIEW_W * 2);
	} else {
		/* 纵向 1.2x 最近邻：200 → 240，还原 4:3 */
		for (y = 0; y < SCR_H; y++) {
			const uint16_t *s = src +
				(size_t)((y * VIEW_H) / SCR_H) * VIEW_W;
			memcpy(dst + (size_t)y * stride, s, VIEW_W * 2);
		}
	}
}

static void draw_touch_test(uint16_t *dst, int stride)
{
	static int last_rx = -1, last_ry = -1;
	int x = tin_x(), y = tin_y();
	int i;

	/* 四角标记 + 一个大十字准星：一眼看出「手指位置 = 画面位置」对不对 */
	for (i = 0; i < 12; i++) {
		ovl_text(dst, stride, SCR_W, SCR_H,  4,     4 + i, "X", 1, 0xF800);
		ovl_text(dst, stride, SCR_W, SCR_H, SCR_W - 10, 4 + i, "X", 1, 0xF800);
		break;
	}
	ovl_text(dst, stride, SCR_W, SCR_H, 4, 4, "TL", 1, 0xF800);
	ovl_text(dst, stride, SCR_W, SCR_H, SCR_W - 24, 4, "TR", 1, 0xF800);
	ovl_text(dst, stride, SCR_W, SCR_H, 4, SCR_H - OVL_BAR_H - 14, "BL", 1, 0xF800);
	ovl_text(dst, stride, SCR_W, SCR_H, SCR_W - 24, SCR_H - OVL_BAR_H - 14,
		 "BR", 1, 0xF800);

	for (i = -40; i <= 40; i++) {
		int yy = y + i, xx = x + i;
		if (yy >= 0 && yy < SCR_H)
			ovl_text(dst, stride, SCR_W, SCR_H, x, yy, "|", 1, 0x07E0);
		if (xx >= 0 && xx < SCR_W)
			ovl_text(dst, stride, SCR_W, SCR_H, xx - 2, y, "-", 1, 0x07E0);
	}
	(void)last_rx;
	(void)last_ry;
}

/* 首屏：别让用户看到上一帧残留 */
static void splash(const char *msg)
{
	uint16_t *dst = panel_out_backbuf();
	int i;

	for (i = 0; i < SCR_W * SCR_H; i++)
		dst[i] = 0x0000;
	ovl_text(dst, SCR_W, SCR_W, SCR_H, 60, 60, "DOOM", 4, 0xF800);
	ovl_text(dst, SCR_W, SCR_W, SCR_H, 60, 100, "ON", 2, 0xFFFF);
	ovl_text(dst, SCR_W, SCR_W, SCR_H, 100, 100, "BE14000", 2, 0xFFFF);
	ovl_text(dst, SCR_W, SCR_W, SCR_H, 20, 150, msg, 1, 0xFFFF);
}

/* ── doomgeneric 平台钩子 ─────────────────────────────────────── */
void DG_Init(void)
{
	int tfd_ok;

	fprintf(stderr, "=== doomgeneric / GL-BE14000 ===\n");
	/* 定时器松弛默认 50us 起，在 1ms 级睡眠上会被放大成十几毫秒 —— 直接压到 1ns */
	prctl(PR_SET_TIMERSLACK, 1);
	if (panel_out_init(SCR_W, SCR_H) < 0) {
		fprintf(stderr, "!! 面板 DRM 初始化失败\n");
		exit(1);
	}

	{
		struct tin_cfg tc;
		memset(&tc, 0, sizeof(tc));
		tc.dev = o.touch_dev;
		tc.swap_xy = o.swap_xy;
		tc.invert_x = o.inv_x;
		tc.invert_y = o.inv_y;
		tc.deadzone = o.deadzone;
		tc.radius = o.radius;
		tc.tap_ms = o.tap_ms;
		tc.pulse_ms = o.pulse_ms;
		tfd_ok = tin_init(&tc, SCR_W, SCR_H) == 0;
	}
	if (!tfd_ok)
		fprintf(stderr, "  [tin] warn: 没有触摸设备，只能用键盘/演示模式\n");

	splash("LOADING WAD ...");
	panel_out_submit();
	memset(panel_out_backbuf(), 0, SCR_W * SCR_H * 2);
	panel_out_submit();

	t_start_ = now_s();
	t_last_frame_ = t_start_;
	spi_start_ = panel_out_spi_bytes();	/* 后面算"本次上了几帧"要减它 */
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ms_start_ = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	{
		FILE *f = fopen(PIDFILE, "w");
		if (f) {
			fprintf(f, "%d\n", (int)getpid());
			fclose(f);
		}
	}

	fprintf(stderr, "  [cfg] fps=%d view=%s stats=%d touch_test=%d no_submit=%d "
		"spin_sleep=%d wad=%s\n",
		o.fps, o.view ? "stretch" : "reserve", o.stats, o.touch_test,
		o.no_submit, o.spin_sleep, o.wad);
}

void DG_DrawFrame(void)
{
	double t = now_s(), t0, t1, t2;
	uint16_t *dst;

	/* 循环周期：无论这帧提不提交都要记 —— 它才是帧率的倒数 */
	pf_loop += t - t_last_frame_;
	t_last_frame_ = t;
	pf_calls++;

	tin_pump();

	/* ★ 节流门必须用**令牌桶**，不能写成「距上次提交 >= 1/fps 就放行」。
	 * 原因：Doom 的渲染是被 35 Hz 的 game tic 驱动的（帧间隔 28.57 ms），
	 * 而 1/30 = 33.33 ms 恰好只比它大一点点 ⇒ 每次都得等到第 2 个 tic 才过门，
	 * 帧率被**拍频**腰斩成 35/2 = 17.5 fps（实测 17.43，分毫不差），
	 * 而同一份代码把 fps 设成 0（无门）立刻跑出 36.4 fps。
	 * 令牌桶按「已累计多少帧预算」记账，允许在攒到 85% 时就提前放行，
	 * 长期平均帧率仍是 1/fps，但不会再和 tic 周期共振。
	 * fps<=0 = 完全不设上限（用来量引擎的真实天花板）。 */
	if (o.fps > 0) {
		double T = 1.0 / o.fps;

		if (!gate_init_) {
			gate_init_ = 1;
			gate_t_ = t;
			gate_credit_ = 1.0;	/* 首帧立刻放行 */
		}
		gate_credit_ += (t - gate_t_) / T;
		gate_t_ = t;
		if (gate_credit_ > 2.0)
			gate_credit_ = 2.0;
		if (gate_credit_ < 0.85)
			return;
		gate_credit_ -= 1.0;
	}

	t0 = now_s();
	dst = panel_out_backbuf();
	blit_doom(dst, SCR_W);

	memset(&st_, 0, sizeof(st_));
	tin_fill_ovl(&st_);
	st_.blend_bar = (o.view != 0);
	st_.show_stats = o.stats;
	if (o.stats) {
		static char buf[48];
		double el = t - t_start_;
		snprintf(buf, sizeof(buf), "FPS %.1f  TGT %d  D %lu",
			 drawn_ / (el > 0.5 ? el : 1), o.fps, drawn_);
		st_.stats = buf;
	}
	ovl_draw(dst, SCR_W, SCR_W, SCR_H, &st_);
	if (o.touch_test)
		draw_touch_test(dst, SCR_W);
	t1 = now_s();

	if (!o.no_submit)
		panel_out_submit();
	t2 = now_s();

	pf_draw += t1 - t0;
	pf_commit += t2 - t1;
	pf_frames++;
	drawn_++;
	if (drawn_ <= 3 || (drawn_ % 120) == 0)
		fprintf(stderr, "  [dbg] frame %lu\n", drawn_);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	int on, act;

	if (g_quit)
		I_Quit();

	tin_pump();
	while (tin_next_event(&on, &act)) {
		unsigned char k;

		if (o.touch_echo)
			fprintf(stderr, "  [tin] %-4s %-7s raw=(%4d,%4d) "
				"scr=(%3d,%3d) down=%d\n",
				on ? "DOWN" : "up", act_name(act),
				tin_raw_x(), tin_raw_y(), tin_x(), tin_y(),
				tin_down());

		if (act == ACT_QUIT) {
			/* 屏幕上那个红色的 EXIT：Doom 自己不会退，平台层来退。
			 * 用 I_Quit() 走 doomgeneric 的正常退出路径（会跑 atexit）。 */
			fprintf(stderr, "  [tin] EXIT 按钮 ⇒ 退出\n");
			g_quit = 1;
			I_Quit();
			return 0;
		}

		k = act_to_doom(act);
		if (!k)
			continue;
		*pressed = on;
		*key = k;
		return 1;
	}
	return 0;
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec t0, t1;
	double req, act;

	tin_pump();
	if (ms == 0)
		return;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (o.spin_sleep && ms <= 3) {
		/* 备选实现：Doom 的 TryRunTics() 是「I_Sleep(1) 直到下一个 35 Hz tic
		 * 到点」。若内核把 1ms 的 nanosleep 折到下一个 jiffy（低分辨率定时器 /
		 * 大 timer slack），每次都要多睡几倍 ⇒ 35 Hz 可能被腰斩。先用
		 * nanosleep 量出真实实睡时长，再决定要不要换自旋。 */
		double end = now_s() + ms / 1000.0;
		while (now_s() < end)
			;
	} else {
		struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
		nanosleep(&ts, NULL);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	req = ms / 1000.0;
	act = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	pf_sleep_req += req;
	pf_sleep_act += act;
	pf_sleeps++;
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec ts;
	long ms;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
	return (uint32_t)(ms - ms_start_);
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

/* ── 收尾：退出时把「真上屏了几帧」印出来 ─────────────────────── */
static void cleanup(void)
{
	double el = now_s() - t_start_;
	unsigned long spi_now = panel_out_spi_bytes();
	/* ⚠️ SPI 计数器是**自开机累计**的绝对值，必须减掉起始快照。
	 * 不减的话会印出 `SPI 累计 18755952392 B ⇒ 122100.3 帧 (4686.2 fps)`
	 * 这种夸张的假数字（真机上踩过），把人往错误方向带。 */
	unsigned long spi = spi_now - spi_start_;

	fprintf(stderr,
		"==== 提交 %lu 帧 / %.2fs = %.2f fps；本会话 SPI Δ %lu B "
		"⇒ 实际推上屏 %.1f 帧 (%.1f fps)\n"
		"     （SPI 绝对值 %lu B，减掉起始快照 %lu B 才是本次的量）\n",
		drawn_, el, el > 0 ? drawn_ / el : 0, spi, spi / 153611.0,
		el > 0 ? spi / 153611.0 / el : 0, spi_now, spi_start_);

	if (pf_calls)
		fprintf(stderr,
			"     分项  每次循环 %.2f ms（%lu 次，%.1f 次/s）│ "
			"每帧：绘制 %.2f ms + 提交 %.2f ms（%lu 帧）\n",
			pf_loop * 1000 / pf_calls, pf_calls,
			pf_loop > 0 ? pf_calls / pf_loop : 0,
			pf_frames ? pf_draw * 1000 / pf_frames : 0.0,
			pf_frames ? pf_commit * 1000 / pf_frames : 0.0, pf_frames);
	if (pf_sleeps)
		fprintf(stderr, "     睡眠  req %.3f ms → act %.3f ms（%lu 次，%s）\n",
			pf_sleep_req * 1000 / pf_sleeps, pf_sleep_act * 1000 / pf_sleeps,
			pf_sleeps, o.spin_sleep ? "自旋" : "nanosleep");
	/* 游戏速度的判据：gametic 应该按 35 Hz 涨。若 10 秒涨了 ~700 就是两倍速，
	 * 那说明 I_GetTime() 的时基不对（Doom 的时间标尺全挂在它上面）。 */
	if (el > 1.0)
		fprintf(stderr, "     tic   gametic=%d / %.2fs = %.1f Hz "
			"（Doom 标称 35 Hz；明显偏离就是时间标尺错了）\n",
			gametic, el, gametic / el);
	tin_close();
	panel_out_close();
	unlink(PIDFILE);
}

/* ── main ─────────────────────────────────────────────────────── */
static void usage(const char *p)
{
	fprintf(stderr,
"用法: %s [选项] [Doom 参数...]\n"
"  --wad <路径>        IWAD（默认 /usr/share/xwrt-doom/doom1.wad）\n"
"  --touch-dev <路径>  触摸设备，默认 auto（按能力自动找）\n"
"  --fps <n>           上屏帧率上限，默认 0 = 不限制\n"
"                     （实际上限由 SPI 带宽定，单帧 153,611 B @52MHz ≈ 23.6ms ⇒ 约 36~38fps）\n"
"  --view reserve|stretch  reserve=底部 40 行留给控制条（默认）；stretch=拉伸铺满 4:3\n"
"  --stats             左上角叠一行帧率\n"
"  --touch-test        触摸标定模式：全屏十字准星跟着手指\n"
"  --no-submit         只绘制不提交（测引擎+绘制的上限，画面不动）\n"
"  --spin-sleep        短睡眠用自旋代替 nanosleep\n"
"  --touch-echo        把每个触摸事件（原始坐标/屏幕坐标/命中动作）打到 stderr\n"
"  --swap/--invert-x/--invert-y   触摸轴兜底开关\n"
"  --deadzone <px> --radius <px> --tap <ms> --pulse <ms>\n"
"  其余参数原样转给 Doom（如 -warp 1 1 直接进 E1M1）\n", p);
}

static int needs_value(const char *s)
{
	return !strcmp(s, "--wad") || !strcmp(s, "--touch-dev") ||
	       !strcmp(s, "--fps") || !strcmp(s, "--view") ||
	       !strcmp(s, "--deadzone") || !strcmp(s, "--radius") ||
	       !strcmp(s, "--tap") || !strcmp(s, "--pulse");
}

int main(int argc, char **argv)
{
	char **dargv;
	int dargc = 0, i, have_iwad = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		}
		if (!strcmp(argv[i], "-iwad"))
			have_iwad = 1;
	}

	dargv = calloc(argc + 12, sizeof(char *));
	if (!dargv)
		return 1;
	dargv[dargc++] = argv[0];

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (!strcmp(a, "--wad") && v)              { o.wad = v; i++; }
		else if (!strcmp(a, "--touch-dev") && v)   { o.touch_dev = v; i++; }
		else if (!strcmp(a, "--fps") && v)         { o.fps = atoi(v); i++; }
		else if (!strcmp(a, "--view") && v)        { o.view = strcmp(v, "stretch") == 0; i++; }
		else if (!strcmp(a, "--stats"))            { o.stats = 1; }
		else if (!strcmp(a, "--touch-test"))       { o.touch_test = 1; o.stats = 1; }
		else if (!strcmp(a, "--no-submit"))        { o.no_submit = 1; }
		else if (!strcmp(a, "--spin-sleep"))       { o.spin_sleep = 1; }
		else if (!strcmp(a, "--touch-echo"))       { o.touch_echo = 1; }
		else if (!strcmp(a, "--swap"))             { o.swap_xy = 1; }
		else if (!strcmp(a, "--invert-x"))         { o.inv_x = 1; }
		else if (!strcmp(a, "--invert-y"))         { o.inv_y = 1; }
		else if (!strcmp(a, "--deadzone") && v)    { o.deadzone = atoi(v); i++; }
		else if (!strcmp(a, "--radius") && v)      { o.radius = atoi(v); i++; }
		else if (!strcmp(a, "--tap") && v)         { o.tap_ms = atoi(v); i++; }
		else if (!strcmp(a, "--pulse") && v)       { o.pulse_ms = atoi(v); i++; }
		else if (needs_value(a) && v)              { i++; }	/* 认不出的选项吞掉它的值 */
		else if (a[0] == '-' && a[1] == '-')       { /* 未知长选项：忽略 */ }
		else                                        dargv[dargc++] = (char *)a;
	}

	if (!have_iwad) {
		dargv[dargc++] = "-iwad";
		dargv[dargc++] = (char *)o.wad;
	}
	dargv[dargc++] = "-nosound";	/* BE14000 没有扬声器 */
	dargv[dargc++] = "-gfxmode";
	dargv[dargc++] = "rgb565";	/* 直出 RGB565，省掉每帧 76800 次转换 */

	atexit(cleanup);
	/* ★ 关键：这个 port 的 D_DoomLoop() 不是死循环 —— 它只做一次「起搏」就返回，
	 *   无限循环必须由平台层的 main() 自己驱动（xlib / sdl / linuxvt 三个参考 port
	 *   都写成 while (1) doomgeneric_Tick();）。漏掉这一步的症状极具迷惑性：
	 *   一切初始化正常、第一帧也上了屏，然后进程以退出码 0 干净退出。 */
	doomgeneric_Create(dargc, dargv);
	while (!g_quit)
		doomgeneric_Tick();
	I_Quit();
	return 0;
}
