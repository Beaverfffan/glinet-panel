/*
 * tinput.h —— 触摸输入 → 玩家意图。
 *
 * 硬件事实（2026-09-17 真机核实）：
 *   /dev/input/event0 = "Hynitron CST353X Touchscreen"
 *   capabilities/abs = 0x3  ⇒ 只有 ABS_X / ABS_Y，**单点触摸**（没有 ABS_MT_*）
 *   capabilities/key = 0x400 ⇒ BTN_TOUCH
 *   DTS：touchscreen-size-x=240 / size-y=320 + swapped-x-y + inverted-x + inverted-y
 *   驱动 cst353x.c 走 touchscreen_parse_properties() + touchscreen_report_pos()，
 *   且 RAW 读取时已做过一次 X 反相 ⇒ 两层反相 + 交换之后，
 *   **evdev 的 ABS_X ∈ [0,319] 就是屏幕 X，ABS_Y ∈ [0,239] 就是屏幕 Y**，无需再摆弄。
 *   （推导见 BE14000-fixes/DOOM-ON-PANEL.md；仍留 swap/invert 开关以便真机兜底。）
 */
#ifndef TINPUT_H
#define TINPUT_H

#include "ovl.h"

struct tin_cfg {
	const char *dev;	/* NULL 或 "auto" = 按能力自动找 */
	int swap_xy;
	int invert_x;
	int invert_y;
	int deadzone;		/* 摇杆死区（像素） */
	int radius;		/* 摇杆可视/最大半径（像素） */
	int tap_ms;		/* 点按判定时长上限 */
	int tap_slop;		/* 点按允许位移（像素） */
	int pulse_ms;		/* FIRE/USE 这类「一下」动作的按住时长 */
};

int  tin_init(const struct tin_cfg *cfg, int scr_w, int scr_h);
void tin_pump(void);
void tin_close(void);

/* 取一个动作事件：返回 1 = 有事件，(pressed, action) 已填；0 = 无 */
int  tin_next_event(int *pressed, int *action);

/* 位置/原始值（--touch-test 与排障用） */
int  tin_down(void);
int  tin_x(void);
int  tin_y(void);
int  tin_raw_x(void);
int  tin_raw_y(void);
int  tin_dev_present(void);

/* 把状态搬进 ovl_state 供绘制 */
void tin_fill_ovl(struct ovl_state *st);

/* 触摸是否在移动（画摇杆提示用） */
int  tin_moving(void);

#endif /* TINPUT_H */
