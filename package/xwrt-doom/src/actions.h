/*
 * actions.h —— 「玩家意图」中立层。
 *
 * 刻意不用 Doom 的 KEY_* 常量：<linux/input-event-codes.h> 和 doomkeys.h 有大量同名宏
 * （KEY_TAB / KEY_ENTER / KEY_MINUS / KEY_F1..F12 …），混在一个编译单元里必炸。
 * 触摸层只说 ACT_*，由 doom_be14000.c 一处翻译成 Doom 键码。
 */
#ifndef ACTIONS_H
#define ACTIONS_H

enum {
	ACT_NONE = 0,

	/* 移动 / 转向 */
	ACT_FORWARD,
	ACT_BACK,
	ACT_TURN_L,
	ACT_TURN_R,
	ACT_STRAFE_L,
	ACT_STRAFE_R,

	/* 动作 */
	ACT_FIRE,
	ACT_USE,
	ACT_RUN,
	ACT_ESCAPE,
	ACT_ENTER,
	ACT_YES,
	ACT_NO,
	ACT_MAP,
	ACT_GAMMA,

	/* 平台层自己消费，不翻译成 Doom 键码：
	 * Doom 1.9 的菜单里没有 Quit Game，屏幕上必须有个退出的办法。 */
	ACT_QUIT,

	/* 界面 */
	ACT_PAGE,	/* 控制条翻页 */

	ACT_MAX
};

const char *act_name(int a);

#endif /* ACTIONS_H */
