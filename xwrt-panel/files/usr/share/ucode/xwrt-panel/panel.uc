'use strict';

/*
 * xwrt-panel — minimal statistics panel for the GL-BE10000 / BE14000 TFT.
 *
 * X-Wrt style: three swipeable pages — usage, clients, wifi. No lock
 * screen, no PIN, no weather. Written from scratch on ucode-mod-lvgl,
 * using only the public lv/ubus/uci/uloop/nl80211 bindings.
 */

import * as lv from 'lv';
import * as ubus from 'ubus';
import * as uci from 'uci';
import * as uloop from 'uloop';
import { readfile, writefile, lsdir } from 'fs';

const W = 320;
const H = 240;
const POINTS = 28;

const C_SCREEN = 0x000000;
const C_SURFACE = 0x1c1c1e;
const C_RULE = 0x636366;
const C_TXT = 0xffffff;
const C_DIM = 0x9a9aa2;
const C_IDLE = 0x3e3e44;
const C_RX = 0x0a6cff;
const C_TX = 0x64d2ff;
const C_OK = 0x32d74b;
const C_DOWN = 0xff453a;

const FONT_DIR = '/usr/share/glinet-panel-ui/fonts';

/* ------------------------------------------------------------------ */
/* config                                                              */

let cfg = {
	brightness: 80,
	rotate: 0,	/* seconds, 0 = off */
	blank: 5,	/* minutes idle before backlight off, 0 = never */
	page_usage: true,
	page_clients: true,
	page_wifi: true
};

function truish(v) {
	return index([ '1', 'on', 'true', 'yes', 'enabled' ], lc(v ?? '')) >= 0;
}

function config_read() {
	let cursor = uci.cursor();

	if (!cursor.load('xwrt_panel'))
		return;

	let g = (name) => cursor.get('xwrt_panel', '@panel[0]', name);

	if (+g('brightness') >= 10 && +g('brightness') <= 100)
		cfg.brightness = +g('brightness');
	if (+g('rotate') >= 0)
		cfg.rotate = +g('rotate');
	if (+g('blank') >= 0)
		cfg.blank = +g('blank');
	if (g('page_usage') != null)
		cfg.page_usage = truish(g('page_usage'));
	if (g('page_clients') != null)
		cfg.page_clients = truish(g('page_clients'));
	if (g('page_wifi') != null)
		cfg.page_wifi = truish(g('page_wifi'));
}

/* ------------------------------------------------------------------ */
/* backlight                                                           */

let bl_path, bl_max = 100;

function backlight_find() {
	for (let d in lsdir('/sys/class/backlight') ?? []) {
		let max = readfile(sprintf('/sys/class/backlight/%s/max_brightness', d));

		if (max) {
			bl_path = sprintf('/sys/class/backlight/%s', d);
			bl_max = +trim(max) || 100;
			return;
		}
	}
}

function backlight_set(pct) {
	if (!bl_path)
		return;

	writefile(bl_path + '/brightness',
		  sprintf('%d', int(bl_max * pct / 100)));
}

/* ------------------------------------------------------------------ */
/* fonts / widgets                                                     */

function face(name) {
	try {
		return lv.font_load(sprintf('%s/%s.bin', FONT_DIR, name));
	}
	catch (e) {
		return null;
	}
}

let F_TITLE, F_BIG, F_MED, F_SMALL;

function fonts_load() {
	F_TITLE = face('inter_semibold_21') ?? face('inter_semibold_15');
	F_BIG = face('inter_light_45') ?? face('inter_regular_26') ?? F_TITLE;
	F_MED = face('inter_regular_15') ?? face('inter_regular_13');
	F_SMALL = face('inter_regular_13') ?? face('inter_regular_11') ?? F_MED;
}

function label_new(parent, font, colour, text) {
	let l = lv.label(parent);

	l.text(text);

	let st = { text_color: colour };

	if (font)
		st.text_font = font;

	l.style(st);

	return l;
}

