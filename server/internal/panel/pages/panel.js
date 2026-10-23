// ttt2-server admin panel: the one script (served from the panel itself; the Content-Security-Policy allows no
// other). Every page and every action works without it; it adds live updates (Server-Sent Events from /events),
// the kick / ban dialog, confirmations, countdowns, relative times and filters.
//
// Every text from the server (players' names, reasons) goes into the page with textContent / createElement only:
// never as HTML (Trusted Types make the browser refuse that too, where it has them).
(function () {
  'use strict';

  var d = document;
  var body = d.body;
  if (!body || !body.classList.contains('app')) return;

  var isID = /^[0-9]{1,20}$/;
  var KINDS = { join: 1, leave: 1, match: 1, matchend: 1, kick: 1, ban: 1, unban: 1, banend: 1, info: 1 };
  var STATS = ['online', 'playing', 'rooms', 'inRooms', 'bans', 'matches24h'];
  var FEED_MAX = 100;
  var skew = 0; // the server's clock minus this browser's (s)

  function $(sel, root) { return (root || d).querySelector(sel); }
  function $$(sel, root) { return Array.prototype.slice.call((root || d).querySelectorAll(sel)); }
  function el(tag, cls, text) {
    var e = d.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function txt(s) { return d.createTextNode(String(s)); }
  function parse(s) { try { return JSON.parse(s); } catch (e) { return null; } }
  function now() { return Date.now() / 1000 + skew; }
  function meta(name) { var m = $('meta[name="' + name + '"]'); return m ? m.getAttribute('content') : ''; }
  var csrf = meta('csrf');
  var back = body.getAttribute('data-back') || '/';

  // ---- times

  function dur(s) {
    s = Math.max(0, Math.floor(s));
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    if (s < 86400) {
      var m = Math.floor(s % 3600 / 60);
      return Math.floor(s / 3600) + 'h' + (m ? ' ' + m + 'm' : '');
    }
    var h = Math.floor(s % 86400 / 3600);
    return Math.floor(s / 86400) + 'd' + (h ? ' ' + h + 'h' : '');
  }
  function countdown(s) {
    s = Math.max(0, Math.floor(s));
    if (s >= 3600) return dur(s);
    var m = Math.floor(s / 60), r = s % 60;
    return (m ? m + 'm ' : '') + (r < 10 && m ? '0' : '') + r + 's';
  }
  var dateFmt = null;
  try {
    dateFmt = new Intl.DateTimeFormat(undefined, { year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  } catch (e) { /* the server's own text stays */ }
  function full(t) { return dateFmt ? dateFmt.format(new Date(t * 1000)) : ''; }

  // <time data-ts="unix" data-fmt="ago|since|local">, <span data-until="unix">
  function refreshTimes(root) {
    var n = now();
    $$('[data-ts]', root).forEach(function (e) {
      var t = +e.getAttribute('data-ts');
      if (!t) return;
      switch (e.getAttribute('data-fmt')) {
        case 'ago':
          e.textContent = n - t < 10 ? 'just now' : dur(n - t) + ' ago';
          break;
        case 'since':
          e.textContent = dur(n - t);
          break;
        case 'local':
          if (dateFmt && !e.hasAttribute('data-done')) { e.textContent = full(t); e.setAttribute('data-done', ''); }
          return;
      }
      if (dateFmt && !e.title) e.title = full(t);
    });
    $$('[data-until]', root).forEach(function (e) {
      var t = +e.getAttribute('data-until');
      if (!t) return;
      e.textContent = t > n ? 'ends in ' + countdown(t - n) : 'ending now';
      if (dateFmt) e.title = 'until ' + full(t);
    });
  }
  refreshTimes();
  setInterval(function () { refreshTimes(); }, 1000);

  // ---- building rows (the same as the server's templates)

  function timeEl(t, fmt) {
    var e = el('time');
    e.setAttribute('datetime', new Date(t * 1000).toISOString());
    e.setAttribute('data-ts', String(t));
    e.setAttribute('data-fmt', fmt);
    return e;
  }
  function tag(cls, text) { return el('span', 'tag' + (cls ? ' ' + cls : ''), text); }
  function cell(label, cls) {
    var td = el('td', cls);
    td.setAttribute('data-label', label);
    return td;
  }
  function playerLink(id, name, cls) {
    if (!isID.test(id)) return el('span', cls, name || '?');
    var a = el('a', cls, name || id);
    a.href = '/player?steam=' + id;
    return a;
  }
  function actLink(kind, p) {
    var a = el('a', 'btn sm ' + (kind === 'kick' ? 'warn-ghost' : 'danger-ghost'), kind === 'kick' ? 'Kick' : 'Ban');
    a.href = '/player?steam=' + p.id + '#' + kind;
    a.setAttribute('data-act', kind);
    a.setAttribute('data-id', p.id);
    a.setAttribute('data-name', p.name || '');
    return a;
  }
  function stateEl(p) {
    var f = d.createDocumentFragment();
    if (p.state === 'match') {
      f.appendChild(tag(p.ranked ? 'violet' : 'ok', p.ranked ? 'Ranked match' : 'In a match'));
      if (p.vs && p.vs.length) {
        var vs = el('span', 'vs', 'vs ');
        p.vs.forEach(function (w, i) {
          if (i) vs.appendChild(txt(', '));
          vs.appendChild(playerLink(w.id, w.name));
        });
        f.appendChild(txt(' '));
        f.appendChild(vs);
      }
    } else if (p.state === 'room') {
      f.appendChild(tag('accent', (p.ranked ? 'Ranked room ' : 'Hosting a room ') + (p.filled || 0) + '/' + (p.max || 0)));
    } else {
      var t = tag('', 'Online');
      t.title = "In the menus, or a guest in another player's room";
      f.appendChild(t);
    }
    return f;
  }
  function playerRow(p) {
    var tr = el('tr');
    tr.setAttribute('data-id', p.id);
    var c = cell('Player');
    c.appendChild(playerLink(p.id, p.name, 'who'));
    if (!p.verified) {
      c.appendChild(txt(' '));
      var u = tag('warn', 'unverified');
      u.title = 'Steam did not check this login';
      c.appendChild(u);
    }
    c.appendChild(el('span', 'mono sub', p.id));
    tr.appendChild(c);
    tr.appendChild(cell('Now', 'wide-sm')).appendChild(stateEl(p));
    tr.appendChild(cell('Online for')).appendChild(timeEl(p.since, 'since'));
    tr.appendChild(cell('IP address', 'mono')).textContent = p.ip || '';
    c = cell('', 'row-actions');
    c.appendChild(actLink('kick', p));
    c.appendChild(txt(' '));
    c.appendChild(actLink('ban', p));
    tr.appendChild(c);
    return tr;
  }
  function hidden(form, name, value) {
    var i = el('input');
    i.type = 'hidden';
    i.name = name;
    i.value = value;
    form.appendChild(i);
  }
  function banRow(b) {
    var tr = el('tr');
    tr.setAttribute('data-id', b.id);
    var c = cell('Player');
    c.appendChild(playerLink(b.id, b.name || '(never logged in)', 'who'));
    c.appendChild(el('span', 'mono sub', b.id));
    tr.appendChild(c);
    tr.appendChild(cell('Kind')).appendChild(b.kick ? tag('warn', 'Kick') : tag('danger', 'Ban'));
    c = tr.appendChild(cell('Ends'));
    if (b.until) {
      var s = el('span');
      s.setAttribute('data-until', String(b.until));
      c.appendChild(s);
    } else {
      c.appendChild(tag('danger', 'never'));
    }
    tr.appendChild(cell('Reason', 'reason wide-sm')).textContent = b.reason || '-';
    c = cell('', 'row-actions');
    var f = el('form', 'inline');
    f.method = 'post';
    f.action = '/unban';
    var who = b.name || b.id;
    f.setAttribute('data-confirm', b.kick ? 'Let ' + who + ' back in now?' : 'Unban ' + who + '?');
    hidden(f, 'csrf', csrf);
    hidden(f, 'steam_id', b.id);
    hidden(f, 'back', back);
    var btn = el('button', 'btn sm', b.kick ? 'Lift kick' : 'Unban');
    btn.type = 'submit';
    f.appendChild(btn);
    c.appendChild(f);
    tr.appendChild(c);
    return tr;
  }

  // keyed redraw: rows that did not change stay as they are (focus, hover), new ones are marked
  function redraw(tbody, list, make) {
    var have = {};
    $$('tr[data-id]', tbody).forEach(function (tr) { have[tr.getAttribute('data-id')] = tr; });
    var at = tbody.firstElementChild; // where the next row belongs
    list.forEach(function (item) {
      if (!item || !isID.test(item.id)) return;
      var sig = JSON.stringify(item);
      var tr = have[item.id];
      delete have[item.id];
      if (!tr) {
        tr = make(item);
        tr.classList.add('row-new');
      } else if (tr.getAttribute('data-sig') !== sig) {
        // changed (or drawn by the server: drawn again once, the same)
        var fresh = make(item);
        if (at === tr) at = fresh;
        tbody.replaceChild(fresh, tr);
        tr = fresh;
      }
      tr.setAttribute('data-sig', sig);
      if (tr === at) at = at.nextElementSibling;
      else tbody.insertBefore(tr, at);
    });
    Object.keys(have).forEach(function (k) { tbody.removeChild(have[k]); });
    refreshTimes(tbody);
    var empty = $('[data-empty="' + tbody.id + '"]');
    if (empty) empty.hidden = list.length > 0;
    applyFilter(tbody);
  }

  // ---- filters (input[data-filter] names a tbody)

  function applyFilter(tbody) {
    var input = $('input[data-filter="' + tbody.id + '"]');
    var q = input ? input.value.trim().toLowerCase() : '';
    var shown = 0, rows = $$('tr', tbody);
    rows.forEach(function (tr) {
      var hit = !q || tr.textContent.toLowerCase().indexOf(q) >= 0;
      tr.hidden = !hit;
      if (hit) shown++;
    });
    var none = $('[data-nomatch="' + tbody.id + '"]');
    if (none) none.hidden = !(q && rows.length && !shown);
  }
  $$('input[data-filter]').forEach(function (input) {
    var tbody = d.getElementById(input.getAttribute('data-filter'));
    if (!tbody) return;
    input.hidden = false;
    input.addEventListener('input', function () { applyFilter(tbody); });
  });

  // selects that search at once
  $$('select[data-autosubmit]').forEach(function (s) {
    s.addEventListener('change', function () {
      if (s.form) { if (s.form.requestSubmit) s.form.requestSubmit(); else s.form.submit(); }
    });
  });

  // answers to actions can be closed
  $$('.flash').forEach(function (f) {
    if (!f.closest('.main')) return;
    var b = el('button', 'flash-close', '×');
    b.type = 'button';
    b.setAttribute('aria-label', 'Close');
    b.addEventListener('click', function () { f.remove(); });
    f.appendChild(b);
  });

  // ---- dialogs

  var hasDialog = typeof HTMLDialogElement === 'function';
  var actDialog = $('#act-dialog');
  var confirmDialog = $('#confirm-dialog');
  $$('dialog [data-close]').forEach(function (b) {
    b.addEventListener('click', function () { b.closest('dialog').close(); });
  });
  $$('dialog').forEach(function (dlg) {
    // a click on the backdrop closes it
    dlg.addEventListener('click', function (e) { if (e.target === dlg) dlg.close(); });
  });

  function presetLabel(form, name) {
    var r = $('input[name="' + name + '"]:checked', form);
    if (!r) return '';
    var v = r.value === 'custom' ? ($('input[name="' + name + '_custom"]', form) || {}).value : r.value;
    var n = parseInt(v, 10);
    if (isNaN(n)) return '';
    if (name === 'days') return n === 0 ? ' for good' : ' for ' + n + (n === 1 ? ' day' : ' days');
    if (n === 0) return ' (they can come back at once)';
    return n % 60 === 0 && n >= 60 ? ' for ' + n / 60 + (n === 60 ? ' hour' : ' hours') : ' for ' + n + ' min';
  }
  function updateSubmit(form) {
    var btn = $('[data-submit]', form);
    if (!btn) return;
    var mode = form.getAttribute('data-mode');
    btn.textContent = (mode === 'ban' ? 'Ban' : 'Kick') + presetLabel(form, mode === 'ban' ? 'days' : 'minutes');
  }
  // the "custom" choice is picked by typing a number
  $$('.chip.custom input[type="number"]').forEach(function (n) {
    var radio = $('input[type="radio"]', n.closest('.chip'));
    function pick() { if (radio) radio.checked = true; var f = n.form; if (f) updateSubmit(f); }
    n.addEventListener('focus', pick);
    n.addEventListener('input', pick);
  });

  function openAct(kind, id, name) {
    var form = $('form', actDialog);
    form.reset();
    form.action = kind === 'ban' ? '/ban' : '/kick';
    form.setAttribute('data-mode', kind);
    $('input[name="steam_id"]', form).value = id;
    $('[data-verb]', form).textContent = kind === 'ban' ? 'Ban' : 'Kick';
    $('[data-name]', form).textContent = name || id;
    $('[data-idtext]', form).textContent = 'Steam ID ' + id;
    $$('fieldset[data-mode]', form).forEach(function (fs) {
      var on = fs.getAttribute('data-mode') === kind;
      fs.hidden = !on;
      fs.disabled = !on;
    });
    updateSubmit(form);
    actDialog.showModal();
    var first = $('fieldset[data-mode="' + kind + '"] input:checked', form);
    if (first) first.focus();
  }
  if (hasDialog && actDialog) {
    var actForm = $('form', actDialog);
    actForm.addEventListener('change', function () { updateSubmit(actForm); });
    d.addEventListener('click', function (e) {
      var a = e.target.closest && e.target.closest('a[data-act]');
      if (!a || e.ctrlKey || e.metaKey || e.shiftKey || e.button !== 0) return;
      var id = a.getAttribute('data-id');
      if (!isID.test(id)) return;
      e.preventDefault();
      openAct(a.getAttribute('data-act'), id, a.getAttribute('data-name'));
    });
  }

  // forms with data-confirm ask first
  if (hasDialog && confirmDialog) {
    var pending = null;
    $('[data-ok]', confirmDialog).addEventListener('click', function () {
      var f = pending;
      pending = null;
      confirmDialog.close();
      if (!f) return;
      f.setAttribute('data-confirmed', '');
      if (f.requestSubmit) f.requestSubmit(); else f.submit();
    });
    d.addEventListener('submit', function (e) {
      var f = e.target;
      if (!f.hasAttribute || !f.hasAttribute('data-confirm')) return;
      if (f.hasAttribute('data-confirmed')) { f.removeAttribute('data-confirmed'); return; }
      e.preventDefault();
      pending = f;
      $('#confirm-text', confirmDialog).textContent = f.getAttribute('data-confirm');
      var ok = $('[data-ok]', confirmDialog);
      ok.textContent = (f.querySelector('button[type="submit"]') || {}).textContent || 'Confirm';
      confirmDialog.showModal();
      ok.focus();
    }, true);
  }

  // ---- live

  var pill = $('#live-pill');
  function setPill(state, text, title) {
    if (!pill) return;
    pill.setAttribute('data-state', state);
    $('.pill-text', pill).textContent = text;
    pill.title = title || '';
  }

  function applyState(s) {
    if (!s || !s.stats) return;
    STATS.forEach(function (k) {
      var v = s.stats[k];
      if (typeof v !== 'number') return;
      $$('[data-stat="' + k + '"]').forEach(function (e) {
        var t = String(v);
        if (e.textContent !== t) {
          e.textContent = t;
          if (e.classList.contains('card-value')) {
            e.classList.remove('bump');
            void e.offsetWidth;
            e.classList.add('bump');
          }
        }
      });
    });
    var players = $('tbody[data-live="players"]');
    if (players && Array.isArray(s.players)) redraw(players, s.players, playerRow);
    var bans = $('tbody[data-live="bans"]');
    if (bans && Array.isArray(s.bans) && !s.moreBans) redraw(bans, s.bans, banRow);
  }

  var feed = $('ol[data-live="feed"]');
  var cursor = body.getAttribute('data-live-since') || '';
  var epoch = cursor.split('-')[0];
  var lastSeq = parseInt(cursor.split('-')[1], 10) || 0;
  function feedItem(e) {
    var li = el('li', 'ev ev-' + (KINDS[e.kind] ? e.kind : 'info') + ' new');
    li.setAttribute('data-seq', String(e.seq));
    li.appendChild(el('span', 'ev-dot')).setAttribute('aria-hidden', 'true');
    var p = el('p', 'ev-text');
    var who = Array.isArray(e.who) ? e.who : [];
    var join = e.kind === 'match' || e.kind === 'matchend' ? ' vs ' : ', ';
    who.forEach(function (w, i) {
      if (i) p.appendChild(txt(join));
      p.appendChild(playerLink(w.id, w.name, 'who'));
    });
    p.appendChild(txt((who.length ? ' ' : '') + (e.text || '')));
    li.appendChild(p);
    li.appendChild(timeEl(e.at, 'ago'));
    return li;
  }
  function addActivity(list, id) {
    var ep = String(id || '').split('-')[0];
    if (ep && ep !== epoch) { epoch = ep; lastSeq = 0; } // a restarted server numbers afresh
    if (!feed || !Array.isArray(list)) return;
    list.forEach(function (e) {
      if (!e || typeof e.seq !== 'number' || e.seq <= lastSeq) return;
      lastSeq = e.seq;
      feed.insertBefore(feedItem(e), feed.firstChild);
    });
    while (feed.children.length > FEED_MAX) feed.removeChild(feed.lastChild);
    refreshTimes(feed);
    var empty = $('[data-empty="feed"]');
    if (empty) empty.hidden = feed.children.length > 0;
  }

  function ended(reason) {
    setPill('ended', 'Not live', reason);
    if ($('.session-ended')) return;
    var box = el('div', 'flash err session-ended');
    box.setAttribute('role', 'alert');
    var t = el('span', 'flash-text', 'Live updates stopped: ' + (reason || 'the session ended') + '. ');
    var a = el('a', '', 'Log in again');
    a.href = '/login';
    t.appendChild(a);
    box.appendChild(t);
    var main = $('#main');
    var head = $('.page-head', main);
    if (head && head.nextSibling) main.insertBefore(box, head.nextSibling); else if (main) main.appendChild(box);
  }

  if (typeof EventSource === 'function') {
    var es = new EventSource('/events?since=' + encodeURIComponent(cursor));
    setPill('connecting', 'Connecting');
    es.addEventListener('open', function () { setPill('live', 'Live', 'Updates as it happens'); });
    es.addEventListener('error', function () {
      if (es.readyState === 2) setPill('off', 'Not live', 'Reload the page to try again');
      else setPill('connecting', 'Reconnecting');
    });
    es.addEventListener('clock', function (e) {
      var t = +e.data;
      if (t) skew = t - Date.now() / 1000;
    });
    es.addEventListener('state', function (e) { applyState(parse(e.data)); });
    es.addEventListener('activity', function (e) { addActivity(parse(e.data), e.lastEventId); });
    es.addEventListener('end', function (e) {
      es.close();
      var r = parse(e.data);
      ended(r && typeof r.reason === 'string' ? r.reason : '');
    });
    window.addEventListener('pagehide', function () { es.close(); });
  }
})();
