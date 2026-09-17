/*
 * ovl.c —— 控制条绘制与命中判定。
 *
 * 这里 include <math.h>，绝不能 include <linux/input.h>：后者的 KEY_* 与 doomkeys.h
 * 大面积重名。同一个理由，「动作」统一走 actions.h 的 ACT_*。
 */
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "ovl.h"
#include "actions.h"
#include "font5x7.h"

/* RGB565 —— 面板 plane 报 RG16，实测直写正确（bad-apple 已验证） */
#define C_BAR		0x1082	/* 控制条底：近黑 */
#define C_BAR_EDGE	0x7BEF
#define C_BTN		0x39E7	/* 按钮：中灰 */
#define C_BTN_PR	0x7BEF	/* 按下：亮灰 */
#define C_BTN_ON	0xFBE0	/* 锁定：琥珀 */
#define C_BTN_RED	0xB104	/* 危险按钮（EXIT）：暗红 */
#define C_BTN_RED_HI	0xFA20	/* 危险按钮按下：亮红 */
#define C_TXT		0xFFFF
#define C_TXT_ON	0x0000
#define C_HINT		0x52AA
#define C_HINT_HI	0xFFFF
#define C_SHADOW	0x0000

#define NBTN_MAX 8

/* 第 0 页：游戏中 */
static const struct ovl_btn page0[] = {
	{ "MNU",   0,  48, ACT_PAGE, 0, 0 },
	{ "RUN",  48,  64, ACT_RUN,  1, 0 },
	{ "USE", 112,  64, ACT_USE,  0, 0 },
	{ "FIRE",176, 144, ACT_FIRE, 1, 0 },
};

/* 第 1 页：菜单 / 存档 / 地图 / 亮度 / 退出
 *
 * ★ EXIT 是必需品，不是装饰：Doom 1.9 的菜单里**没有 Quit Game**（那是后来的
 *   source port 才加的）。没有这个按钮，doom 一旦全屏跑起来，用户就只能 SSH
 *   进去 kill 它 —— 那就谈不上「可游玩」了。所以给它最宽的一格 + 红色底。 */
static const struct ovl_btn page1[] = {
	{ "ESC",   0, 40, ACT_ESCAPE, 0, 0 },
	{ "ENT",  40, 44, ACT_ENTER,  0, 0 },
	{ "Y",    84, 32, ACT_YES,    0, 0 },
	{ "N",   116, 32, ACT_NO,     0, 0 },
	{ "GAM", 148, 44, ACT_GAMMA,  0, 0 },
	{ "MAP", 192, 44, ACT_MAP,    0, 0 },
	{ "EXIT",236, 84, ACT_QUIT,   0, 1 },
};

int ovl_bar_y(int scr_h)
{
	return scr_h - OVL_BAR_H;
}

int ovl_btn_count(int page)
{
	return page ? (int)(sizeof(page1) / sizeof(page1[0]))
		    : (int)(sizeof(page0) / sizeof(page0[0]));
}

const struct ovl_btn *ovl_btn(int page, int idx)
{
	const struct ovl_btn *t = page ? page1 : page0;
	int n = ovl_btn_count(page);

	if (idx < 0 || idx >= n)
		return NULL;
	return &t[idx];
}

int ovl_btn_action(int page, int idx)
{
	const struct ovl_btn *b = ovl_btn(page, idx);

	return b ? b->action : ACT_NONE;
}

int ovl_in_bar(int scr_h, int y)
{
	return y >= ovl_bar_y(scr_h);
}

int ovl_hit(int scr_h, int page, int x, int y)
{
	int by = ovl_bar_y(scr_h);
	int n = ovl_btn_count(page);
	int i;

	if (y < by || y >= by + OVL_BAR_H)
		return -1;
	for (i = 0; i < n; i++) {
		const struct ovl_btn *b = ovl_btn(page, i);
		if (x >= b->x && x < b->x + b->w)
			return i;
	}
	return -1;
}