function box_new(parent, colour, radius) {
	let b = lv.obj(parent);

	b.style({ bg_color: colour, radius, border_width: 0, pad_all: 0 });
	b.clickable(false);
	b.scrollable(false);

	return b;
}

/* label wrapper that only redraws on change */
function text_new(parent, font, colour, text) {
	return { obj: label_new(parent, font, colour, text), last: text };
}

function text_set(t, text) {
	if (t.last == text)
		return;

	t.last = text;
	t.obj.text(text);
}

/* text width with a usable fallback when a font failed to load */
function text_w(font, s) {
	return font ? lv.text_width(font, s) : 7 * length(s);
}

/* ------------------------------------------------------------------ */
/* data                                                                */

const state = {
	hostname: 'openwrt',
	clock: '',
	wan_device: null,
	wan_rx: null, wan_tx: null, wan_ts: null,
	rx_total: 0, tx_total: 0,
	rx: [], tx: [],
	rx_rate: 0, tx_rate: 0,
	clients: [],
	networks: []
};

function monotonic() {
	let c = clock(true);

	return c[0] + c[1] / 1000000000.0;
}

function ubus_call(object, method, data) {
	try {
		return ubus.call({ object, method, data: data ?? {} });
	}
	catch (e) {
		return null;
	}
}

function wan_device_read() {
	let wan = ubus_call('network.interface.wan', 'status');

	state.wan_device = wan?.l3_device ?? wan?.device;
}

function counter_read(dev, name) {
	let raw = readfile(sprintf('/sys/class/net/%s/statistics/%s', dev, name));

	return raw ? +trim(raw) : null;
}

function wan_read() {
	let now = monotonic();

	if (!state.wan_device)
		wan_device_read();

	let rx = state.wan_device ? counter_read(state.wan_device, 'rx_bytes') : null;
	let tx = state.wan_device ? counter_read(state.wan_device, 'tx_bytes') : null;

	if (rx == null || tx == null) {
		state.rx_rate = 0;
		state.tx_rate = 0;
	} else {
		let dt = state.wan_ts != null ? now - state.wan_ts : 0;

		if (dt > 0 && rx >= state.wan_rx && tx >= state.wan_tx) {
			state.rx_rate = int((rx - state.wan_rx) / dt);
			state.tx_rate = int((tx - state.wan_tx) / dt);
		}

		state.wan_rx = rx;
		state.wan_tx = tx;
		state.wan_ts = now;
		state.rx_total = rx;
		state.tx_total = tx;
	}

	push(state.rx, state.rx_rate);
	push(state.tx, state.tx_rate);

	while (length(state.rx) > POINTS)
		shift(state.rx);
	while (length(state.tx) > POINTS)
		shift(state.tx);
}

function words(line) {
	return filter(split(replace(trim(line), /[ \t]+/g, ' '), ' '),
		      p => p != '');
}

function clients_read() {
	let found = {};
	let text = readfile('/tmp/dhcp.leases');

	for (let line in split(text ?? '', '\n')) {
		let f = words(line);

		if (length(f) < 4)
			continue;

		found[lc(f[1])] = {
			mac: lc(f[1]),
			ip: f[2],
			hostname: f[3] != '*' ? f[3] : null
		};
	}

	/* dhcpsnoop (x-wrt) knows about clients dnsmasq has forgotten */
	let dump = ubus_call('dhcpsnoop', 'dump');

	for (let mac, entry in dump ?? {}) {
		mac = lc(mac);

		if (found[mac])
			continue;

		let rec = { mac, ip: null, hostname: null };

		if (type(entry) == 'string')
			rec.ip = entry;
		else if (type(entry) == 'object')
			rec.ip = entry.ip;

		found[mac] = rec;
	}

	let out = [];

	for (let mac, rec in found)
		push(out, rec);

	sort(out, (a, b) => (a.hostname ?? a.mac) < (b.hostname ?? b.mac) ? -1 : 1);

	state.clients = out;
}

function band_of(freq) {
	if (!freq)
		return null;
	if (freq < 2500)
		return '2.4G';
	if (freq < 5900)
		return '5G';

	return '6G';
}

