'use strict';
'require view';
'require form';
'require ui';
'require rpc';
'require uci';

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

var callInitAction = rpc.declare({
	object: 'rc',
	method: 'init',
	params: [ 'name', 'action' ],
	expect: { result: false }
});

/* Brightness slider: range input synced with a number box, writing UCI
 * like an ordinary form.Value. */
var SliderValue = form.Value.extend({
	renderWidget: function(section_id, option_index, cfgvalue) {
		var value = (cfgvalue != null && cfgvalue !== '') ? +cfgvalue : 100;

		var range = E('input', {
			type: 'range', min: '10', max: '100', step: '1',
			value: String(value),
			style: 'width: 220px; vertical-align: middle',
			id: this.cbid(section_id),
			name: this.cbid(section_id),
			'data-widget': 'cbi-slider'
		});

		var num = E('input', {
			type: 'number', min: '10', max: '100',
			value: String(value),
			class: 'cbi-input-text',
			style: 'width: 5em; margin-left: 0.5em',
			readonly: 'readonly'
		});

		range.addEventListener('input', function() {
			num.value = range.value;
			range.dispatchEvent(new Event('change', { bubbles: true }));
		});

		return E('div', {}, [ range, num, ' %' ]);
	},

	remove: function() {}, /* brightness always has a sane value */
});

return view.extend({
	callInit: function(action) {
		return callInitAction('glinet-panel-ui', action).then(function(ok) {
			ui.addNotification(null,
				E('p', _('Service action "%s" %s').format(action,
					ok ? _('succeeded') : _('failed'))),
				ok ? 'info' : 'error');
			return ok;
		});
	},

	render: function(res) {
		var svc = res && res['glinet-panel-ui'];
		var running = !!(svc && svc.instances &&
			Object.keys(svc.instances).length);

		var m, s, o;

		m = new form.Map('glinet_panel',
			_('GL.iNet Panel Screen'),
			_('Settings for the TFT panel UI (glinet-panel-ui) of the '
			  + 'GL-BE10000 / GL-BE14000. Changes are applied to '
			  + '/etc/config/glinet_panel and the panel service is '
			  + 'restarted on "Save & Apply".'));

		/* ---- status & service control ---------------------------- */
		s = m.section(form.TypedSection, 'panel', _('Panel service'));
		s.anonymous = true;

		o = s.option(form.DummyValue, '_status', _('Service state'));
		o.rawhtml = true;
		o.cfgvalue = function() {
			return running
				? '<span style="color:#3a7">&bull; ' + _('running') + '</span>'
				: '<span style="color:#c33">&bull; ' + _('not running') + '</span>';
		};

		o = s.option(form.Button, '_restart', _('Control'));
		o.inputtitle = _('Restart panel');
		o.inputstyle = 'apply';
		o.onclick = L.bind(this.callInit, this, 'restart');

		o = s.option(form.Button, '_stop', '');
		o.inputtitle = _('Stop');
		o.inputstyle = 'remove';
		o.onclick = L.bind(this.callInit, this, 'stop');

		/* ---- appearance ----------------------------------------- */
		s = m.section(form.TypedSection, 'panel', _('Appearance'));
		s.anonymous = true;

		o = s.option(SliderValue, 'brightness', _('Brightness'),
			_('Panel backlight brightness, 10 to 100 percent.'));
		o.default = '100';
		o.rmempty = false;

		o = s.option(form.ListValue, 'background', _('Background'),
			_('Gradient background from '
			  + '/usr/share/glinet-panel-ui/backgrounds. '
			  + '"Black" disables the background image.'));
		o.value('0', _('Black (none)'));
		for (var i = 1; i <= 8; i++)
			o.value(String(i), _('Gradient %d').format(i));
		o.default = '7';
		o.rmempty = false;

		o = s.option(form.Value, 'list_opacity', _('Card opacity'),
			_('How solid the card behind a list is, 0-100. '
			  + 'Lower values let the background shine through.'));
		o.datatype = 'range(0,100)';
		o.default = '85';
		o.placeholder = '85';

		o = s.option(form.Flag, 'scroll', _('Follow finger'),
			_('Pages follow the finger while swiping instead of '
			  + 'changing in one step.'));
		o.default = '1';
		o.enabled = '1';
		o.disabled = '0';

		/* ---- behaviour ------------------------------------------ */
		s = m.section(form.TypedSection, 'panel', _('Behaviour'));
		s.anonymous = true;

		o = s.option(form.Value, 'auto_lock', _('Auto lock (min)'),
			_('Minutes without interaction before the panel goes '
			  + 'idle, 1 to 30.'));
		o.datatype = 'range(1,30)';
		o.default = '5';

		o = s.option(form.ListValue, 'idle_mode', _('When idle'),
			_('What happens when the panel goes idle: keep the '
			  + 'clock lit or turn the backlight off.'));
		o.value('on', _('Clock stays on'));
		o.value('blank', _('Blank (backlight off)'));
		o.default = 'on';
		o.rmempty = false;

		o = s.option(form.Value, 'pin', _('Unlock PIN'),
			_('Optional 6-digit PIN, asked for when leaving idle. '
			  + 'Leave empty for no keypad.'));
		o.datatype = 'and(uinteger, minlength(6), maxlength(6))';
		o.placeholder = _('not set');

		o = s.option(form.Flag, 'test_pages', _('Test pages'),
			_('Append the touch, font and colour test pages to the '
			  + 'page list.'));
		o.default = '0';
		o.enabled = '1';
		o.disabled = '0';

		/* ---- weather -------------------------------------------- */
		s = m.section(form.TypedSection, 'panel', _('Weather page'));
		s.anonymous = true;

		o = s.option(form.Value, 'latitude', _('Latitude'),
			_('Panel location in degrees. Both latitude and '
			  + 'longitude are required for the weather page.'));
		o.datatype = 'float';
		o.placeholder = '51.5072';

		o = s.option(form.Value, 'longitude', _('Longitude'));
		o.datatype = 'float';
		o.placeholder = '-0.1276';

		/* ---- pages ---------------------------------------------- */
		s = m.section(form.TypedSection, 'panel', _('Pages'));
		s.anonymous = true;

		o = s.option(form.DynamicList, 'pages', _('Page order'),
			_('Root pages in swipe order. Use the entries below to '
			  + 'add pages; drag-free ordering: remove and re-add to '
			  + 'reorder.'));
		var known = {
			traffic:     _('Traffic'),
			weather:     _('Weather'),
			wifi:        _('WiFi status'),
			wifitoggles: _('WiFi toggles'),
			radio:       _('Radio'),
			qrcodes:     _('QR codes'),
			clients:     _('Clients'),
			interfaces:  _('Interfaces'),
			ports:       _('Ports'),
			system:      _('System'),
			brightness:  _('Brightness'),
			reboot:      _('Reboot')
		};
		for (var k in known)
			o.value(k, known[k]);

		return m.render();
	},

	load: function() {
		return L.resolveDefault(callServiceList('glinet-panel-ui'), {});
	}
});
