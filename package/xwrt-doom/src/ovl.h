/*
 * ovl.h —— 屏幕控制条（虚拟按键）的版式、绘制与命中判定。
 *
 * 单点触摸的现实：CST353X 驱动只上报 ABS_X/ABS_Y + BTN_TOUCH（capabilities/abs = 0x3），
 * 拿不到第二个触点。所以「边跑边打」必须靠锁定式按钮（FIRE/RUN 点一下锁住），
 * 自由度交给两根拇指以外的那一根手指 —— 单手也能打通 E1M1。
 */
#ifndef OVL_H
#define OVL_H

#include <stdint.h>

#define OVL_BAR_H	40	/* 控制条高度（屏幕底部） */
#define OVL_ZONE_SPLIT	160	/* 左：移动/平移；右：转向 */
#define OVL_JOY_R	40	/* 虚拟摇杆可视半径 */
#define OVL_JOY_DZ	12	/* 摇杆死区（像素） */

struct ovl_btn {
	const char *label;
	int x, w;
	int action;
	int latch;	/* 1 = 点按锁定（再点解锁） */
	int danger;	/* 1 = 画成醒目的红色（EXIT 之类） */
};

struct ovl_state {
	int page;
	int press;			/* 当前按下的按钮下标，-1 表示无 */
	int latched[8];			/* 各按钮锁定态 */
	int blend_bar;			/* 1 = 控制条半透明（拉伸视图时用） */
	int show_stats;
	const char *stats;
	int joy;			/* 0 无 / 1 左区 / 2 右区 */
	int joy_x, joy_y;		/* 当前触点 */
	int joy_ox, joy_oy;		/* 摇杆原点 */
};

int ovl_bar_y(int scr_h);
int ovl_btn_count(int page);
const struct ovl_btn *ovl_btn(int page, int idx);
/* 该按钮对应的 ACT_* */
int ovl_btn_action(int page, int idx);
/* 命中返回按钮下标，未命中返回 -1 */
int ovl_hit(int scr_h, int page, int x, int y);
/* 点在控制条区域内（不论是否命中按钮） */
int ovl_in_bar(int scr_h, int y);

void ovl_draw(uint16_t *fb, int stride, int scr_w, int scr_h,
	      const struct ovl_state *st);

/* 给 doom_be14000.c 用的通用画字（状态行也用它） */
void ovl_text(uint16_t *fb, int stride, int scr_w, int scr_h, int x, int y,
	      const char *s, int scale, uint16_t fg);

#endif /* OVL_H */
