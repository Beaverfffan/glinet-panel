/*
 * panel_out.h —— BE14000 机身屏（panel-mipi-dbi / ST7789 系 320x240）DRM 输出。
 *
 * 为什么不用 /dev/fb0：fbdev 模拟是「影子缓冲 + 异步脏页刷新」，连续写会被合并，
 * 无法决定「这一帧什么时候出去」（实测连写 20 帧只推出 0~3 帧）。
 * 这里直接走 DRM：双 dumb buffer 交替 drmModeSetPlane ⇒ 内核每次 commit 真推一整帧。
 */
#ifndef PANEL_OUT_H
#define PANEL_OUT_H

#include <stdint.h>

/* 初始化并接管面板。成功返回 0。 */
int panel_out_init(int w, int h);

int panel_out_width(void);
int panel_out_height(void);

/* 取「当前可写」的后台缓冲（w*h 个 RGB565 像素，行距 = width()）。 */
uint16_t *panel_out_backbuf(void);

/* 提交后台上屏并切到另一张缓冲。成功返回 0。 */
int panel_out_submit(void);

/* 已成功提交的帧数 */
unsigned long panel_out_frames(void);

/* SPI 累计字节 / 换算成整帧数 —— 判断「真的上屏了几帧」的唯一硬证据：
 * 一次全帧 commit = +153,611 B（320x240x2 + 11 B 命令）。 */
unsigned long panel_out_spi_bytes(void);
float panel_out_spi_frames(void);

void panel_out_close(void);

#endif /* PANEL_OUT_H */