function security_of(enc) {
	if (!enc || enc == 'none')
		return 'Open';
	if (index(enc, 'sae') >= 0)
		return 'WPA3';
	if (index(enc, 'psk2') >= 0)
		return 'WPA2';
	if (index(enc, 'psk') >= 0)
		return 'WPA';
	if (index(enc, 'wep') >= 0)
		return 'WEP';

	return enc;
}

function wifi_read() {
	let status = ubus_call('network.wireless', 'status') ?? {};
	let running = {};

	for (let radio, st in status)
		for (let iface in st?.interfaces ?? [])
			if (iface.section)
				running[iface.section] = {
					ifname: iface.ifname,
					up: !!st.up
				};

	let found = [];
	let cursor = uci.cursor();

	cursor.foreach('wireless', 'wifi-iface', function(s) {
		if (!s.ssid)
			return;
		if (s.mode && s.mode != 'ap')
			return;

		let now = running[s['.name']] ?? {};
		let up = !truish(s.disabled) && now.up && now.ifname;
		let clients = 0;
		let freq = null;

		if (up) {
			let cl = ubus_call(sprintf('hostapd.%s', now.ifname),
					   'get_clients')?.clients;

			clients = cl ? length(cl) : 0;

			let info = ubus_call(sprintf('hostapd.%s', now.ifname),
					     'get_status');

			freq = info?.freq;
		}

		push(found, {
			ssid: s.ssid,
			band: band_of(freq),
			security: security_of(s.encryption),
			up: !!up,
			clients
		});
	});

	state.networks = found;
}

function clock_read() {
	let now = localtime(time());

	state.clock = sprintf('%02d:%02d', now.hour, now.min);
}

/* ------------------------------------------------------------------ */
/* pages                                                               */

let pages = [];		/* { name, tile, enter?, update? } */
let dots = [];
let active = 0;
let screen, tileview;
let clock_label;	/* text_new() wrapper */
let last_touch;

const HEAD_TOP = 11;
const HEAD_X = 12;
const BODY_Y = 44;
const BODY_H = H - BODY_Y - 12;

function rate_fmt(bytes) {
	if (bytes >= 1000000)
		return sprintf('%s MB/s', bytes >= 100000000
				       ? sprintf('%d', int(bytes / 1000000))
				       : sprintf('%.1f', int(bytes / 100000.0) / 10.0));
	if (bytes >= 100)
		return sprintf('%s KB/s', bytes >= 100000
				       ? sprintf('%d', int(bytes / 1000))
				       : sprintf('%.1f', int(bytes / 100.0) / 10.0));

	return sprintf('%d B/s', bytes);
}

function total_fmt(bytes) {
	if (bytes >= 1000000000)
		return sprintf('%.2f GB', bytes / 1000000000.0);
	if (bytes >= 1000000)
		return sprintf('%.1f MB', bytes / 1000000.0);
	if (bytes >= 1000)
		return sprintf('%.1f KB', bytes / 1000.0);

	return sprintf('%d B', bytes);
}

function header_new(parent, title, count) {
	label_new(parent, F_TITLE, C_TXT, title).set({ x: HEAD_X, y: HEAD_TOP });

	if (count != null) {
		let t = label_new(parent, F_SMALL, C_RULE, count);

		t.set({ x: HEAD_X + 4 + text_w(F_TITLE, title),
			y: HEAD_TOP + 8 });
	}
}

function bar_scaled(value, full, height) {
	if (full <= 0)
		return 0;

	let scaled = int(value * 1000 / full);

	if (scaled > 1000)
		scaled = 1000;

	let floor = int((2 * 1000 + height - 1) / height);

	return scaled > floor ? scaled : floor;
}

/* ---- usage page ---------------------------------------------------- */

let usage = {};