/* ── 基础绘制原语 ─────────────────────────────────────────────── */
static void px(uint16_t *fb, int stride, int scr_w, int scr_h, int x, int y,
	       uint16_t c)
{
	if (x < 0 || y < 0 || x >= scr_w || y >= scr_h)
		return;
	fb[(size_t)y * stride + x] = c;
}

static void px_blend(uint16_t *fb, int stride, int scr_w, int scr_h, int x,
		     int y, uint16_t c)
{
	uint16_t d;

	if (x < 0 || y < 0 || x >= scr_w || y >= scr_h)
		return;
	d = fb[(size_t)y * stride + x];
	/* RGB565 五五混合（绿色 6 位，0xF7DE 掩码把最低位抹平） */
	fb[(size_t)y * stride + x] =
		(uint16_t)(((d & 0xF7DE) >> 1) + ((c & 0xF7DE) >> 1));
}

static void rect_fill(uint16_t *fb, int stride, int scr_w, int scr_h, int x,
		      int y, int w, int h, uint16_t c, int blend)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			if (blend)
				px_blend(fb, stride, scr_w, scr_h, x + i,
					 y + j, c);
			else
				px(fb, stride, scr_w, scr_h, x + i, y + j, c);
		}
}

static void rect_edge(uint16_t *fb, int stride, int scr_w, int scr_h, int x,
		      int y, int w, int h, uint16_t c)
{
	int i;

	for (i = 0; i < w; i++) {
		px(fb, stride, scr_w, scr_h, x + i, y, c);
		px(fb, stride, scr_w, scr_h, x + i, y + h - 1, c);
	}
	for (i = 0; i < h; i++) {
		px(fb, stride, scr_w, scr_h, x, y + i, c);
		px(fb, stride, scr_w, scr_h, x + w - 1, y + i, c);
	}
}

void ovl_text(uint16_t *fb, int stride, int scr_w, int scr_h, int x, int y,
	      const char *s, int scale, uint16_t fg)
{
	int cx = x;

	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		const uint8_t *g;
		int r, c;

		if (ch < 32 || ch > 127)
			ch = ' ';
		g = font5x7[ch - 32];
		for (r = 0; r < FONT_H; r++) {
			for (c = 0; c < FONT_W; c++) {
				int dx, dy;
				if (!((g[r] >> (FONT_W - 1 - c)) & 1))
					continue;
				for (dy = 0; dy < scale; dy++)
					for (dx = 0; dx < scale; dx++)
						px(fb, stride, scr_w, scr_h,
						   cx + c * scale + dx,
						   y + r * scale + dy, fg);
			}
		}
		cx += (FONT_W + 1) * scale;
	}
}

static void text_shadow(uint16_t *fb, int stride, int scr_w, int scr_h, int x,
			int y, const char *s, int scale, uint16_t fg)
{
	ovl_text(fb, stride, scr_w, scr_h, x + 1, y + 1, s, scale, C_SHADOW);
	ovl_text(fb, stride, scr_w, scr_h, x, y, s, scale, fg);
}

static int text_w(const char *s, int scale)
{
	int n = 0;

	while (s[n])
		n++;
	return n ? n * (FONT_W + 1) * scale - scale : 0;
}

