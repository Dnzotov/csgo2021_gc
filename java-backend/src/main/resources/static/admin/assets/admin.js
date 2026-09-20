(function () {
  'use strict';

  var csrf = document.querySelector('meta[name="csrf-token"]').content;
  var categories = [];
  var serverTimeOffset = 0;      // server clock - browser clock, so "12 s ago" is right even with a skewed PC clock
  var lastSearches = [];
  var editingServerId = null;
  var serversCache = [];         // the registry, for the map suggestions of the fake players form
  var editingFakeId = null;

  // ---------------------------------------------------------------- helpers
  function el(tag, attrs) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === 'class') node.className = attrs[k];
        else if (k === 'text') node.textContent = attrs[k];
        else if (k === 'title') node.title = attrs[k];
        else if (k.slice(0, 2) === 'on') node.addEventListener(k.slice(2), attrs[k]);
        else node.setAttribute(k, attrs[k]);
      });
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c == null) continue;
      node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return node;
  }

  function api(method, url, body) {
    var headers = { 'Accept': 'application/json', 'X-CSRF-TOKEN': csrf };
    var opts = { method: method, headers: headers, credentials: 'same-origin' };
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(url, opts).then(function (r) {
      if (r.status === 401) { window.location = '/admin/login'; throw new Error('signed out'); }
      if (r.status === 204) return null;
      return r.json().catch(function () { return {}; }).then(function (data) {
        if (!r.ok) throw new Error(data.message || ('HTTP ' + r.status));
        return data;
      });
    });
  }

  var bannerTimer;
  function showError(message) {
    var b = document.getElementById('banner');
    b.textContent = message;
    b.hidden = false;
    clearTimeout(bannerTimer);
    bannerTimer = setTimeout(function () { b.hidden = true; }, 6000);
  }
  function fail(e) { if (e && e.message !== 'signed out') showError(e.message); }

  function nowMs() { return Date.now() + serverTimeOffset; }

  function formatDuration(seconds) {
    seconds = Math.max(0, Math.floor(seconds));
    if (seconds < 60) return seconds + ' s';
    var m = Math.floor(seconds / 60), s = seconds % 60;
    if (m < 60) return m + ' min ' + (s < 10 ? '0' : '') + s + ' s';
    return Math.floor(m / 60) + ' h ' + (m % 60) + ' min';
  }
  function formatAgo(seconds) {
    return seconds < 1 ? 'just now' : formatDuration(seconds) + ' ago';
  }
  function formatDate(iso) { return new Date(iso).toLocaleString(); }

  function fillCategorySelect(select, selected) {
    select.textContent = '';
    categories.forEach(function (c) {
      var o = el('option', { value: c.key, text: c.label });
      if (c.key === selected) o.selected = true;
      select.appendChild(o);
    });
  }

  // ---------------------------------------------------------------- tabs
  document.querySelectorAll('.tab').forEach(function (tab) {
    tab.addEventListener('click', function () {
      document.querySelectorAll('.tab').forEach(function (t) { t.classList.toggle('active', t === tab); });
      ['searches', 'servers', 'matches', 'fake'].forEach(function (name) {
        document.getElementById('tab-' + name).hidden = tab.dataset.tab !== name;
      });
      if (tab.dataset.tab === 'servers') loadServers();
      if (tab.dataset.tab === 'matches') loadMatches();
      if (tab.dataset.tab === 'fake') { loadServers().then(renderChips); loadFake(); }
    });
  });

  // ---------------------------------------------------------------- active searches
  var searchBody = document.querySelector('#searchTable tbody');

  function renderSearches() {
    searchBody.textContent = '';
    var liveCount = 0;
    lastSearches.forEach(function (s) {
      var searching = live(s.status);
      if (searching) liveCount++;
      var started = Date.parse(s.started_at);
      var elapsed = searching ? (nowMs() - started) / 1000 : s.duration_seconds;

      var modeCell = el('td', null, s.mode_label);
      if (s.accept_required) modeCell.appendChild(el('span', { class: 'tag', text: 'Accept' }));
      if (s.source === 'admin') modeCell.appendChild(el('span', { class: 'tag', text: 'TEST' }));

      var accountCell = el('td', { class: 'num', title: 'SteamID64 ' + s.steam_id64 }, String(s.account_id));
      var actions = el('td', { class: 'actions' });
      if (searching) {
        actions.appendChild(el('button', {
          class: 'small danger', text: 'End', title: 'Remove this search from the queue',
          onclick: function () { endSearch(s); }
        }));
      }

      var where = '—';
      if (s.assignment) {
        where = s.assignment.server_address + ':' + s.assignment.server_port + '  ' + (s.assignment.map || '') + '  (' + s.assignment.match_id + ')';
      } else if (s.match) {
        where = s.match.players + '/' + s.match.required_players + (s.match.fake_players ? ' (' + s.match.fake_players + ' fake)' : '')
          + ' on ' + (s.match.map || '?') + '  (' + s.match.match_id + ')';
      }

      searchBody.appendChild(el('tr', { class: searching ? '' : 'ended' },
        accountCell,
        modeCell,
        el('td', { class: 'num', title: 'eGame ' + s.e_game + ', map mask ' + (s.game_type >>> 8) }, String(s.game_type)),
        el('td', null, s.game_mode),
        mapsCell(s),
        el('td', { title: formatDate(s.started_at) }, formatAgo((nowMs() - started) / 1000)),
        el('td', { class: 'num' }, formatDuration(elapsed)),
        el('td', { class: 'small' }, where),
        el('td', null, el('span', { class: 'pill ' + s.status.toLowerCase(), text: statusLabel(s.status) })),
        actions));
    });
    document.getElementById('searchCount').textContent = String(liveCount);
    document.getElementById('searchEmpty').hidden = lastSearches.length > 0;
    document.getElementById('searchTable').hidden = lastSearches.length === 0;
  }

  // Skirmish carries the selected modes (each with the maps of its group and the server category that serves it)
  function mapsCell(s) {
    if (s.variants && s.variants.length) {
      var cell = el('td', { class: 'maps' });
      s.variants.forEach(function (v, i) {
        cell.appendChild(el('span', { title: v.category + ': ' + (v.maps.length ? v.maps.join(', ') : 'any map') },
          (i ? ', ' : '') + v.name + ' [' + v.category + ']'));
      });
      return cell;
    }
    return el('td', { class: 'maps' }, s.maps && s.maps.length ? s.maps.join(', ') : '—');
  }

  function live(status) {
    return status === 'SEARCHING' || status === 'MATCHED' || status === 'WAITING_ACCEPT' || status === 'READY_TO_CONNECT';
  }

  function statusLabel(status) {
    return { SEARCHING: 'Searching', MATCHED: 'Matched', WAITING_ACCEPT: 'Waiting accept', READY_TO_CONNECT: 'Ready to connect',
      CANCELLED: 'Cancelled', EXPIRED: 'Timed out', REMOVED: 'Removed', COMPLETED: 'Completed' }[status] || status;
  }

  function loadSearches() {
    var finished = document.getElementById('showFinished').checked;
    return api('GET', '/api/v1/matchmaking/searches?include_finished=' + finished).then(function (data) {
      serverTimeOffset = Date.parse(data.server_time) - Date.now();
      lastSearches = data.searches;
      document.getElementById('searchInfo').textContent =
        'searches not refreshed for ' + formatDuration(data.stale_search_timeout_seconds) + ' expire';
      renderSearches();
    }).catch(fail);
  }

  function endSearch(s) {
    if (!confirm('End the search of AccountID ' + s.account_id + '?')) return;
    api('DELETE', '/admin/api/searches/' + s.id).then(loadSearches).catch(fail);
  }

  document.getElementById('showFinished').addEventListener('change', loadSearches);

  document.getElementById('testSearchForm').addEventListener('submit', function (ev) {
    ev.preventDefault();
    var f = ev.target;
    api('POST', '/admin/api/searches', {
      account_id: Number(f.account_id.value),
      mode: f.mode.value,
      map: f.map.value.trim()
    }).then(function () { f.reset(); return loadSearches(); }).catch(fail);
  });

  setInterval(loadSearches, 2000);
  setInterval(renderSearches, 1000);   // keeps the durations ticking between polls

  // ---------------------------------------------------------------- game servers
  var serverBody = document.querySelector('#serverTable tbody');

  function loadServers() {
    return api('GET', '/admin/api/servers').then(renderServers).catch(fail);
  }

  function renderServers(servers) {
    serversCache = servers;
    serverBody.textContent = '';
    servers.forEach(function (s) {
      serverBody.appendChild(s.id === editingServerId ? editRow(s) : viewRow(s));
    });
    document.getElementById('serverCount').textContent = String(servers.length);
    document.getElementById('serverEmpty').hidden = servers.length > 0;
    document.getElementById('serverTable').hidden = servers.length === 0;
  }

  function viewRow(s) {
    return el('tr', null,
      el('td', null, s.host),
      el('td', { class: 'num' }, String(s.port)),
      el('td', null, s.category_label, s.accept_required ? el('span', { class: 'tag', text: 'Accept' }) : null),
      el('td', null, s.map || '—'),
      el('td', { title: s.reserved_match_id ? 'match ' + s.reserved_match_id : '' },
        el('span', { class: 'pill ' + s.state.toLowerCase(), text: stateLabel(s.state) }),
        s.reserved_match_id ? el('span', { class: 'tag', text: s.reserved_match_id }) : null),
      el('td', null, el('span', { class: 'pill ' + (s.enabled ? 'enabled' : 'disabled'), text: s.enabled ? 'Enabled' : 'Disabled' })),
      el('td', { class: 'small' }, s.last_heartbeat_at ? formatDate(s.last_heartbeat_at) : 'never'),
      el('td', { title: formatDate(s.created_at) }, formatDate(s.created_at)),
      el('td', { class: 'actions' },
        stateButton(s),
        el('button', { class: 'small', text: s.enabled ? 'Disable' : 'Enable', onclick: function () { toggleServer(s); } }),
        el('button', { class: 'small', text: 'Edit', onclick: function () { editingServerId = s.id; loadServers(); } }),
        el('button', { class: 'small danger', text: 'Delete', onclick: function () { deleteServer(s); } })));
  }

  function editRow(s) {
    var host = el('input', { value: s.host, maxlength: '253' });
    var port = el('input', { type: 'number', min: '1', max: '65535', value: String(s.port), class: 'narrow' });
    var category = el('select');
    fillCategorySelect(category, s.category);
    var map = el('input', { value: s.map || '', pattern: '[A-Za-z0-9_]{0,64}' });
    var enabled = el('input', { type: 'checkbox' });
    enabled.checked = s.enabled;
    return el('tr', null,
      el('td', null, host), el('td', null, port), el('td', null, category), el('td', null, map),
      el('td', null, el('span', { class: 'pill ' + s.state.toLowerCase(), text: stateLabel(s.state) })),
      el('td', null, enabled), el('td', { class: 'small' }, s.last_heartbeat_at ? formatDate(s.last_heartbeat_at) : 'never'),
      el('td', { text: formatDate(s.created_at) }),
      el('td', { class: 'actions' },
        el('button', {
          class: 'small primary', text: 'Save',
          onclick: function () {
            api('PUT', '/admin/api/servers/' + s.id, {
              host: host.value.trim(), port: Number(port.value), category: category.value,
              map: map.value.trim(), enabled: enabled.checked
            }).then(function () { editingServerId = null; return loadServers(); }).catch(fail);
          }
        }),
        el('button', { class: 'small', text: 'Cancel', onclick: function () { editingServerId = null; loadServers(); } })));
  }

  function stateLabel(state) {
    return { AVAILABLE: 'Available', RESERVED: 'Reserved', BUSY: 'Busy' }[state] || state;
  }

  // Available <-> Busy; a Reserved server can be released (its match ends, forming players go back to searching)
  function stateButton(s) {
    var label = s.state === 'RESERVED' ? 'Release' : s.state === 'BUSY' ? 'Set available' : 'Set busy';
    var target = s.state === 'AVAILABLE' ? 'BUSY' : 'AVAILABLE';
    return el('button', { class: 'small', text: label, onclick: function () {
      if (s.state === 'RESERVED' && !confirm('Release ' + s.host + ':' + s.port + '? The match on it ends.')) return;
      api('POST', '/admin/api/servers/' + s.id + '/state', { state: target }).then(loadServers).catch(fail);
    } });
  }

  function toggleServer(s) {
    api('POST', '/admin/api/servers/' + s.id + '/enabled', { enabled: !s.enabled }).then(loadServers).catch(fail);
  }

  function deleteServer(s) {
    if (!confirm('Delete game server ' + s.host + ':' + s.port + '?')) return;
    api('DELETE', '/admin/api/servers/' + s.id).then(loadServers).catch(fail);
  }

  document.getElementById('serverForm').addEventListener('submit', function (ev) {
    ev.preventDefault();
    var f = ev.target;
    api('POST', '/admin/api/servers', {
      host: f.host.value.trim(), port: Number(f.port.value), category: f.category.value,
      map: f.map.value.trim(), enabled: f.enabled.checked, state: f.state.value
    }).then(function () { f.reset(); return loadServers(); }).catch(fail);
  });

  // ---------------------------------------------------------------- matches
  var matchBody = document.querySelector('#matchTable tbody');

  function loadMatches() {
    return api('GET', '/admin/api/matches').then(function (matches) {
      matchBody.textContent = '';
      matches.forEach(function (m) {
        matchBody.appendChild(el('tr', null,
          el('td', { class: 'num' }, m.id),
          el('td', null, m.mode_label, m.accept_required ? el('span', { class: 'tag', text: 'Accept' }) : null),
          el('td', { class: 'num' }, m.server_address ? m.server_address + ':' + m.server_port : '— (no server yet)'),
          el('td', null, m.map || '—'),
          el('td', null, el('span', { class: 'pill ' + m.status.toLowerCase(), text: m.status })),
          el('td', { class: 'num' }, m.players + ' / ' + m.required_players),
          el('td', { class: 'small' }, rosterText(m)),
          el('td', { class: 'small', title: formatDate(m.created_at) }, formatDate(m.created_at))));
      });
      document.getElementById('matchCount').textContent = String(matches.filter(function (m) {
        return ['FORMING', 'FULL', 'READY', 'ACCEPTING', 'ACCEPTED'].indexOf(m.status) >= 0;
      }).length);
      document.getElementById('matchEmpty').hidden = matches.length > 0;
      document.getElementById('matchTable').hidden = matches.length === 0;
    }).catch(fail);
  }

  function rosterText(m) {
    var text = m.account_ids.join(', ');
    if (m.fake_players) text += (text ? ' + ' : '') + m.fake_players + ' fake';
    return text || '—';
  }

  // the server list and the match list change without the admin doing anything (assignments, TTL): keep them fresh
  setInterval(function () {
    if (!document.getElementById('tab-servers').hidden && editingServerId === null) loadServers();
    if (!document.getElementById('tab-matches').hidden) loadMatches();
    if (!document.getElementById('tab-fake').hidden && editingFakeId === null) loadFake();
  }, 3000);

  // ---------------------------------------------------------------- fake players (TEST tool)
  // A profile per mode: how many virtual players a match of the mode gets (RESEARCH_FINDINGS.md #67). Everything here is stored
  // by the backend and applies to the NEXT match that is decided; a match that has its players keeps them.
  var fakeBody = document.querySelector('#fakeTable tbody');
  var fakeEnabledOnBackend = true;

  function acceptCategories() {
    return categories.filter(function (c) { return c.accept_required; });
  }

  function serversOfMode(mode) {
    return serversCache.filter(function (s) { return s.category === mode; });
  }

  function loadFake() {
    return api('GET', '/admin/api/fake-searches').then(function (data) {
      fakeEnabledOnBackend = data.enabled;
      document.getElementById('fakeOff').hidden = data.enabled;
      var master = document.getElementById('fakeMaster');
      if (document.activeElement !== master) master.checked = data.master;
      var win = document.getElementById('fakeWindow');
      if (document.activeElement !== win) win.value = String(data.gather_window_seconds);
      document.getElementById('fakeMasterState').textContent = data.master ? 'ON: matches get the virtual players of their mode profile'
        : 'OFF: matches wait for real players only, like without the tool';
      renderFake(data.fake_searches);
    }).catch(fail);
  }

  function renderFake(list) {
    fakeBody.textContent = '';
    list.forEach(function (f) {
      fakeBody.appendChild(f.id === editingFakeId ? fakeEditRow(f) : fakeViewRow(f));
    });
    document.getElementById('fakeCount').textContent = String(list.filter(function (f) { return f.enabled; }).length);
    document.getElementById('fakeEmpty').hidden = list.length > 0;
    document.getElementById('fakeTable').hidden = list.length === 0;
  }

  function fakeUsedText(f) {
    if (!f.matches.length) return '—';
    return f.matches.map(function (m) {
      var players = m.real_players + ' real + ' + m.fake_players + ' fake' + (m.configured !== m.fake_players ? ' (' + m.configured + ' configured)' : '');
      return m.match_id + ' ' + m.match_status.toLowerCase() + ': ' + players + (m.server_address ? ' on ' + m.server_address + ':' + m.server_port : '');
    }).join('\n');
  }

  function fakeViewRow(f) {
    var toggle = f.enabled
      ? el('button', { class: 'small', text: 'Switch off', onclick: function () { setFakeEnabled(f, false); } })
      : el('button', { class: 'small primary', text: 'Switch on', onclick: function () { setFakeEnabled(f, true); } });
    var count = f.players === 0 ? '0 (real players only)' : String(f.players);
    return el('tr', { class: f.enabled ? '' : 'ended' },
      el('td', null, el('span', { class: 'pill ' + (f.enabled ? 'enabled' : 'disabled'), text: f.status })),
      el('td', null, f.mode_label),
      el('td', { class: 'num' }, count),
      el('td', { class: 'maps' }, f.maps.length ? f.maps.join(', ') : 'any map'),
      el('td', { class: 'small' }, f.server_label || 'any server'),
      el('td', { class: 'num' }, String(f.priority)),
      el('td', { class: 'small used' }, fakeUsedText(f)),
      el('td', { class: 'actions' }, toggle,
        el('button', { class: 'small', text: 'Edit', onclick: function () { editingFakeId = f.id; loadFake(); } }),
        el('button', { class: 'small danger', text: 'Delete', onclick: function () { deleteFake(f); } })));
  }

  function serverSelect(mode, current) {
    var select = el('select');
    select.appendChild(el('option', { value: '', text: 'any server' }));
    serversOfMode(mode).forEach(function (s) {
      var o = el('option', { value: String(s.id), text: s.host + ':' + s.port + (s.map ? ' ' + s.map : '') });
      if (current === s.id) o.selected = true;
      select.appendChild(o);
    });
    return select;
  }

  function fakeEditRow(f) {
    var mode = el('select');
    acceptCategories().forEach(function (c) {
      var o = el('option', { value: c.key, text: c.label });
      if (c.key === f.mode) o.selected = true;
      mode.appendChild(o);
    });
    var players = el('input', { type: 'number', min: '0', max: String(f.max_players), value: String(f.players), class: 'narrow' });
    var maps = el('input', { value: f.maps.join(', '), placeholder: 'any map', pattern: '[A-Za-z0-9_, ]*' });
    var server = serverSelect(f.mode, f.server_id);
    mode.addEventListener('change', function () {
      var fresh = serverSelect(mode.value, null);
      server.parentNode.replaceChild(fresh, server);
      server = fresh;
    });
    var priority = el('input', { type: 'number', min: '-1000', max: '1000', value: String(f.priority), class: 'narrow' });
    return el('tr', null,
      el('td', null, el('span', { class: 'pill ' + (f.enabled ? 'enabled' : 'disabled'), text: f.status })),
      el('td', null, mode), el('td', null, players), el('td', null, maps), el('td', null, server), el('td', null, priority),
      el('td', { class: 'small used' }, fakeUsedText(f)),
      el('td', { class: 'actions' },
        el('button', { class: 'small primary', text: 'Save', onclick: function () {
          api('PUT', '/admin/api/fake-searches/' + f.id, {
            mode: mode.value, players: Number(players.value), maps: parseMaps(maps.value),
            priority: Number(priority.value), server_id: server.value ? Number(server.value) : null
          }).then(function () { editingFakeId = null; return loadFake(); }).catch(fail);
        } }),
        el('button', { class: 'small', text: 'Cancel', onclick: function () { editingFakeId = null; loadFake(); } })));
  }

  function setFakeEnabled(f, enabled) {
    api('POST', '/admin/api/fake-searches/' + f.id + '/enabled', { enabled: enabled }).then(loadFake).catch(fail);
  }

  function deleteFake(f) {
    if (!confirm('Delete this profile (' + f.players + ' fake players, ' + f.mode_label + ')?')) return;
    api('DELETE', '/admin/api/fake-searches/' + f.id).then(loadFake).catch(fail);
  }

  function parseMaps(text) {
    var seen = {}, out = [];
    text.split(/[,\s]+/).forEach(function (m) { if (m && !seen[m]) { seen[m] = true; out.push(m); } });
    return out;
  }

  // suggestions come from the registry: the maps of the servers of the selected mode
  function renderChips() {
    var chips = document.getElementById('fakeChips');
    var mode = document.getElementById('fakeMode').value;
    var input = document.getElementById('fakeMaps');
    var chosen = parseMaps(input.value);
    var maps = [];
    serversCache.forEach(function (s) {
      if (s.category === mode && s.map && maps.indexOf(s.map) < 0) maps.push(s.map);
    });
    chips.textContent = '';
    maps.forEach(function (m) {
      var on = chosen.indexOf(m) >= 0;
      chips.appendChild(el('button', { type: 'button', class: on ? 'on' : '', text: m, title: 'a game server of this mode runs it',
        onclick: function () {
          var list = parseMaps(input.value);
          var i = list.indexOf(m);
          if (i >= 0) list.splice(i, 1); else list.push(m);
          input.value = list.join(', ');
          renderChips();
        } }));
    });
    var c = categories.filter(function (x) { return x.key === mode; })[0];
    var max = c ? c.required_players - 1 : 1;
    document.getElementById('fakePlayers').max = String(max);
    var select = document.getElementById('fakeServer');
    var keep = select.value;
    select.textContent = '';
    select.appendChild(el('option', { value: '', text: 'any server' }));
    serversOfMode(mode).forEach(function (s) {
      var o = el('option', { value: String(s.id), text: s.host + ':' + s.port + (s.map ? ' ' + s.map : '') });
      if (String(s.id) === keep) o.selected = true;
      select.appendChild(o);
    });
    document.getElementById('fakeHint').textContent = c
      ? c.label + ' takes ' + c.required_players + ' players. A match gets min(count, ' + c.required_players + ' - real players) virtual players: '
        + 'the capacity is never changed or topped up to. Count 0 = the match starts with its real players only (after the gather window). '
        + 'No enabled profile / switch OFF = matches wait for real players like without the tool.'
        + (maps.length ? ' The buttons above are the maps of your ' + c.label + ' servers; a match is only played on a server whose map is ticked (empty = any).'
          : ' No ' + c.label + ' server is registered yet.')
      : '';
  }

  document.getElementById('fakeMode').addEventListener('change', renderChips);
  document.getElementById('fakeMaps').addEventListener('input', renderChips);

  document.getElementById('fakeForm').addEventListener('submit', function (ev) {
    ev.preventDefault();
    var f = ev.target;
    api('POST', '/admin/api/fake-searches', {
      mode: f.mode.value, players: Number(f.players.value), maps: parseMaps(f.maps.value), enabled: f.enabled.checked,
      priority: Number(f.priority.value || 0), server_id: f.server.value ? Number(f.server.value) : null
    }).then(function () { f.maps.value = ''; renderChips(); return loadFake(); }).catch(fail);
  });

  document.getElementById('fakeSettingsForm').addEventListener('submit', function (ev) {
    ev.preventDefault();
    api('PUT', '/admin/api/fake-settings', {
      master: document.getElementById('fakeMaster').checked,
      gather_window_seconds: Number(document.getElementById('fakeWindow').value)
    }).then(loadFake).catch(fail);
  });
  document.getElementById('fakeMaster').addEventListener('change', function () {
    api('PUT', '/admin/api/fake-settings', { master: this.checked }).then(loadFake).catch(fail);
  });

  // ---------------------------------------------------------------- start
  api('GET', '/admin/api/categories').then(function (list) {
    categories = list;
    fillCategorySelect(document.getElementById('testMode'));
    fillCategorySelect(document.getElementById('serverCategory'));
    var fakeMode = document.getElementById('fakeMode');
    acceptCategories().forEach(function (c) { fakeMode.appendChild(el('option', { value: c.key, text: c.label })); });
    return Promise.all([loadSearches(), loadServers(), loadMatches(), loadFake()]).then(renderChips);
  }).catch(fail);
})();