function usage_build(parent) {
	header_new(parent, 'Usage', null);

	let card = box_new(parent, C_SURFACE, 12);

	card.set({ x: 10, y: BODY_Y, w: W - 20, h: BODY_H });

	usage.rx_label = text_new(card, F_MED, C_RX, '↓ 0 KB/s');
	usage.rx_label.obj.set({ x: 16, y: 14 });

	usage.tx_label = text_new(card, F_MED, C_TX, '↑ 0 KB/s');
	usage.tx_label.obj.set({ x: 16, y: 40 });

	usage.total = text_new(card, F_SMALL, C_DIM, '');
	usage.total.obj.set({ x: 16, y: 66, w: W - 52 });
	usage.total.obj.long_mode(lv.LABEL_LONG_DOTS);

	let chart = lv.chart(card);

	chart.set({ x: 10, y: 92, w: W - 40, h: BODY_H - 102 });
	chart.chart_type(lv.CHART_TYPE_BAR);
	chart.point_count(POINTS);
	chart.update_mode(lv.CHART_UPDATE_SHIFT);
	chart.div_lines(0, 0);
	chart.scrollbar(lv.SCROLLBAR_OFF);
	chart.clickable(false);
	chart.scrollable(false);
	chart.style({ bg_opa: lv.OPA_TRANSP, border_width: 0, pad_all: 0,
		      pad_column: 2 });
	chart.style({ width: 0, height: 0 }, lv.PART_INDICATOR);
	chart.chart_range(0, 1000);
	chart.series(C_RX);
	chart.series(C_TX);
	chart.style({ bg_color: C_RX, bg_opa: lv.OPA_COVER, radius: 1 },
		    lv.PART_ITEMS);

	usage.chart = chart;
	usage.bucket = 1;
}

function usage_update() {
	let peak = 1;

	for (let v in state.rx)
		if (v > peak)
			peak = v;
	for (let v in state.tx)
		if (v > peak)
			peak = v;

	let bucket = peak * 1.25;

	if (bucket != usage.bucket) {
		usage.bucket = bucket;
		usage.chart.points(0, map(state.rx,
			v => bar_scaled(v, bucket, BODY_H - 102)));
		usage.chart.points(1, map(state.tx,
			v => bar_scaled(v, bucket, BODY_H - 102)));
	} else {
		usage.chart.push(0, bar_scaled(state.rx_rate, bucket, BODY_H - 102));
		usage.chart.push(1, bar_scaled(state.tx_rate, bucket, BODY_H - 102));
	}

	text_set(usage.rx_label, sprintf('↓ %s', rate_fmt(state.rx_rate)));
	text_set(usage.tx_label, sprintf('↑ %s', rate_fmt(state.tx_rate)));
	text_set(usage.total, sprintf('Total ↓ %s   ↑ %s',
				      total_fmt(state.rx_total),
				      total_fmt(state.tx_total)));
}

/* ---- clients page -------------------------------------------------- */

let clients_pg = {};

function clients_build(parent) {
	header_new(parent, 'Clients', null);

	clients_pg.count = label_new(parent, F_SMALL, C_RULE, '');

	let card = lv.obj(parent);

	card.set({ x: 10, y: BODY_Y, w: W - 20, h: BODY_H });
	card.style({ bg_color: C_SURFACE, radius: 12, border_width: 0,
		     pad_all: 0, clip_corner: true });
	card.clickable(true);
	card.scrollable(true);
	card.scroll_dir(lv.DIR_VER);
	card.scrollbar(lv.SCROLLBAR_AUTO);
	card.on(lv.EVENT_PRESSED, touch_note);

	clients_pg.card = card;
	clients_pg.rows = [];
}