/* ── 控制条 ───────────────────────────────────────────────────── */
static void draw_bar(uint16_t *fb, int stride, int scr_w, int scr_h,
		     const struct ovl_state *st)
{
	int by = ovl_bar_y(scr_h);
	int n = ovl_btn_count(st->page);
	int i, k;

	rect_fill(fb, stride, scr_w, scr_h, 0, by, scr_w, OVL_BAR_H, C_BAR,
		  st->blend_bar);
	rect_fill(fb, stride, scr_w, scr_h, 0, by, scr_w, 1, C_BAR_EDGE, 0);

	for (i = 0; i < n; i++) {
		const struct ovl_btn *b = ovl_btn(st->page, i);
		uint16_t idle = b->danger ? C_BTN_RED : C_BTN;
		uint16_t down = b->danger ? C_BTN_RED_HI : C_BTN_PR;
		uint16_t fill = st->latched[i] ? C_BTN_ON : idle;
		int tw, tx, ty;

		if (st->press == i)
			fill = st->latched[i] ? C_BTN_ON : down;

		rect_fill(fb, stride, scr_w, scr_h, b->x + 1, by + 2, b->w - 2,
			  OVL_BAR_H - 5, fill, st->blend_bar);
		rect_edge(fb, stride, scr_w, scr_h, b->x + 1, by + 2, b->w - 2,
			  OVL_BAR_H - 5, C_BAR_EDGE);

		tw = text_w(b->label, 2);
		tx = b->x + (b->w - tw) / 2;
		ty = by + (OVL_BAR_H - FONT_H * 2) / 2;
		ovl_text(fb, stride, scr_w, scr_h, tx, ty, b->label, 2,
			 (st->latched[i] && st->press != i) ? C_TXT_ON : C_TXT);
	}

	/* 左右功能区分界线：提示「左滑移动 / 右滑转向」 */
	for (k = 0; k < by; k += 4) {
		px_blend(fb, stride, scr_w, scr_h, OVL_ZONE_SPLIT, k, C_HINT);
		px_blend(fb, stride, scr_w, scr_h, OVL_ZONE_SPLIT, k + 1, C_HINT);
	}
	text_shadow(fb, stride, scr_w, scr_h, 6, 6, "MOVE", 1, C_HINT);
	text_shadow(fb, stride, scr_w, scr_h, OVL_ZONE_SPLIT + 6, 6, "TURN", 1,
		    C_HINT);
}

/* 摇杆 / 转向现场提示：只画淡轮廓，不遮画面 */
static void draw_joy(uint16_t *fb, int stride, int scr_w, int scr_h,
		     const struct ovl_state *st)
{
	int cx, cy, i;

	if (!st->joy)
		return;

	cx = st->joy_ox;
	cy = st->joy_oy;

	if (st->joy == 1) {
		for (i = 0; i < 64; i++) {
			double a = i * 6.283185307179586 / 64.0;
			int x = cx + (int)(OVL_JOY_R * 0.75 * cos(a));
			int y = cy + (int)(OVL_JOY_R * 0.75 * sin(a));
			px_blend(fb, stride, scr_w, scr_h, x, y, C_HINT);
		}
		for (i = -OVL_JOY_R; i <= OVL_JOY_R; i += 2) {
			px_blend(fb, stride, scr_w, scr_h, cx + i, cy, C_HINT);
			px_blend(fb, stride, scr_w, scr_h, cx, cy + i, C_HINT);
		}
	} else {
		int y = cy;
		for (i = -16; i <= 16; i++) {
			px_blend(fb, stride, scr_w, scr_h, cx + i * 3, y, C_HINT);
			px_blend(fb, stride, scr_w, scr_h, cx + i * 3, y + 1,
				 C_HINT);
		}
	}

	for (i = -3; i <= 3; i++)
		px_blend(fb, stride, scr_w, scr_h, st->joy_x + 1,
			 st->joy_y + i, C_HINT_HI);
}

void ovl_draw(uint16_t *fb, int stride, int scr_w, int scr_h,
	      const struct ovl_state *st)
{
	draw_joy(fb, stride, scr_w, scr_h, st);
	draw_bar(fb, stride, scr_w, scr_h, st);

	if (st->show_stats && st->stats && st->stats[0]) {
		int w = text_w(st->stats, 1) + 6;
		rect_fill(fb, stride, scr_w, scr_h, 2, scr_h - OVL_BAR_H - 12,
			  w, FONT_H + 4, 0x0000, 1);
		ovl_text(fb, stride, scr_w, scr_h, 5, scr_h - OVL_BAR_H - 10,
			 st->stats, 1, C_TXT);
	}
}
