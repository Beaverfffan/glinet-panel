/*
 * luci-app-xwrt-doom —— 控制 xwrt-doom（机身小屏上的触屏 Doom）
 *
 * 页面做三件事：
 *   1. 显示运行状态（读 /var/run/xwrt-doom.status，init 脚本写的 key=value）
 *   2. 起停 / 取 IWAD / 调参（uci xwrt_doom + /etc/init.d/xwrt-doom）
 *   3. **把玩法与排障说明展示出来** —— 直接读设备上的
 *      /usr/share/xwrt-doom/README，所以只有一份权威文档，改文档不用改这个页面。
 */
'use strict';
'require view';
'require form';
'require fs';
'require uci';
'require ui';
'require poll';

var STATUS = '/var/run/xwrt-doom.status';
var README = '/usr/share/xwrt-doom/README';
var GETWAD = '/usr/bin/xwrt-doom-get-wad';
var DEFAULT_WAD = '/usr/share/xwrt-doom/doom1.wad';

function parseKV(text) {
	var o = {};

	String(text || '').split('\n').forEach(function (l) {
		var i = l.indexOf('=');
		if (i > 0)
			o[l.slice(0, i)] = l.slice(i + 1);
	});
	return o;
}

function fmtBytes(n) {
	n = parseInt(n, 10);
	if (!isFinite(n) || n <= 0)
		return '-';
	if (n < 1024)
		return n + ' B';
	if (n < 1048576)
		return (n / 1024).toFixed(1) + ' KiB';
	return (n / 1048576).toFixed(1) + ' MiB';
}