function clients_update() {
	let card = clients_pg.card;

	card.clean();
	clients_pg.rows = [];

	clients_pg.count.text(sprintf('%d', length(state.clients)));
	clients_pg.count.set({ x: HEAD_X + 4 + text_w(F_TITLE, 'Clients'),
			       y: HEAD_TOP + 8 });

	let y = 8;

	if (!length(state.clients)) {
		label_new(card, F_SMALL, C_DIM, 'no clients')
			.set({ x: 16, y });
		return;
	}

	for (let c in state.clients) {
		let name = c.hostname ?? c.mac;
		let sub = c.hostname ? c.mac : (c.ip ?? '');

		if (c.ip && c.hostname)
			sub = sprintf('%s  %s', c.ip, c.mac);
		else if (c.ip && !c.hostname)
			sub = c.ip;

		label_new(card, F_SMALL, C_TXT, name).set({ x: 16, y, w: W - 52 });

		if (sub)
			label_new(card, F_SMALL, C_DIM, sub)
				.set({ x: 16, y: y + 17, w: W - 52 });

		y += 40;

		box_new(card, C_RULE, 0).set({ x: 16, y: y - 3, w: W - 52, h: 1 });
	}
}

/* ---- wifi page ----------------------------------------------------- */

let wifi_pg = {};

function wifi_build(parent) {
	header_new(parent, 'WiFi', null);

	let card = lv.obj(parent);

	card.set({ x: 10, y: BODY_Y, w: W - 20, h: BODY_H });
	card.style({ bg_color: C_SURFACE, radius: 12, border_width: 0,
		     pad_all: 0, clip_corner: true });
	card.clickable(true);
	card.scrollable(true);
	card.scroll_dir(lv.DIR_VER);
	card.scrollbar(lv.SCROLLBAR_AUTO);
	card.on(lv.EVENT_PRESSED, touch_note);

	wifi_pg.card = card;
}

function wifi_update() {
	let card = wifi_pg.card;

	card.clean();

	let y = 8;
	let total = 0;

	if (!length(state.networks)) {
		label_new(card, F_SMALL, C_DIM, 'no wireless networks')
			.set({ x: 16, y });
		return;
	}

	for (let n in state.networks) {
		total += n.clients;

		box_new(card, n.up ? C_OK : C_DOWN, 4)
			.set({ x: 16, y: y + 4, w: 7, h: 7 });

		label_new(card, F_SMALL, C_TXT, n.ssid)
			.set({ x: 31, y, w: 160 });
		label_new(card, F_SMALL, C_DIM,
			  sprintf('%s  %s', n.band ?? '?', n.security))
			.set({ x: 31, y: y + 17 });

		let cnt = label_new(card, F_SMALL, n.clients ? C_TXT : C_DIM,
				    sprintf('%d', n.clients));

		cnt.set({ x: W - 70, y: y + 8, w: 30 });
		cnt.style({ text_align: lv.TEXT_ALIGN_RIGHT });

		y += 40;

		box_new(card, C_RULE, 0).set({ x: 16, y: y - 3, w: W - 52, h: 1 });
	}

	label_new(card, F_SMALL, C_DIM,
		  sprintf('total %d station%s', total, total == 1 ? '' : 's'))
		.set({ x: 16, y: y + 4 });
}

/* ------------------------------------------------------------------ */
/* frame                                                               */

function dots_refresh() {
	for (let i = 0; i < length(dots); i++)
		dots[i].style({ bg_color: i == active ? C_DIM : C_IDLE });
}

function page_show(i, anim) {
	if (i == active)
		return;

	active = i;
	tileview.tile_set(i, 0, anim);
	dots_refresh();

	let p = pages[i];

	p.update?.();
}

function tile_changed() {
	let now = tileview.tile_active()?.col ?? 0;

	touch_note();

	if (now == active)
		return;

	active = now;
	dots_refresh();

	let p = pages[active];

	p.update?.();
}

function touch_note() {
	last_touch = monotonic();

	if (blanked) {
		blanked = false;
		backlight_set(cfg.brightness);
		lv.touch_drop();
	}
}

let blanked = false;

function blank_check() {
	if (!cfg.blank || blanked)
		return;

	if (monotonic() - last_touch < cfg.blank * 60)
		return;

	blanked = true;
	backlight_set(0);
}

function dir_for(pos, count) {
	if (count < 2)
		return lv.DIR_NONE;
	if (pos == 0)
		return lv.DIR_RIGHT;
	if (pos == count - 1)
		return lv.DIR_LEFT;

	return lv.DIR_HOR;
}

