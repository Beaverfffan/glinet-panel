/*
 * luci-app-xwrt-media —— 控制 xwrt-media（.xwpm 媒体包播放）
 *
 * 页面做三件事：
 *   1. 显示播放状态与当前媒体包的容器信息（读 /var/run/xwrt-media.status）
 *   2. 提供播放/停止与设置（走 uci xwrt_media + /etc/init.d/xwrt-media）
 *   3. **把媒体格式说明展示出来** —— 直接读设备上的
 *      /usr/share/xwrt-media/FORMAT.md，所以只有一份权威文档，
 *      改文档不用改这个页面。
 */
'use strict';
'require view';
'require form';
'require fs';
'require uci';
'require ui';
'require poll';

var STATUS = '/var/run/xwrt-media.status';
var FORMAT = '/usr/share/xwrt-media/FORMAT.md';
var MEDIA_DIRS = ['/usr/share/xwrt-media', '/root/media'];

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
			uci.load('xwrt_media'),
			fs.read(STATUS).catch(function () { return ''; }),
			fs.read(FORMAT).catch(function () { return ''; }),
			fs.list(MEDIA_DIRS[0]).catch(function () { return []; }),
			fs.list(MEDIA_DIRS[1]).catch(function () { return []; })
		]);
	},

	/* ---------- 状态卡片 ---------- */
	renderStatus: function () {
		var self = this;

		this.statusNode = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('播放状态')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('项')),
					E('th', { 'class': 'th' }, _('值'))
				]),
				E('tbody', { 'id': 'xwrt-media-status-body' })
			])
		]);

		this.refreshStatus = function () {
			return fs.read(STATUS).then(function (txt) {
				var st = parseKV(txt);
				var body = document.getElementById('xwrt-media-status-body');
				if (!body)
					return;
				var rows = [
					[_('运行中'), st.running === '1' ? _('是 (pid %s)').format(st.pid) : _('否')],
					[_('媒体文件'), st.file || '-'],
					[_('容器'), st.format === 'xwpm'
						? '%sx%s  bpp=%s  fps=%s  %s 帧 (%s 秒)'
							.format(st.width, st.height, st.bpp, st.fps, st.frames,
								st.duration || '?')
						: '-'],
					[_('单帧 / 整包'), st.frame_bytes
						? '%s / %s'.format(fmtBytes(st.frame_bytes), fmtBytes(st.stream_bytes))
						: '-'],
					[_('调色板'), st.palette || '-'],
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

	/* ---------- 媒体文件列表 ---------- */
	renderFilePicker: function (files) {
		var self = this;

		if (!files.length)
			return E('div', { 'class': 'cbi-section' }, [
				E('p', { 'class': 'cbi-section-descr' },
					_('在 %s 或 %s 下没有找到 .xwpm / .xwpm.gz。把转好的包放进这两个目录之一，或者直接填绝对路径。')
						.format(MEDIA_DIRS[0], MEDIA_DIRS[1]))
			]);

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('设备上的媒体包')),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('文件')),
					E('th', { 'class': 'th' }, _('大小')),
					E('th', { 'class': 'th' }, '')
				]),
				E('tbody', {}, files.map(function (f) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, f.path),
						E('td', { 'class': 'td' }, fmtBytes(f.size)),
						E('td', { 'class': 'td' }, [
							E('button', {
								'class': 'btn cbi-button cbi-button-action',
								'click': ui.createHandlerFn(self, function () {
									uci.set('xwrt_media', 'main', 'file', f.path);
									uci.set('xwrt_media', 'main', 'gzip',
										/\.gz$/.test(f.path) ? '1' : '0');
									return uci.save().then(function () {
										ui.addNotification(null,
											E('p', {}, _('已把媒体文件设为 %s（记得保存并应用）').format(f.path)));
										return self.refreshStatus();
									});
								})
							}, _('用它'))
						])
					]);
				}))
			])
		]);
	},

	/* ---------- 媒体格式说明（直接读设备上的 FORMAT.md）---------- */
	renderFormatDoc: function (doc) {
		var body = doc
			? E('pre', {
				'style': 'white-space:pre-wrap;word-break:break-word;'
					+ 'background:rgba(127,127,127,.08);padding:1em;'
					+ 'border-radius:4px;max-height:32em;overflow:auto;'
					+ 'font-size:12px;line-height:1.5'
			}, doc)
			: E('p', { 'class': 'cbi-section-descr' },
				_('读不到 %s —— 请确认 xwrt-media 包已安装。').format(FORMAT));

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('媒体格式说明（如何替换成自己的图片或视频）')),
			E('p', { 'class': 'cbi-section-descr' },
				_('下面的内容就是设备上的 %s。要点：面板是 320x240，用 tools/xwpm-encode.py 把视频或图片转成 .xwpm 即可；容器格式本身很简单（24 字节头 + 调色板 + 顺序帧），细节都在这里。')
					.format(FORMAT)),
			body
		]);
	},

	/* ---------- 控制按钮 ---------- */
	renderControls: function () {
		var self = this;

		function act(cmd) {
			return fs.exec('/etc/init.d/xwrt-media', [cmd]).then(function (r) {
				if (r.code)
					ui.addNotification(null, E('p', {},
						_('“%s”失败（rc=%d）：%s').format(cmd, r.code,
							(r.stderr || '').trim() || '?')));
				else
					ui.addNotification(null, E('p', {}, _('已执行 %s').format(cmd)));
				return self.refreshStatus();
			}).catch(function (e) {
				ui.addNotification(null, E('p', {}, _('执行失败：%s').format(e)), 'error');
			});
		}

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('控制')),
			E('p', { 'class': 'cbi-section-descr' },
				_('“播放”会先停掉面板服务（两者抢同一块 DRM plane），结束后自动恢复。也可以用命令行：xwpmplay <文件> [fps]。')),
			E('div', { 'class': 'cbi-value' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': ui.createHandlerFn(self, function () { return act('start'); })
				}, _('播放')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-reset',
					'click': ui.createHandlerFn(self, function () { return act('stop'); })
				}, _('停止')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-neutral',
					'click': ui.createHandlerFn(self, function () { return act('restart'); })
				}, _('重启播放')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-neutral',
					'click': ui.createHandlerFn(self, function () { return self.refreshStatus(); })
				}, _('刷新状态'))
			])
		]);
	},

	render: function (data) {
		var doc = data[2] || '';
		var files = [];

		MEDIA_DIRS.forEach(function (d, i) {
			var list = data[3 + i] || [];
			list.forEach(function (e) {
				if (e.type !== 'directory' && /\.xwpm(\.gz)?$/.test(e.name))
					files.push({ path: d + '/' + e.name, size: e.size });
			});
		});
		files.sort(function (a, b) { return a.path < b.path ? -1 : 1; });

		var m = new form.Map('xwrt_media',
			_('媒体播放'),
			_('在机身小屏（GL-BE14000 的 320x240 MIPI-DBI 屏）上播放 .xwpm 媒体包。'));

		var s = m.section(form.NamedSection, 'main', 'media', _('设置'));
		s.anonymous = true;

		var o = s.option(form.Flag, 'enabled', _('开机自动播放'),
			_('打开后写进 /etc/config/xwrt_media，服务会随开机启动。'));
		o.rmempty = false;

		o = s.option(form.Value, 'file', _('媒体文件'),
			_('绝对路径。可以是 .xwpm，也可以是 gzip 过的 .xwpm.gz。'));
		o.placeholder = MEDIA_DIRS[0] + '/badapple.xwpm.gz';
		o.rmempty = false;

		o = s.option(form.Flag, 'gzip', _('文件是 gzip 压缩的'),
			_('打开后用 gunzip 流式喂给播放器，不落盘。'));
		o.rmempty = false;

		o = s.option(form.Value, 'fps', _('帧率覆盖'),
			_('0 = 用容器里写着的帧率。'));
		o.datatype = 'uinteger';
		o.rmempty = false;

		o = s.option(form.Flag, 'loop', _('循环播放'));
		o.rmempty = false;

		o = s.option(form.Value, 'brightness', _('播放时亮度 (%)'),
			_('0 = 不动背光。'));
		o.datatype = 'range(0,100)';

		o = s.option(form.Flag, 'stop_panel', _('播放时停掉面板服务'),
			_('必须停：播放器与面板服务抢同一块 DRM plane。'));
		o.rmempty = false;

		o = s.option(form.Flag, 'restore_panel', _('结束后恢复面板服务'));
		o.rmempty = false;

		var self = this;

		return m.render().then(function (node) {
			return E('div', {}, [
				node,
				self.renderControls(),
				self.renderStatus(),
				self.renderFilePicker(files),
				self.renderFormatDoc(doc)
			]);
		});
	},

	handleSaveApply: function (ev, mode) {
		return this.handleSave(ev).then(L.bind(function () {
			return fs.exec('/etc/init.d/xwrt-media', ['reload']).catch(function () { });
		}, this)).then(function () {
			return ui.changes.apply(mode == '0');
		});
	}
});