return view.extend({
	load: function () {
		return Promise.all([
			uci.load('xwrt_doom'),
			fs.read(STATUS).catch(function () { return ''; }),
			fs.read(README).catch(function () { return ''; })
		]);
	},

	/* ---------- 状态卡片 ---------- */
	renderStatus: function () {
		var self = this;

		this.statusNode = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('运行状态')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('项')),
					E('th', { 'class': 'th' }, _('值'))
				]),
				E('tbody', { 'id': 'xwrt-doom-status-body' })
			])
		]);

		this.refreshStatus = function () {
			return fs.read(STATUS).then(function (txt) {
				var st = parseKV(txt);
				var body = document.getElementById('xwrt-doom-status-body');

				if (!body)
					return;

				var rows = [
					[_('运行中'), st.running === '1'
						? _('是 (pid %s)').format(st.pid) : _('否')],
					[_('开机自启'), st.enabled === '1' ? _('已打开') : _('关闭')],
					[_('IWAD'), '%s  (%s)'.format(st.wad || '-',
						fmtBytes(st.wad_bytes))],
					[_('帧率上限'), st.fps === '0'
						? _('不限制（实测约 36 fps，受 SPI 带宽限制）')
						: _('%s fps').format(st.fps)],
					[_('画面版式'), st.view === 'stretch'
						? _('stretch —— 纵向 1.2 倍铺满 4:3')
						: _('reserve —— 底部 40 行留给控制条')],
					[_('触摸设备'), st.touch_dev || '-'],
					[_('手感'), _('死区 %s px / 半径 %s px / 轻点 %s ms / 脉冲 %s ms')
						.format(st.deadzone, st.radius, st.tap_ms, st.pulse_ms)],
					[_('DRM 占用'), _('%s 个客户端（含本机 doom 与面板服务）')
						.format(st.drm_clients)],
					[_('错误'), st.error ? st.error : '-']
				];

				body.replaceChildren.apply(body, rows.map(function (r) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td', 'style': 'width:30%' }, r[0]),
						E('td', { 'class': 'td' }, r[1])
					]);
				}));
			}).catch(function () { });
		};

		this.refreshStatus();
		poll.add(L.bind(function () {
			return fs.read(STATUS).then(function () { self.refreshStatus(); });
		}, this), 5);

		return this.statusNode;
	},

	/* ---------- 控制按钮 ---------- */
	renderControls: function () {
		var self = this;
		var busy = false;

		function notify(msg, kind) {
			ui.addNotification(null, E('p', {}, msg), kind);
		}

		function act(cmd) {
			if (busy)
				return Promise.resolve();
			busy = true;
			return fs.exec('/etc/init.d/xwrt-doom', [cmd]).then(function (r) {
				if (r.code) {
					var err = (r.stderr || '').trim() || (r.stdout || '').trim();
					notify(_('“%s”失败（rc=%d）：%s').format(cmd, r.code, err || '?'),
						'error');
				}
				else {
					notify(_('已执行 %s').format(cmd));
				}
			}).catch(function (e) {
				notify(_('执行失败：%s').format(e), 'error');
			}).then(function () {
				busy = false;
				return new Promise(function (res) { setTimeout(res, 1500); });
			}).then(function () {
				return self.refreshStatus();
			});
		}

		function getWad() {
			if (busy)
				return Promise.resolve();
			busy = true;
			notify(_('正在下载 IWAD 到 %s ……（约 4 MB，会校验大小与 md5）')
				.format(DEFAULT_WAD));
			return fs.exec(GETWAD, []).then(function (r) {
				var out = ((r.stdout || '') + (r.stderr || '')).trim();
				if (r.code)
					notify(_('下载失败：%s').format(out || ('rc=' + r.code)), 'error');
				else
					notify(_('IWAD 就绪：%s').format(out.split('\n').pop()));
			}).catch(function (e) {
				notify(_('下载失败：%s').format(e), 'error');
			}).then(function () {
				busy = false;
				return new Promise(function (res) { setTimeout(res, 1500); });
			}).then(function () {
				return self.refreshStatus();
			});
		}

		function btn(label, cls, fn) {
			return E('button', {
				'class': 'btn cbi-button ' + cls,
				'click': ui.createHandlerFn(self, fn)
			}, label);
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('控制')),
			E('p', { 'class': 'cbi-section-descr' },
				_('启动后整块屏（含底部控制条）都归 Doom，面板服务的页面会看不见 —— 所以启动前会先停掉面板服务与媒体播放。要退出 Doom：点控制条的 MNU 翻到第 2 页，点最右边红色的 EXIT。')),
			E('div', { 'class': 'cbi-value' }, [
				btn(_('启动 Doom'), 'cbi-button-apply', function () {
					return act('start');
				}),
				' ',
				btn(_('停止'), 'cbi-button-reset', function () {
					return act('stop');
				}),
				' ',
				btn(_('重启'), 'cbi-button-neutral', function () {
					return act('restart');
				}),
				' ',
				btn(_('下载 shareware IWAD'), 'cbi-button-action', function () {
					return getWad();
				}),
				' ',
				btn(_('刷新状态'), 'cbi-button-neutral', function () {
					return self.refreshStatus();
				})
			])
		]);
	},

	/* ---------- 玩法与排障文档（直接读设备上的 README）---------- */
	renderDoc: function (doc) {
		var body = doc
			? E('pre', {
				'style': 'white-space:pre-wrap;word-break:break-word;'
					+ 'background:rgba(127,127,127,.08);padding:1em;'
					+ 'border-radius:4px;max-height:32em;overflow:auto;'
					+ 'font-size:12px;line-height:1.5'
			}, doc)
			: E('p', { 'class': 'cbi-section-descr' },
				_('读不到 %s —— 请确认 xwrt-doom 包已安装。').format(README));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('玩法与排障')),
			body
		]);
	},

	render: function (data) {
		var doc = data[2] || '';
		var self = this;

		var m = new form.Map('xwrt_doom', _('屏幕上的 Doom'),
			_('在机身小屏（GL-BE14000 的 320x240 MIPI-DBI 屏）上玩触屏 Doom。引擎是 doomgeneric，输出走 DRM 双缓冲，实测 36 fps。'));

		var s = m.section(form.NamedSection, 'main', 'doom', _('设置'));
		s.anonymous = true;

		var o = s.option(form.Flag, 'enabled', _('开机自动启动'),
			_('打开后写进 /etc/config/xwrt_doom。注意启动后整块屏都归 Doom，面板服务在退出前一直看不见。'));
		o.rmempty = false;

		o = s.option(form.Value, 'wad', _('IWAD 路径'),
			_('Doom 的关卡/贴图数据文件。可以点上面的“下载 shareware IWAD”，或自己放一份 doom1.wad / freedoom1.wad 进去。'));
		o.placeholder = DEFAULT_WAD;
		o.rmempty = false;

		o = s.option(form.Value, 'fps', _('帧率上限'),
			_('0 = 不设上限。引擎每帧只烧约 1 ms CPU，真正定速的是 SPI 带宽（约 36 fps）。'));
		o.datatype = 'uinteger';
		o.rmempty = false;

		o = s.option(form.ListValue, 'view', _('画面版式'));
		o.value('reserve', _('reserve —— 底部 40 行固定给控制条'));
		o.value('stretch', _('stretch —— 纵向 1.2 倍铺满 4:3，控制条半透明'));
		o.rmempty = false;

		o = s.option(form.Value, 'touch_dev', _('触摸设备'),
			_('auto = 按“有没有 ABS_X/ABS_Y”自动找，不看设备名。'));
		o.placeholder = 'auto';

		o = s.option(form.Value, 'deadzone', _('摇杆死区 (px)'),
			_('越小越灵敏，但手指微抖会误触。'));
		o.datatype = 'uinteger';

		o = s.option(form.Value, 'radius', _('摇杆半径 (px)'),
			_('推到底所需的距离。'));
		o.datatype = 'uinteger';

		o = s.option(form.Value, 'tap_ms', _('轻点阈值 (ms)'),
			_('按住不超过这么久、且几乎没移动，就算“轻点画面 = 开一枪”。'));
		o.datatype = 'uinteger';

		o = s.option(form.Value, 'pulse_ms', _('动作最短按住 (ms)'),
			_('Doom 读 gamekeydown[]，按下与释放落在同一个 35 Hz tick 内等于没按过，所以别调到 40 以下。'));
		o.datatype = 'uinteger';

		o = s.option(form.Flag, 'swap_xy', _('交换触摸 X/Y'),
			_('GL-BE14000 上不该开：坐标映射已按 DTS 推导并实测确认。换了别的机型再考虑。'));
		o.rmempty = false;

		o = s.option(form.Flag, 'invert_x', _('X 轴反相'));
		o.rmempty = false;

		o = s.option(form.Flag, 'invert_y', _('Y 轴反相'));
		o.rmempty = false;

		o = s.option(form.Value, 'brightness', _('运行期间亮度 (%)'),
			_('0 = 不动背光。'));
		o.datatype = 'range(0,100)';

		o = s.option(form.Flag, 'stats', _('左上角叠帧率'),
			_('调参时用，平时关掉（会盖住一点画面）。'));
		o.rmempty = false;

		o = s.option(form.Flag, 'stop_panel', _('启动时停掉面板/媒体服务'),
			_('必须停：三者抢同一块 DRM plane，谁没拿到 master 就只是安静地白跑。'));
		o.rmempty = false;

		o = s.option(form.Flag, 'restore_panel', _('退出后恢复面板服务'));
		o.rmempty = false;

		o = s.option(form.Value, 'extra_args', _('额外引擎参数'),
			_('原样透传。例如“-warp 1 1”直接进 E1M1，“-timedemo demo1”跑 demo 测速。'));

		return m.render().then(function (node) {
			return E('div', {}, [
				node,
				self.renderControls(),
				self.renderStatus(),
				self.renderDoc(doc)
			]);
		});
	},

	handleSaveApply: function (ev, mode) {
		return this.handleSave(ev).then(L.bind(function () {
			return fs.exec('/etc/init.d/xwrt-doom', ['reload']).catch(function () { });
		}, this)).then(function () {
			return ui.changes.apply(mode == '0');
		});
	}
});