function frame_build() {
	screen = lv.screen_create();

	screen.style({ bg_color: C_SCREEN, border_width: 0, pad_all: 0 });
	screen.scrollbar(lv.SCROLLBAR_OFF);
	screen.scrollable(false);
	screen.on(lv.EVENT_PRESSED, touch_note);

	let defs = [
		{ key: 'page_usage', name: 'usage',
		  build: usage_build, update: usage_update },
		{ key: 'page_clients', name: 'clients',
		  build: clients_build, update: clients_update },
		{ key: 'page_wifi', name: 'wifi',
		  build: wifi_build, update: wifi_update }
	];

	/* keep enabled pages only, so we know the count before tiling */
	let enabled = [];

	for (let d in defs) {
		if (cfg[d.key])
			push(enabled, d);
	}

	tileview = lv.tileview(screen);
	tileview.set({ x: 0, y: 0, w: W, h: H });
	tileview.style({ bg_opa: lv.OPA_TRANSP, border_width: 0, pad_all: 0 });
	tileview.scrollbar(lv.SCROLLBAR_OFF);
	tileview.on(lv.EVENT_VALUE_CHANGED, tile_changed);

	let count = length(enabled);

	for (let idx = 0; idx < count; idx++) {
		let d = enabled[idx];
		let tile = tileview.tile_add(idx, 0, dir_for(idx, count));

		tile.style({ bg_opa: lv.OPA_TRANSP, border_width: 0, pad_all: 0 });
		tile.scrollbar(lv.SCROLLBAR_OFF);
		tile.scrollable(false);
		tile.on(lv.EVENT_PRESSED, touch_note);

		d.build(tile);

		push(pages, { name: d.name, tile, update: d.update });
	}

	/* page dots */
	let left = int((W - ((count - 1) * 10 + 5)) / 2);

	for (let i = 0; i < count; i++) {
		let dot = box_new(screen, C_IDLE, 5);

		dot.set({ x: left + i * 10, y: H - 9, w: 5, h: 5 });
		push(dots, dot);
	}

	/* clock, top right */
	clock_label = text_new(screen, F_SMALL, C_DIM, '');

	clock_label.obj.set({ x: W - 52, y: HEAD_TOP + 6, w: 44 });
	clock_label.obj.style({ text_align: lv.TEXT_ALIGN_RIGHT });

	dots_refresh();
}

/* ------------------------------------------------------------------ */
/* main                                                                */

config_read();
backlight_find();
backlight_set(cfg.brightness);

if (!lv.init())
	die('cannot initialise LVGL');

if (!lv.display_drm(getenv('PANEL_DRM_DEVICE') ?? '/dev/dri/card0', -1))
	die('cannot open the DRM display');

let touch_device = getenv('PANEL_TOUCH_DEVICE');

if (touch_device)
	lv.indev_evdev(touch_device);

fonts_load();

let sysinfo = ubus_call('system', 'board');

state.hostname = sysinfo?.hostname ?? 'openwrt';

uloop.init();

frame_build();

if (!length(pages))
	die('no pages enabled in /etc/config/xwrt_panel');

/* first fill */
wan_device_read();
wan_read();
clients_read();
wifi_read();
clock_read();

pages[0].update?.();
text_set(clock_label, state.clock);

lv.refresh();

last_touch = monotonic();

uloop.interval(1000, function() {
	wan_read();
	clock_read();
	text_set(clock_label, state.clock);
	pages[active].update?.();
	lv.refresh();
});

uloop.interval(5000, function() {
	clients_read();
	wifi_read();

	if (pages[active].name != 'usage')
		pages[active].update?.();

	lv.refresh();
});

let rotate_tick = 0;

uloop.interval(1000, function() {
	blank_check();

	if (!cfg.rotate || blanked)
		return;

	if (++rotate_tick < cfg.rotate)
		return;

	rotate_tick = 0;
	page_show((active + 1) % length(pages), true);
	lv.refresh();
});

ubus.listener('network.interface', function() {
	wan_device_read();
});

uloop.run();
