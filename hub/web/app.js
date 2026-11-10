'use strict';
// CS:TV hub website: home (create / join), /l/<code> lobby (synced player, queue, chat, people, settings),
// /link (connect a game). Everything after the page load goes over the WebSocket (/ws, see PROTOCOL.md).

const P = { LINK: 1, QUEUE: 2, SKIP: 4, PAUSE: 8, SEEK: 16, PLAYBACK: 32, SCREEN: 64, ALL: 127, ADMIN: 128, OWNER: 256, SITE: 512 };
const PERM_NAMES = [[P.LINK, 'Play links'], [P.QUEUE, 'Add to queue'], [P.SKIP, 'Skip'], [P.PAUSE, 'Pause'],
  [P.SEEK, 'Seek'], [P.PLAYBACK, 'Speed & loop'], [P.SCREEN, 'Place the TV']];

// ---------------------------------------------------------------- small helpers
const $ = (sel, el = document) => el.querySelector(sel);
// replaceChildren without null / false (replaceChildren would print them)
function put(el, ...kids) { el.replaceChildren(...kids.flat().filter((k) => k != null && k !== false)); }
function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k in el && typeof v !== 'string') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false) el.append(kid);
  return el;
}
function icon(path) {
  const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  s.setAttribute('viewBox', '0 0 24 24');
  s.innerHTML = path;
  return s;
}
const ICONS = {
  play: '<path d="M7 5v14l11-7z" fill="currentColor" stroke="none"/>',
  pause: '<path d="M8 5v14M16 5v14"/>',
  out: '<path d="M14 4h6v6M20 4l-9 9M18 14v6H4V6h6"/>',
  back: '<path d="M11 17l-5-5 5-5M18 17l-5-5 5-5"/>',
  fwd: '<path d="M13 17l5-5-5-5M6 17l5-5-5-5"/>',
  skip: '<path d="M6 5l9 7-9 7zM18 5v14"/>',
  prev: '<path d="M18 5l-9 7 9 7zM6 5v14"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  loop: '<path d="M17 2l3 3-3 3"/><path d="M4 11V9a4 4 0 0 1 4-4h12M7 22l-3-3 3-3"/><path d="M20 13v2a4 4 0 0 1-4 4H4"/>',
  game: '<rect x="2" y="7" width="20" height="11" rx="4"/><path d="M7 11v3M5.5 12.5h3M15 12h.01M18 12h.01"/>',
  web: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
  steam: '<circle cx="12" cy="12" r="9.5"/><circle cx="15" cy="9.5" r="2.5"/><path d="M3 14l5 2M8 16a2.5 2.5 0 1 0 4.4-1.6"/>',
  trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  ban: '<circle cx="12" cy="12" r="9"/><path d="M5.6 5.6l12.8 12.8"/>',
  full: '<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>',
  cinema: '<rect x="2" y="5" width="20" height="14"/><path d="M7 5v14M17 5v14"/>',
  chat: '<path d="M4 5h16v11H9l-5 4z"/>',
  smile: '<circle cx="12" cy="12" r="9"/><path d="M8 14.5c1 1.3 2.4 2 4 2s3-.7 4-2M9 9.5h.01M15 9.5h.01"/>',
};
function fmtTime(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  sec = Math.floor(sec);
  const hh = Math.floor(sec / 3600), mm = Math.floor(sec / 60) % 60, ss = sec % 60;
  return (hh ? hh + ':' + String(mm).padStart(2, '0') : mm) + ':' + String(ss).padStart(2, '0');
}
function nameColor(id) {
  let x = 0;
  for (const ch of String(id)) x = (x * 31 + ch.charCodeAt(0)) >>> 0;
  return `hsl(${x % 360} 70% 70%)`;
}
function store(key, val) {
  try { if (val === undefined) return localStorage.getItem(key); localStorage.setItem(key, val); } catch { return null; }
}
let toastTimer;
function toast(msg, bad) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (bad ? ' bad' : '');
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}
function loadScript(src, ready) {
  if (ready()) return Promise.resolve();
  loadScript.p = loadScript.p || {};
  if (!loadScript.p[src]) {
    loadScript.p[src] = new Promise((res, rej) => {
      const s = h('script', { src, async: true });
      s.onerror = () => rej(new Error('could not load ' + src));
      s.onload = () => { const wait = () => (ready() ? res() : setTimeout(wait, 50)); wait(); };
      document.head.append(s);
    });
  }
  return loadScript.p[src];
}

// ---------------------------------------------------------------- connection + hub clock
let ws, me = null, lobby = null, st = null, everConnected = false, siteBanned = false;
let hubOffset = null;            // hub ms - performance.now()
const clock = [];                // [rtt, offset] samples
function hubNow() { return performance.now() + (hubOffset ?? 0); }
function send(m) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(m)); }

function connect() {
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  ws.onopen = () => send({ t: 'hello', kind: 'web' });
  ws.onmessage = (e) => { try { onMsg(JSON.parse(e.data)); } catch (err) { console.error(err); } };
  ws.onclose = (e) => {
    if (siteBanned || (e.code === 4003 && e.reason === 'banned')) { siteBanned = true; renderGone("You're banned from cstv.xyz."); return; }
    if (e.code === 4003) { renderGone('Open this page at ' + location.protocol + '//cstv.xyz.'); return; }   // wrong origin
    if (everConnected) toast('Lost the connection, reconnecting…', true);
    setTimeout(connect, 2000);
  };
}
function syncClock() {
  let n = 0;
  const go = () => { send({ t: 'time', c: performance.now() }); if (++n < 6) setTimeout(go, 120); };
  go();
}
setInterval(() => { if (ws && ws.readyState === 1) syncClock(); }, 30000);

function route() {
  const p = location.pathname;
  const m = p.match(/^\/l\/([A-Za-z0-9]{4,8})$/);
  if (m) return { view: 'lobby', code: m[1].toUpperCase() };
  if (p === '/link') return { view: 'link' };
  if (p === '/admin') return { view: 'admin' };
  return { view: 'home' };
}
function go(path) { history.pushState(null, '', path); show(); }
window.addEventListener('popstate', show);
for (const a of document.querySelectorAll('.nav a, .brand')) {
  a.addEventListener('click', (e) => { if (e.button || e.metaKey || e.ctrlKey) return; e.preventDefault(); go(a.getAttribute('href')); });
}

function lobbyPw(code, pw) {   // the password this tab joined a lobby with (reconnects rejoin by themselves)
  try { if (pw === undefined) return sessionStorage.getItem('cstv_pw_' + code) || undefined; sessionStorage.setItem('cstv_pw_' + code, pw); } catch { return undefined; }
}
function silentIn(code, on) {   // a site admin's silent join (from the admin page), kept for reconnects in this tab
  try { if (on === undefined) return sessionStorage.getItem('cstv_silent_' + code) === '1'; sessionStorage.setItem('cstv_silent_' + code, on ? '1' : ''); } catch { return false; }
}
let pollTimer = null;   // the home and admin pages refresh their lists
function poll(fn, ms) {
  clearInterval(pollTimer);
  pollTimer = fn ? setInterval(() => { if (ws && ws.readyState === 1 && !document.hidden) fn(); }, ms) : null;
}
function show() {
  const r = route();
  for (const a of document.querySelectorAll('.nav a')) a.classList.toggle('on', a.dataset.nav === r.view);
  document.body.dataset.view = r.view;
  if (r.view !== 'lobby' && lobby) { send({ t: 'leave' }); lobby = null; st = null; destroyPlayer(); }
  applyCinema();
  $('#lobbyHead').hidden = r.view !== 'lobby';
  poll(null);
  if (r.view === 'home') {
    renderHome();
    send({ t: 'mylobbies' });
    send({ t: 'lobbies', page: lobbyPage });
    poll(() => send({ t: 'lobbies', page: lobbyPage }), 10000);
  } else if (r.view === 'link') renderLink();
  else if (r.view === 'admin') {
    if (!(me && me.admin)) { renderGone('Not found.'); return; }
    put($('#view'), h('div', { class: 'loading' }, 'Loading…'));
    send({ t: 'site', action: 'overview' });
    poll(() => send({ t: 'site', action: 'overview' }), 5000);
  } else {
    put($('#view'), h('div', { class: 'loading' }, 'Joining…'));
    send({ t: 'join', code: r.code, password: lobbyPw(r.code), silent: silentIn(r.code) || undefined });
  }
}
function renderPassword(m) {
  $('#lobbyHead').hidden = true;
  const input = h('input', { type: 'password', maxlength: 64, placeholder: 'Password', 'aria-label': 'Password', autocomplete: 'off' });
  put($('#view'), h('section', { class: 'home' },
    h('h1', {}, m.name || m.lobby),
    h('div', { class: 'section' },
      h('header', {}, h('span', { class: 'label' }, 'Password'), m.code === 'password' ? null : h('span', { class: 'bad' }, m.msg)),
      h('form', { class: 'joinbar', onsubmit: (e) => {
        e.preventDefault();
        if (!input.value) return;
        lobbyPw(m.lobby, input.value);
        send({ t: 'join', code: m.lobby, password: input.value });
      } }, input, h('button', { class: 'primary' }, 'Join')))));
  input.focus();
}

function onMsg(m) {
  switch (m.t) {
    case 'welcome':
      everConnected = true;
      me = { steamid: m.steamid, name: m.name, avatar: m.avatar, gifs: !!m.gifs, stats: m.stats, admin: !!m.admin };
      if (hubOffset == null) hubOffset = m.now - performance.now();
      syncClock();
      renderMe();
      show();
      break;
    case 'time': {
      const rtt = performance.now() - m.c;
      clock.push([rtt, m.h - (m.c + rtt / 2)]);
      if (clock.length > 12) clock.shift();
      hubOffset = clock.reduce((a, b) => (b[0] < a[0] ? b : a))[1];
      break;
    }
    case 'lobby':
      lobby = m;
      st = m.state;
      if (hubOffset == null) hubOffset = st.now - performance.now();
      renderLobby();
      break;
    case 'state':
      st = m;
      if (lobby) { lobby.state = m; onState(); }
      break;
    case 'queue': if (lobby) { lobby.queue = m.items; renderQueue(); } break;
    case 'history': if (lobby) { lobby.history = m.items; renderHistory(); } break;
    case 'members': if (lobby) { Object.assign(lobby, { members: m.list, guests: m.guests, guest_list: m.guest_list, banned: m.banned }); renderPeople(); } break;
    case 'chat': if (lobby) { lobby.chat.push(m.msg); addChat(m.msg); } break;
    case 'chat_delete':
      if (lobby) {
        lobby.chat = lobby.chat.filter((x) => !m.ids.includes(x.id));
        for (const id of m.ids) { const el = els.chatLog && els.chatLog.querySelector(`[data-id="${id}"]`); if (el) el.remove(); }
      }
      break;
    case 'gifs': if (gifPick) gifPick.show(m); break;
    case 'you': if (lobby) { lobby.you = m; renderPerms(); renderPeople(); renderSettings(); } break;
    case 'settings': if (lobby) { Object.assign(lobby, m); delete lobby.t; renderHead(); renderPerms(); renderSettings(); loadEmotes(); } break;
    case 'created':
      history.replaceState(null, '', '/l/' + m.code);
      document.body.dataset.view = 'lobby';
      for (const a of document.querySelectorAll('.nav a')) a.classList.remove('on');
      $('#lobbyHead').hidden = false;
      break;
    case 'mylobbies': renderMyLobbies(m); break;
    case 'lobbies': renderAllLobbies(m); break;
    case 'site_overview': if (route().view === 'admin') renderAdmin(m); break;
    case 'site_visits': if (route().view === 'admin') renderVisits(m); break;
    case 'warn': showWarning(m.msg); break;
    case 'deleted': if (route().view === 'home') send({ t: 'mylobbies' }); else go('/'); toast('Lobby deleted'); break;
    case 'kicked': lobby = null; st = null; destroyPlayer(); renderGone(m.msg); break;
    case 'denied': toast(m.msg, true); break;
    case 'whep': case 'whep_fail': whepAnswer(m); break;
    case 'hls': case 'hls_fail': hlsAnswer(m); break;
    case 'ytchat_status': ytStatus(m); break;
    case 'ytchat': ytAdd(m); break;
    case 'ytchat_del': ytRemove(m.v, (x) => m.ids.includes(x.id)); break;
    case 'ytchat_clear': ytRemove(m.v, (x) => x.ch === m.ch); break;
    case 'error':
      if (m.code === 'sitebanned') { siteBanned = true; renderGone("You're banned from cstv.xyz."); }
      else if (m.code === 'nolobby' || m.code === 'banned') renderGone(m.code === 'banned' ? "You're banned from this lobby." : 'There is no lobby with that code.');
      else if (m.code === 'password' || m.code === 'wrong' || m.code === 'locked') { lobbyPw(m.lobby, ''); renderPassword(m); }
      else if (m.code === 'haslobby') { toast(m.msg); go('/l/' + m.lobby); }
      else toast(m.msg, true);
      break;
    case 'notice': toast(m.msg); break;
    case 'pair_code': showPairCode(m); break;
    case 'game_linked': toast('Game connected: ' + m.label); send({ t: 'games' }); break;
    case 'games': renderGames(m.items); break;
  }
}

// ---------------------------------------------------------------- header
// Valve's official "Sign in through Steam" button (the one every Steam sign-in uses): it goes to steamcommunity.com.
function signInLink(next) {
  return h('a', { class: 'steam-btn', href: '/login?next=' + encodeURIComponent(next || location.pathname),
    title: 'Sign in through Steam' },
    h('img', { src: '/static/steam-signin.png', alt: 'Sign in through Steam', width: 180, height: 35 }));
}
function renderMe() {
  $('#navAdmin').hidden = !(me && me.admin);
  const el = $('#me');
  if (!me || !me.steamid) { put(el, signInLink(null)); return; }
  put(el, 
    me.avatar ? h('img', { src: me.avatar, alt: '' }) : null,
    h('span', { class: 'who' }, me.name || me.steamid),
    h('a', { href: '/logout' }, 'Sign out'));
}
function renderHead() {
  if (!lobby) return;
  put($('#lobbyName'), lobby.name, lobby.locked ? h('span', { class: 'lock', title: 'Has a password' }, icon(ICONS.lock)) : null,
    lobby.you && lobby.you.silent ? h('span', { class: 'badge held', title: "Nobody here can see you're in this lobby" }, 'Silent') : null);
  $('#codeText').textContent = lobby.code;
  document.title = lobby.name + ' · CS:TV';
}
$('#codeBtn').addEventListener('click', async () => {
  if (!lobby) return;
  try { await navigator.clipboard.writeText(lobby.code); toast(`Copied ${lobby.code}`); }
  catch { toast(`Code: ${lobby.code}`); }
});

// ---------------------------------------------------------------- home + link pages
function renderHome() {
  document.title = 'CS:TV';
  const signedIn = me && me.steamid;
  const joinInput = h('input', { placeholder: 'Code', maxlength: 8, autocomplete: 'off', spellcheck: false, 'aria-label': 'Lobby code' });
  put($('#view'), h('section', { class: 'home' },
    h('div', { class: 'section' },
      h('header', {}, h('span', { class: 'label' }, 'Join')),
      h('div', { class: 'joinbar' },
        h('form', { onsubmit: (e) => { e.preventDefault(); const c = joinInput.value.trim().toUpperCase(); if (c) go('/l/' + c); } },
          joinInput, h('button', {}, 'Join')),
        signedIn ? h('button', { class: 'primary', onclick: () => send({ t: 'create' }) }, 'Create lobby') : null)),
    h('div', { class: 'section', id: 'allLobbies' }),
    signedIn ? h('div', { class: 'section', id: 'myLobbies' }) : null,
    h('div', { class: 'section', id: 'recentLobbies', hidden: true })));
}
// ---- every lobby: most people first; what plays and who's there for lobbies without a password
let lobbyPage = 1;
function thumbOf(p) {
  const v = p && p.url ? parseVideo(p.url) : null;
  if (v && v.kind === 'yt') return `https://i.ytimg.com/vi/${v.id}/mqdefault.jpg`;
  if (v && v.kind === 'twc') return `https://static-cdn.jtvnw.net/previews-ttv/live_user_${v.channel.toLowerCase()}-320x180.jpg`;
  return null;
}
function lobbyRows(items, admin) {
  return items.map((l) => {
    const p = l.playing, th = thumbOf(p);
    const who = l.names ? [...l.names, l.guests ? `${l.guests} guest${l.guests > 1 ? 's' : ''}` : null].filter(Boolean).join(', ') : '';
    return h('tr', {},
      h('td', { class: 'th' }, th ? h('img', { src: th, alt: '', loading: 'lazy' }) : h('span', { class: 'th-none' })),
      h('td', { class: 'n' },
        h('span', { class: 't' }, l.name, l.locked ? h('span', { class: 'm', title: 'Password' }, ' ', icon(ICONS.lock)) : null),
        h('span', { class: 'sub' }, p ? (p.paused ? 'Paused: ' : '') + p.title : l.locked && !admin ? '' : 'Nothing playing')),
      h('td', { class: 'c' }, l.code),
      h('td', { class: 'ppl hide-s', title: who }, h('span', { class: 'dot' + (l.people ? ' on' : '') }), String(l.people),
        who ? h('span', { class: 'who' }, who) : null),
      h('td', { class: 'x' }, admin ? h('button', { title: 'Join without anyone seeing you', onclick: () => { silentIn(l.code, true); go('/l/' + l.code); } }, 'Silent') : null,
        h('button', { onclick: () => { if (admin) silentIn(l.code, false); go('/l/' + l.code); } }, 'Open'),
        admin ? deleteButton(l.code) : null));
  });
}
function deleteButton(code) {
  let armed = false;
  const b = h('button', { class: 'danger', onclick: () => {
    if (!armed) { armed = true; b.textContent = 'Confirm'; setTimeout(() => { armed = false; b.textContent = 'Delete'; }, 4000); return; }
    send({ t: 'site', action: 'delete_lobby', code });
  } }, 'Delete');
  return b;
}
function pager(page, pages, goPage) {
  if (pages <= 1) return null;
  return h('div', { class: 'pager' },
    h('button', { disabled: page <= 1, onclick: () => goPage(page - 1) }, 'Prev'),
    h('span', { class: 'count' }, `${page} / ${pages}`),
    h('button', { disabled: page >= pages, onclick: () => goPage(page + 1) }, 'Next'));
}
function renderAllLobbies(m) {
  const box = $('#allLobbies');
  if (!box) return;
  lobbyPage = m.page;
  put(box, h('header', {}, h('span', { class: 'label' }, 'Lobbies'), h('span', { class: 'count' }, m.total || '')),
    h('table', { class: 'list lobbies' }, h('tbody', {}, m.items.length ? lobbyRows(m.items, false)
      : h('tr', { class: 'empty' }, h('td', { colspan: 5 }, 'None')))),
    pager(m.page, m.pages, (n) => send({ t: 'lobbies', page: n })));
}
function lobbyTable(items, empty) {
  return h('table', { class: 'list' }, h('tbody', {}, items.length
    ? items.map((l) => h('tr', {},
        h('td', { class: 'c' }, l.code),
        h('td', { class: 'n' }, l.name, l.locked ? h('span', { class: 'm', title: 'Password' }, ' ', icon(ICONS.lock)) : null),
        h('td', { class: 'x' }, h('button', { onclick: () => go('/l/' + l.code) }, 'Open'))))
    : h('tr', { class: 'empty' }, h('td', { colspan: 3 }, empty))));
}
function renderMyLobbies(m) {
  const box = $('#myLobbies');
  if (box) put(box, h('header', {}, h('span', { class: 'label' }, 'Your lobbies'), h('span', { class: 'count' }, m.owned.length || '')),
    lobbyTable(m.owned, 'None'));
  const rec = $('#recentLobbies');
  if (rec) {
    rec.hidden = !m.recent.length;
    put(rec, h('header', {}, h('span', { class: 'label' }, 'Recent')), lobbyTable(m.recent, ''));
  }
}
function renderLink() {
  document.title = 'Connect game · CS:TV';
  const signedIn = me && me.steamid;
  put($('#view'), h('section', { class: 'home' },
    h('div', { class: 'section' },
      h('header', {}, h('span', { class: 'label' }, 'Game code')),
      h('div', { id: 'pairBox' }, h('button', { class: 'primary', disabled: !signedIn, title: signedIn ? null : 'Sign in through Steam first',
        onclick: () => send({ t: 'pair_code' }) }, 'Get code'))),
    signedIn ? h('div', { class: 'section' }, h('header', {}, h('span', { class: 'label' }, 'Connected games')), h('div', { id: 'gamesBox' })) : null));
  if (signedIn) send({ t: 'games' });
}
let pairTimer = null;
function showPairCode(m) {
  const box = $('#pairBox');
  if (!box) return;
  const until = Date.now() + m.expires_in * 1000;
  const left = h('span', { class: 'timer' });
  const tick = () => {
    const s = Math.max(0, Math.round((until - Date.now()) / 1000));
    left.textContent = s ? `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}` : 'Expired';
    if (s) pairTimer = setTimeout(tick, 1000);
  };
  clearTimeout(pairTimer);
  put(box, h('div', { class: 'pair-code' }, '!tvlogin ' + m.code), h('div', { class: 'row' }, left, h('button', { onclick: () => send({ t: 'pair_code' }) }, 'New code')));
  tick();
}
function renderGames(items) {
  const box = $('#gamesBox');
  if (!box) return;
  const when = (t) => new Date(t * 1000).toLocaleDateString();
  put(box, h('table', { class: 'list' }, h('tbody', {}, items.length
    ? items.map((g) => h('tr', {}, h('td', { class: 'n' }, g.label), h('td', { class: 'm hide-s' }, when(g.last)),
        h('td', { class: 'x' }, h('button', { class: 'danger', onclick: () => send({ t: 'revoke', id: g.id }) }, 'Sign out'))))
    : h('tr', { class: 'empty' }, h('td', {}, 'None')))));
}
// ---------------------------------------------------------------- dialogs
// One at a time, over everything. A warning from an admin can only be closed with its button.
function modal(kids, { locked = false } = {}) {
  const back = $('#modal');
  const box = h('div', { class: 'modal', role: 'dialog', 'aria-modal': 'true' }, kids);
  put(back, box);
  back.hidden = false;
  back.onclick = locked ? null : (e) => { if (e.target === back) closeModal(); };
  back.onkeydown = locked ? null : (e) => { if (e.key === 'Escape') closeModal(); };
  const f = box.querySelector('textarea, input, button.primary');
  if (f) f.focus();
}
function closeModal() { const b = $('#modal'); b.hidden = true; put(b); }
function showWarning(msg) {
  modal([h('span', { class: 'label warn' }, 'Warning'), h('p', { class: 'warn-text' }, msg),
    h('div', { class: 'row end' }, h('button', { class: 'primary', onclick: closeModal }, 'I understand'))], { locked: true });
}

// ---------------------------------------------------------------- admin page (cstv.xyz admins)
function siteAct(action, extra) { send({ t: 'site', action, ...extra }); }
function adminDialog(title, field, okLabel, onOk, danger) {
  modal([h('span', { class: 'label' }, title), field,
    h('div', { class: 'row end' }, h('button', { onclick: closeModal }, 'Cancel'),
      h('button', { class: danger ? 'primary danger-fill' : 'primary', onclick: () => { onOk(); closeModal(); } }, okLabel))]);
}
function warnDialog(p) {
  const ta = h('textarea', { maxlength: 500, rows: 4, placeholder: 'Message', 'aria-label': 'Warning' });
  adminDialog(`Warn ${p.name}`, ta, 'Send', () => { if (ta.value.trim()) siteAct('warn', { ...who(p), msg: ta.value.trim() }); });
}
function muteDialog(p) {
  const sel = h('select', { 'aria-label': 'How long' }, [[10, '10 min'], [60, '1 hour'], [1440, '1 day'], [10080, '1 week'], [-1, 'For good']]
    .map(([v, l]) => h('option', { value: v }, l)));
  adminDialog(`Mute ${p.name} everywhere`, sel, 'Mute', () => siteAct('mute', { steamid: p.steamid, minutes: +sel.value }));
}
// Steam ban: the account, wherever it connects from. IP ban: the address they're on now (anyone there).
function banDialog(p, ip) {
  const reason = h('input', { maxlength: 200, placeholder: 'Reason', 'aria-label': 'Reason' });
  adminDialog(`${ip ? 'IP' : 'Steam'} ban ${p.name}`, h('div', { class: 'col' }, reason,
    h('span', { class: 'count' }, ip ? ['IP ', ipSpoiler(p.ip)] : `Steam ${p.steamid}`)),
    'Ban', () => siteAct(ip ? 'ipban' : 'ban', { ...who(p), reason: reason.value }), true);
}
// IPs stay blurred (screen sharing) until clicked; remembered while this page is open
const shownIps = new Set();
function ipSpoiler(ip) {
  const el = h('span', { class: 'spoiler' + (shownIps.has(ip) ? ' shown' : ''), title: 'Click to show / hide', onclick: (e) => {
    e.stopPropagation();
    if (shownIps.has(ip)) shownIps.delete(ip); else shownIps.add(ip);
    el.classList.toggle('shown', shownIps.has(ip));
  } }, ip);
  return el;
}
const who = (p) => (p.steamid ? { steamid: p.steamid } : { conn: p.conn });
function when(t) { return t ? new Date(t * 1000).toLocaleString() : ''; }
function renderAdmin(m) {
  document.title = 'Admin · CS:TV';
  const people = m.people.map((p) => h('tr', {},
    h('td', { class: 'n' }, h('span', { class: 't' }, p.name, p.admin ? h('span', { class: 'badge held' }, 'Admin') : null),
      h('span', { class: 'sub' }, [p.steamid || 'guest', p.kind === 'game' ? 'game' : 'website'].join('  '), '  ', ipSpoiler(p.ip))),
    h('td', { class: 'm hide-s' }, p.code ? h('a', { href: '/l/' + p.code, onclick: (e) => { e.preventDefault(); silentIn(p.code, true); go('/l/' + p.code); } }, p.where) : p.where),
    h('td', { class: 'x' }, p.admin ? null : [
      h('button', { onclick: () => warnDialog(p) }, 'Warn'),
      p.steamid ? h('button', { onclick: () => muteDialog(p) }, 'Mute') : null,
      p.code ? h('button', { onclick: () => siteAct('kick', who(p)) }, 'Kick') : null,
      p.steamid ? h('button', { class: 'danger', onclick: () => banDialog(p, false) }, 'Steam ban') : null,
      h('button', { class: 'danger', onclick: () => banDialog(p, true) }, 'IP ban')])));
  const bans = [
    ...m.bans.map((b) => h('tr', {}, h('td', { class: 'n' }, h('span', { class: 't' }, b.name), h('span', { class: 'sub' }, [b.steamid, b.reason, when(b.at)].filter(Boolean).join('  '))),
      h('td', { class: 'x' }, h('button', { onclick: () => siteAct('unban', { steamid: b.steamid }) }, 'Unban')))),
    ...m.ip_bans.map((b) => h('tr', {}, h('td', { class: 'n' }, h('span', { class: 't' }, 'IP ', ipSpoiler(b.ip)), h('span', { class: 'sub' }, [b.name, b.reason, when(b.at)].filter(Boolean).join('  '))),
      h('td', { class: 'x' }, h('button', { onclick: () => siteAct('unban', { ip: b.ip }) }, 'Unban')))),
    ...m.mutes.map((x) => h('tr', {}, h('td', { class: 'n' }, h('span', { class: 't' }, x.name), h('span', { class: 'sub' }, 'Muted until ' + when(x.until))),
      h('td', { class: 'x' }, h('button', { onclick: () => siteAct('mute', { steamid: x.steamid, minutes: 0 }) }, 'Unmute'))))];
  const tbl = (rows, cols) => h('table', { class: 'list rows2' }, h('tbody', {}, rows.length ? rows : h('tr', { class: 'empty' }, h('td', { colspan: cols }, 'None'))));
  const sec = (label, count, body) => h('div', { class: 'section' }, h('header', {}, h('span', { class: 'label' }, label), h('span', { class: 'count' }, count || '')), body);
  // the live part is redrawn every 5 s; the visitor log keeps its own box (its day picker stays open)
  let live = $('#adminLive');
  if (!live) {
    put($('#view'), h('section', { class: 'home admin' }, live = h('div', { id: 'adminLive' }), h('div', { class: 'section', id: 'adminVisits' })));
    vis.days = '';
    visitsLoad(vis.day);
  } else if (vis.data && vis.data.day === vis.data.today && Date.now() - vis.at > 30000) visitsLoad(vis.day);
  put(live,
    sec('Lobbies', m.lobbies.length, tbl(lobbyRows(m.lobbies, true), 5)),
    sec('Online', m.people.length, tbl(people, 3)),
    sec('Bans and mutes', bans.length, tbl(bans, 2)));
}

// ---- visitor log: a day at a time (in this browser's time zone), a row per address + account + website/game
const vis = { day: null, sort: 'first', dir: -1, data: null, at: 0, days: '', ips: false };
function visitsLoad(day) {
  vis.at = Date.now();
  siteAct('visits', { tz: Intl.DateTimeFormat().resolvedOptions().timeZone, day: day || undefined });
}
function uaShort(ua) {
  if (!ua) return '';
  const b = /Edg\//.test(ua) ? 'Edge' : /OPR\//.test(ua) ? 'Opera' : /Vivaldi/.test(ua) ? 'Vivaldi' : /Firefox\//.test(ua) ? 'Firefox'
    : /Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : 'Other';
  const os = /iPhone|iPad/.test(ua) ? 'iOS' : /Android/.test(ua) ? 'Android' : /Windows/.test(ua) ? 'Windows'
    : /Mac OS X/.test(ua) ? 'Mac' : /CrOS/.test(ua) ? 'ChromeOS' : /Linux/.test(ua) ? 'Linux' : '';
  return os ? b + ' · ' + os : b;
}
const clockOf = (t) => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
function dayLabel(d, today) {
  if (d === today) return 'Today';
  return new Date(d + 'T12:00:00').toLocaleDateString([], { weekday: 'short', day: 'numeric', month: 'short' });
}
const VIS_COLS = [['first', 'Time'], ['name', 'Name'], ['ip', 'IP'], ['kind', 'Where'], ['ua', 'Browser'], ['n', 'Visits'], ['last', 'Last seen']];
function visKey(it, k) {
  if (k === 'name') return (it.steamid ? it.name || '' : '~' + (it.name || '')).toLowerCase();   // guests after
  if (k === 'ua') return it.kind === 'game' ? '~' : uaShort(it.ua);
  if (k === 'ip') return it.ip.includes(':') ? it.ip : it.ip.split('.').map((x) => x.padStart(3, '0')).join('.');
  return it[k] ?? '';
}
function renderVisits(m) {
  vis.data = m;
  vis.day = m.day;
  const box = $('#adminVisits');
  if (!box) return;
  const days = JSON.stringify(m.days) + m.day;
  if (vis.days !== days) {   // the header: only when the days change (keeps the picker as it is)
    vis.days = days;
    const pick = h('select', { 'aria-label': 'Day', onchange: (e) => visitsLoad(e.target.value) },
      m.days.map((d) => h('option', { value: d.day, selected: d.day === m.day }, dayLabel(d.day, m.today) + '  ' + d.n)));
    const ipsBtn = h('button', { class: 'toggle' + (vis.ips ? ' on' : ''), onclick: () => {
      vis.ips = !vis.ips;
      ipsBtn.classList.toggle('on', vis.ips);
      renderVisits(vis.data);
    } }, 'Show IPs');
    const cur = m.days.find((d) => d.day === m.day);
    put(box, h('header', {}, h('span', { class: 'label' }, 'Visitors'), h('span', { class: 'count' }, cur ? cur.n : 0),
      h('span', { class: 'spacer' }), ipsBtn, pick), h('div', { class: 'visits-body' }));
  }
  const items = [...m.items].sort((a, b) => {
    const x = visKey(a, vis.sort), y = visKey(b, vis.sort);
    return (x < y ? -1 : x > y ? 1 : 0) * vis.dir || b.first - a.first;
  });
  const head = h('tr', {}, VIS_COLS.map(([k, label]) => h('th', { class: 'sort' + (vis.sort === k ? ' on' : '') + (k === 'ua' || k === 'last' ? ' hide-s' : ''),
    onclick: () => { vis.dir = vis.sort === k ? -vis.dir : (k === 'first' || k === 'last' || k === 'n' ? -1 : 1); vis.sort = k; renderVisits(vis.data); } },
  label, vis.sort === k ? (vis.dir < 0 ? ' ▼' : ' ▲') : '')));
  const rows = items.map((it) => h('tr', {},
    h('td', { class: 'm' }, clockOf(it.first)),
    h('td', { class: 'n' }, h('span', { class: 't' }, it.online ? h('span', { class: 'dot on', title: 'Online' }) : null,
      it.steamid ? h('a', { href: 'https://steamcommunity.com/profiles/' + it.steamid, target: '_blank', rel: 'noopener' }, it.name || it.steamid)
        : (it.name && it.name !== 'guest' ? it.name + ' (guest)' : 'guest'))),
    h('td', {}, vis.ips ? it.ip : ipSpoiler(it.ip)),
    h('td', { class: 'm' }, it.kind === 'game' ? 'Game' : 'Website'),
    h('td', { class: 'm hide-s' }, it.kind === 'game' ? 'CS:S' : uaShort(it.ua)),
    h('td', { class: 'm' }, it.n),
    h('td', { class: 'm hide-s' }, it.online ? 'Now' : clockOf(it.last))));
  put(box.querySelector('.visits-body'), h('table', { class: 'list visits' }, h('thead', {}, head),
    h('tbody', {}, rows.length ? rows : h('tr', { class: 'empty' }, h('td', { colspan: VIS_COLS.length }, 'None')))));
}

function renderGone(msg) {
  $('#lobbyHead').hidden = true;
  put($('#view'), h('section', { class: 'home' }, h('h1', {}, msg), h('button', { onclick: () => go('/') }, 'Back')));
}

// ---------------------------------------------------------------- lobby page
let els = {};
const can = (perm) => lobby && lobby.you && (lobby.you.perms & perm) !== 0;

function renderLobby() {
  destroyPlayer();          // a fresh page: the old player's element is gone
  setWatching.on = undefined;
  renderHead();
  const addUrl = h('input', { placeholder: 'Link', 'aria-label': 'Link' });
  let addMode = 'link';
  els = {
    player: h('div', { class: 'player' }),
    title: h('div', { class: 'now-title' }),
    meta: h('div', { class: 'now-meta' }),
    pause: h('button', { title: 'Pause / resume for everyone', onclick: () => send({ t: 'pause', paused: !st.paused }) }),
    back: h('button', { title: 'Back 5 s', onclick: () => send({ t: 'seek', rel: -5000 }) }, icon(ICONS.back), '5s'),
    fwd: h('button', { title: 'Forward 5 s', onclick: () => send({ t: 'seek', rel: 5000 }) }, icon(ICONS.fwd), '5s'),
    prev: h('button', { title: 'Back to the video before', onclick: () => send({ t: 'prev' }) }, icon(ICONS.prev), 'Prev'),
    skip: h('button', { title: 'Next video', onclick: () => send({ t: 'skip' }) }, icon(ICONS.skip), 'Skip'),
    loop: h('button', { class: 'toggle', title: 'Loop this video', onclick: () => send({ t: 'loop', on: !st.loop }) }, icon(ICONS.loop), 'Loop'),
    speed: h('select', { title: 'Speed for everyone', onchange: (e) => send({ t: 'speed', pct: +e.target.value }) },
      [25, 50, 75, 100, 125, 150, 175, 200].map((v) => h('option', { value: v }, v === 100 ? 'Normal' : v / 100 + '×'))),
    quality: h('select', { title: 'Quality (yours only)', 'aria-label': 'Quality', onchange: (e) => { store('cstv_quality', e.target.value); applyQuality(); } },
      QUALITIES.map(([v, l]) => h('option', { value: v, selected: v === quality() }, l))),
    cinema: h('button', { class: 'toggle', title: 'Cinema (yours only): the rest of the page goes dark until the mouse moves', onclick: () => setCinema(!cinemaOn()) },
      icon(ICONS.cinema), 'Cinema'),
    xchatBtn: h('button', { class: 'toggle', hidden: true, onclick: () => { store('cstv_xchat', store('cstv_xchat') === '1' ? '0' : '1'); updateXchat(); } },
      icon(ICONS.chat), h('span', {})),
    xchatLog: h('div', { class: 'chatlog xchat-log', 'aria-live': 'polite', hidden: true }),
    xchatHead: h('span', { class: 'label' }),
    xchatTools: h('span', { class: 'xchat-tools' }),
    xchatFrame: h('iframe', { class: 'xchat-frame', hidden: true, title: 'Twitch chat' }),
    vol: h('input', { type: 'range', min: 0, max: 100, value: +(store('cstv_vol') ?? 80), oninput: (e) => { store('cstv_vol', e.target.value); applyVolume(); } }),
    add: h('form', { class: 'add', onsubmit: (e) => {
      e.preventDefault();
      const url = addUrl.value.trim();
      if (!url) return;
      send({ t: addMode, url });
      addUrl.value = '';
    } }, addUrl,
      h('button', { class: 'primary', onclick: () => { addMode = 'link'; } }, 'Play now'),
      h('button', { onclick: () => { addMode = 'queue'; } }, 'Queue')),
    queue: h('tbody'),
    history: h('div', { class: 'history' }),
    clearQ: h('button', { onclick: () => send({ t: 'queue_action', action: 'clear' }) }, 'Clear'),
    chatLog: h('div', { class: 'chatlog', 'aria-live': 'polite' }),
    chatFoot: h('div', { class: 'chat-foot' }),
    people: h('div', { class: 'people' }),
    settings: h('div', { class: 'settings' }),
    tabs: {},
  };
  const tab = (id, label) => {
    const b = h('button', { onclick: () => pickTab(id) }, label);
    els.tabs[id] = b;
    return b;
  };
  els.peopleCount = h('span', { class: 'count' });
  els.seek = makeSeek();
  els.xchat = h('aside', { class: 'xchat', hidden: true }, h('header', { class: 'xchat-head' }, els.xchatHead, els.xchatTools), els.xchatLog, els.xchatFrame);
  const side = h('aside', { class: 'side' },
    h('div', { class: 'tabs' }, tab('chat', 'Chat'), tab('people', ['People', els.peopleCount]), tab('history', 'History'), tab('settings', 'Settings')),
    h('div', { class: 'tab', id: 'tab-chat' }, els.chatLog, els.chatFoot),
    h('div', { class: 'tab', id: 'tab-people', hidden: true }, els.people),
    h('div', { class: 'tab', id: 'tab-history', hidden: true }, els.history),
    h('div', { class: 'tab', id: 'tab-settings', hidden: true }, els.settings));
  els.lobby = h('section', { class: 'lobby' },
    h('div', { class: 'stage' },
      h('div', { class: 'player-wrap' }, els.player),
      h('div', { class: 'now' }, els.title, els.meta, els.seek.el),
      h('div', { class: 'controls' }, els.back, els.pause, els.fwd, els.prev, els.skip, els.loop, els.speed, els.quality,
        els.cinema, els.xchatBtn,
        h('button', { title: 'Full screen', onclick: toggleFullscreen }, icon(ICONS.full), 'Full screen'),
        h('span', { class: 'spacer' }),
        h('label', { class: 'vol' }, 'Volume', els.vol)),
      els.add,
      h('div', { class: 'section queue' }, h('header', {}, h('span', { class: 'label' }, 'Queue'), els.queueCount = h('span', { class: 'count' }),
        h('span', { class: 'spacer' }), els.clearQ), h('table', { class: 'list' }, els.queue))),
    els.xchat,
    side);
  put($('#view'), els.lobby);
  applyCinema();
  if (boxWatch) boxWatch.disconnect();
  boxWatch = new ResizeObserver(() => fitEmbed());
  boxWatch.observe(els.player);
  put(els.chatLog, );
  for (const msg of lobby.chat) addChat(msg);
  loadEmotes();
  pickTab('chat');
  renderPerms();
  renderQueue();
  renderHistory();
  renderPeople();
  renderSettings();
  onState();
}

function pickTab(id) {
  for (const [k, b] of Object.entries(els.tabs)) {
    b.classList.toggle('on', k === id);
    $('#tab-' + k).hidden = k !== id;
  }
}

function renderPerms() {
  if (!lobby) return;
  const dis = (el, perm) => { el.disabled = !can(perm); };
  dis(els.pause, P.PAUSE); dis(els.back, P.SEEK); dis(els.fwd, P.SEEK); dis(els.skip, P.SKIP); dis(els.prev, P.SKIP);
  dis(els.loop, P.PLAYBACK); dis(els.speed, P.PLAYBACK); dis(els.clearQ, P.QUEUE);
  const [inp, playBtn, queueBtn] = els.add.children;
  playBtn.disabled = !can(P.LINK);
  queueBtn.disabled = !can(P.QUEUE);
  inp.disabled = !can(P.LINK) && !can(P.QUEUE);
  inp.placeholder = 'Link';
  els.tabs.settings.hidden = !can(P.ADMIN);
  renderChatFoot();
  renderQueue();
  renderHistory();
  onState();
}

// ---- chat: emotes (BTTV / 7TV from /api/emotes), GIFs, slow mode, moderation
let emotes = {}, emoteKey = null;
function emoteChannel() {
  if (lobby.emotes) return lobby.emotes;
  const v = st && st.url ? parseVideo(st.url) : null;
  return v && v.kind === 'twc' ? v.channel.toLowerCase() : '';   // the Twitch channel playing
}
async function loadEmotes() {
  if (!lobby) return;
  const key = emoteChannel();
  if (key === emoteKey) return;
  emoteKey = key;
  try {
    const r = await fetch('/api/emotes' + (key ? '?c=' + encodeURIComponent(key) : ''));
    const d = await r.json();
    if (emoteKey !== key) return;
    emotes = d.emotes || {};
    rerenderChat();
    if (emotePick) emotePick.refresh();
  } catch { /* chat still works without emotes */ }
}
function richText(text, set = emotes) {
  return String(text).split(/(\s+)/).map((w) => {
    const e = set[w];
    return e ? h('img', { class: 'emote', src: e.u, alt: w, title: `${w} (${e.s})`, loading: 'lazy' }) : w;
  });
}
// ---- @mentions: "@Name" of someone in the lobby (names may have spaces: the longest name that fits wins)
// players in game who aren't signed in to cstv.xyz are guests here, but they read the lobby chat: they can be @'d too
function gameGuests() {
  return ((lobby && lobby.guest_list) || []).filter((g) => g.game && g.name && g.name !== 'guest')
    .map((g) => ({ name: g.name, steamid: 'g' + g.id, online: true }));
}
function mentionNames() {
  if (!lobby) return [];
  return [...new Set(lobby.members.map((p) => p.name).concat(gameGuests().map((g) => g.name)).filter(Boolean))].sort((a, b) => b.length - a.length);
}
const myName = () => (me && me.steamid ? (me.name || '').toLowerCase() : '');
function splitMentions(text) {
  text = String(text);
  const names = mentionNames(), low = text.toLowerCase(), out = [];
  if (!names.length || !text.includes('@')) return [{ t: text }];
  let buf = '';
  for (let i = 0; i < text.length;) {
    if (text[i] === '@' && (i === 0 || /\s/.test(text[i - 1]))) {
      const n = names.find((x) => low.startsWith(x.toLowerCase(), i + 1) && !/\w/.test(text[i + 1 + x.length] || ''));
      if (n) {
        if (buf) out.push({ t: buf });
        buf = '';
        out.push({ at: text.slice(i, i + 1 + n.length), name: n.toLowerCase() });
        i += 1 + n.length;
        continue;
      }
    }
    buf += text[i++];
  }
  if (buf) out.push({ t: buf });
  return out;
}
function mentionsMe(msg) {
  const mine = myName();
  return !!mine && !msg.sys && msg.steamid !== me.steamid && splitMentions(msg.text || '').some((p) => p.name === mine);
}
function memberPerms(steamid) {
  const p = lobby.members.find((x) => x.steamid === steamid);
  return p ? p.perms : 0;
}
function modTools(msg) {
  if (!can(P.ADMIN) || msg.sys || !msg.steamid) return null;
  const mine = lobby.you.perms, theirs = memberPerms(msg.steamid);
  const isOwner = (theirs & P.OWNER) !== 0, self = msg.steamid === lobby.you.steamid;
  const canDelete = !isOwner || (mine & P.OWNER);
  const canPunish = !self && !isOwner && (!(theirs & P.ADMIN) || (mine & P.OWNER));
  if (!canDelete && !canPunish) return null;
  return h('span', { class: 'mod' },
    canDelete ? h('button', { title: 'Delete this message', onclick: () => send({ t: 'admin', action: 'delete_msg', id: msg.id }) }, icon(ICONS.trash)) : null,
    canPunish ? h('button', { title: 'Time out for 10 min (removes their messages)', onclick: () => send({ t: 'admin', action: 'mute', steamid: msg.steamid, minutes: 10 }) }, icon(ICONS.clock)) : null,
    canPunish ? h('button', { class: 'danger', title: 'Ban from this lobby', onclick: () => send({ t: 'admin', action: 'ban', steamid: msg.steamid }) }, icon(ICONS.ban)) : null);
}
function chatEl(msg) {
  if (msg.sys) return h('div', { class: 'sys', 'data-id': msg.id }, msg.text);
  const mine = myName();
  return h('div', { class: 'm' + (mentionsMe(msg) ? ' mention' : ''), 'data-id': msg.id }, modTools(msg),
    msg.from === 'game' ? h('span', { class: 'from', title: 'from the game' }, 'GAME') : null,
    h('span', { class: 'n', style: 'color:' + nameColor(msg.steamid) }, msg.name),
    splitMentions(msg.text || '').flatMap((p) => (p.at ? [h('span', { class: 'at' + (p.name === mine ? ' you' : '') }, p.at)] : richText(p.t))),
    msg.gif ? h('img', { class: 'gif', src: msg.gif.url, alt: msg.gif.title || 'GIF', title: msg.gif.title || '', width: msg.gif.w || null, height: msg.gif.h || null, loading: 'lazy' }) : null);
}
let titleFlash = null;
function flashTitle() {   // a mention while the tab is in the background
  if (!document.hidden || titleFlash) return;
  const base = document.title;
  document.title = '@ ' + base;
  titleFlash = () => { if (document.title === '@ ' + base) document.title = base; titleFlash = null; };
  document.addEventListener('visibilitychange', () => titleFlash && titleFlash(), { once: true });
}
function addChat(msg) {
  const log = els.chatLog;
  if (!log) return;
  if (mentionsMe(msg)) flashTitle();
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 60;
  log.append(chatEl(msg));
  while (log.children.length > 200) log.firstChild.remove();
  if (atBottom) {
    log.scrollTop = log.scrollHeight;
    const img = log.lastChild.querySelector && log.lastChild.querySelector('img.gif');
    if (img) img.addEventListener('load', () => { log.scrollTop = log.scrollHeight; }, { once: true });
  }
}
function rerenderChat() {
  if (!els.chatLog) return;
  put(els.chatLog, lobby.chat.map(chatEl));
  els.chatLog.scrollTop = els.chatLog.scrollHeight;
}

let lastSent = 0, slowTimer = null, gifPick = null;
function renderChatFoot() {
  if (!els.chatFoot) return;
  closePickers();
  if (!(me && me.steamid)) {   // the header's Steam button signs in
    put(els.chatFoot, h('div', { class: 'chat-form' },
      h('input', { disabled: true, placeholder: 'Sign in through Steam to chat', 'aria-label': 'Message' }), h('button', { class: 'primary', disabled: true }, 'Send')));
    return;
  }
  const input = h('input', { maxlength: 300, placeholder: 'Message', 'aria-label': 'Message' });
  const sendBtn = h('button', { class: 'primary' }, 'Send');
  const mention = mentionPicker(input);
  const slowed = lobby.slow && !can(P.ADMIN);
  const tickSlow = () => {
    const left = slowed ? Math.ceil(lastSent / 1000 + lobby.slow - Date.now() / 1000) : 0;
    sendBtn.disabled = left > 0;
    sendBtn.textContent = left > 0 ? left + 's' : 'Send';
    if (left > 0) { clearTimeout(slowTimer); slowTimer = setTimeout(tickSlow, 250); }
  };
  const gifOK = me.gifs && lobby.gifs;
  put(els.chatFoot,
    lobby.slow ? h('div', { class: 'slow-note' }, `Slow mode ${lobby.slow}s`) : null,
    h('form', { class: 'chat-form', onsubmit: (e) => {
      e.preventDefault();
      if (!input.value.trim() || sendBtn.disabled) return;
      send({ t: 'chat', text: input.value });
      input.value = '';
      lastSent = Date.now();
      tickSlow();
    } },
    h('button', { type: 'button', class: 'pickbtn', title: 'Emotes', 'aria-label': 'Emotes', onclick: () => (emotePick ? emotePick.close() : openEmotes(input)) }, icon(ICONS.smile)),
    gifOK ? h('button', { type: 'button', class: 'pickbtn gifbtn', title: 'Send a GIF (GIPHY)', onclick: () => (gifPick ? gifPick.close() : openGifs()) }, 'GIF') : null,
    input, sendBtn),
    mention.box);
  tickSlow();
}
// Typing "@" + the start of a name lists the lobby's people: arrows to pick, Tab / Enter / click to put it in.
function mentionPicker(input) {
  const box = h('div', { class: 'mention-list', role: 'listbox', hidden: true });
  let items = [], sel = 0, start = -1;
  const close = () => { box.hidden = true; items = []; start = -1; };
  const pick = (i) => {
    const p = items[i];
    if (!p) return;
    const pos = input.selectionStart ?? input.value.length;
    const add = '@' + p.name + ' ';
    input.value = (input.value.slice(0, start) + add + input.value.slice(pos)).slice(0, 300);
    const at = Math.min(start + add.length, input.value.length);
    input.setSelectionRange(at, at);
    close();
    input.focus();
  };
  const draw = () => put(box, items.map((p, i) => h('button', { type: 'button', class: (i === sel ? 'on' : '') + (p.online ? '' : ' off'), role: 'option',
    onmousedown: (e) => { e.preventDefault(); pick(i); } }, h('span', { class: 'n', style: 'color:' + nameColor(p.steamid) }, p.name))));
  const update = () => {
    const pos = input.selectionStart ?? input.value.length;
    const m = /(^|\s)@([^@\n]{0,32})$/.exec(input.value.slice(0, pos));
    if (!m || !lobby) { close(); return; }
    const q = m[2].toLowerCase();
    const found = lobby.members.concat(gameGuests()).filter((p) => p.name && p.steamid !== (me && me.steamid) && p.name.toLowerCase().startsWith(q))
      .sort((a, b) => b.online - a.online || a.name.localeCompare(b.name)).slice(0, 8);
    if (!found.length) { close(); return; }
    if (start !== pos - m[2].length - 1) sel = 0;
    start = pos - m[2].length - 1;
    items = found;
    sel = Math.min(sel, items.length - 1);
    draw();
    box.hidden = false;
  };
  input.addEventListener('input', update);
  input.addEventListener('click', update);
  input.addEventListener('blur', () => setTimeout(close, 100));
  input.addEventListener('keydown', (e) => {
    if (box.hidden) return;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      sel = (sel + (e.key === 'ArrowDown' ? 1 : items.length - 1)) % items.length;
      draw();
    } else if (e.key === 'Tab' || e.key === 'Enter') {
      e.preventDefault();
      pick(sel);
    } else if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close();
    }
  });
  return { box, close };
}
function closePickers() {
  if (gifPick) gifPick.close();
  if (emotePick) emotePick.close();
}
function openGifs() {
  closePickers();
  const input = h('input', { placeholder: 'Search', 'aria-label': 'Search GIFs' });
  const cols = [h('div', { class: 'gifcol' }), h('div', { class: 'gifcol' })];
  const grid = h('div', { class: 'gifgrid' }, cols);
  const tall = [0, 0];       // each column's height in widths, so the next GIF goes in the shorter one
  let q = '', next = 0, waiting = false, seen = new Set(), timer;
  const ask = () => { waiting = true; send({ t: 'gif_search', q, offset: next }); };
  const search = (text) => { q = text; next = 0; ask(); };
  const nearEnd = () => grid.scrollTop + grid.clientHeight > grid.scrollHeight - 400;
  input.addEventListener('input', () => { clearTimeout(timer); timer = setTimeout(() => search(input.value.trim()), 350); });
  input.addEventListener('keydown', (e) => { if (e.key === 'Escape') gifPick.close(); });
  grid.addEventListener('scroll', () => { if (next != null && !waiting && nearEnd()) ask(); }, { passive: true });
  const panel = h('div', { class: 'gifpick' },
    h('div', { class: 'row' }, input, h('button', { type: 'button', class: 'ghost', title: 'Close', onclick: () => gifPick.close() }, '✕')),
    grid, h('div', { class: 'giphy-attr' }, 'Powered by GIPHY'));
  els.chatFoot.append(panel);
  const pick = (it) => {
    const now = Date.now();
    if (lobby.slow && !can(P.ADMIN) && now - lastSent < lobby.slow * 1000) { toast('Slow mode: wait a moment', true); return; }
    send({ t: 'chat', gif: it.id });
    lastSent = now;
    gifPick.close();
    renderChatFoot();
  };
  gifPick = {
    close() { panel.remove(); gifPick = null; },
    show(m) {
      if (m.q !== q || (m.offset || 0) !== next) return;   // an older search or page
      waiting = false;
      if (!m.offset) { cols.forEach((c) => put(c)); tall[0] = tall[1] = 0; seen = new Set(); grid.scrollTop = 0; }
      for (const it of m.items) {
        if (seen.has(it.id)) continue;   // GIPHY's pages overlap now and then
        seen.add(it.id);
        const i = tall[0] <= tall[1] ? 0 : 1;
        tall[i] += it.preview.h / Math.max(1, it.preview.w);
        cols[i].append(h('button', { type: 'button', title: it.title, onclick: () => pick(it) },
          h('img', { src: it.preview.url, alt: it.title, width: it.preview.w, height: it.preview.h, loading: 'lazy' })));
      }
      if (!m.offset && !m.items.length) put(cols[0], h('p', { class: 'slow-note' }, 'None'));
      next = m.next ?? null;
      if (next != null && nearEnd()) ask();   // the page didn't fill the box
    },
  };
  search('');   // trending
  input.focus();
}

// ---- emote picker: the emotes chat shows (the lobby channel's 7TV / BTTV first, then the global ones);
// a click types the name into the message
let emotePick = null;
function emoteGroups(q) {
  const groups = new Map();
  for (const [name, e] of Object.entries(emotes)) {
    if (q && !name.toLowerCase().includes(q)) continue;
    if (!groups.has(e.s)) groups.set(e.s, []);
    groups.get(e.s).push(name);
  }
  return [...groups].sort((a, b) => b[0].includes(' ') - a[0].includes(' '));   // "7TV saya" before "7TV"
}
function insertEmote(input, name) {
  const v = input.value, a = input.selectionStart ?? v.length, b = input.selectionEnd ?? a;
  const add = (a && !/\s$/.test(v.slice(0, a)) ? ' ' : '') + name + ' ';
  input.value = (v.slice(0, a) + add + v.slice(b)).slice(0, 300);
  const at = Math.min(a + add.length, input.value.length);
  input.focus();
  input.setSelectionRange(at, at);
}
function openEmotes(input) {
  closePickers();
  const search = h('input', { placeholder: 'Search', 'aria-label': 'Search emotes' });
  const grid = h('div', { class: 'emotegrid' });
  const fill = () => {
    const groups = emoteGroups(search.value.trim().toLowerCase());
    put(grid, groups.length ? groups.map(([src, names]) => h('section', {},
      h('span', { class: 'label' }, src),
      h('div', { class: 'emotes' }, names.map((n) => h('button', { type: 'button', title: n, onclick: () => insertEmote(input, n) },
        h('img', { src: emotes[n].u.replace(/\/2x(\.webp)?$/, '/1x$1'), alt: n, loading: 'lazy' }))))))   // the small size: hundreds at once
      : h('p', { class: 'slow-note' }, 'None'));
  };
  search.addEventListener('input', fill);
  search.addEventListener('keydown', (e) => { if (e.key === 'Escape') { emotePick.close(); input.focus(); } });
  const panel = h('div', { class: 'gifpick' },
    h('div', { class: 'row' }, search, h('button', { type: 'button', class: 'ghost', title: 'Close', onclick: () => emotePick.close() }, '✕')),
    grid);
  els.chatFoot.append(panel);
  emotePick = { close() { panel.remove(); emotePick = null; }, refresh: fill };
  fill();
  search.focus();
}

function renderQueue() {
  if (!lobby || !els.queue) return;
  const q = lobby.queue;
  els.clearQ.hidden = !q.length || !can(P.QUEUE);
  els.queueCount.textContent = q.length || '';
  put(els.queue, ...(q.length ? q.map((it, i) => h('tr', {},
    h('td', { class: 'num m' }, String(i + 1)),
    h('td', { class: 'n', title: it.url }, it.title || it.url),
    h('td', { class: 'm hide-s' }, it.by || ''),
    h('td', { class: 'x' },
      can(P.LINK) ? h('button', { onclick: () => send({ t: 'queue_action', action: 'play', uid: it.uid }) }, 'Play') : null,
      can(P.QUEUE) && i ? h('button', { onclick: () => send({ t: 'queue_action', action: 'next', uid: it.uid }) }, 'Next') : null,
      can(P.QUEUE) ? h('button', { class: 'danger', title: 'Remove', onclick: () => send({ t: 'queue_action', action: 'remove', uid: it.uid }) }, '✕') : null)))
    : [h('tr', { class: 'empty' }, h('td', { colspan: 4 }, 'Empty'))]));
}

// ---- history: what played in this lobby (newest first)
function ago(t) {
  const s = Math.max(0, Date.now() / 1000 - t);
  return s < 60 ? 'now' : s < 3600 ? Math.floor(s / 60) + 'm' : s < 86400 ? Math.floor(s / 3600) + 'h' : Math.floor(s / 86400) + 'd';
}
function renderHistory() {
  if (!lobby || !els.history) return;
  const items = lobby.history || [];
  put(els.history, h('table', { class: 'list' }, h('tbody', {}, items.length
    ? items.map((it) => h('tr', {},
        h('td', { class: 'n', title: it.url }, h('span', { class: 't' }, it.title || it.url),
          h('span', { class: 'sub' }, [it.by, it.at ? ago(it.at) : null].filter(Boolean).join('  '))),
        h('td', { class: 'x' },
          can(P.LINK) ? h('button', { onclick: () => send({ t: 'link', url: it.url }) }, 'Play') : null,
          can(P.QUEUE) ? h('button', { onclick: () => send({ t: 'queue', url: it.url }) }, 'Queue') : null)))
    : h('tr', { class: 'empty' }, h('td', {}, 'Empty')))));
}
function personPanel(p) {
  const mine = lobby.you.perms;
  const own = p.own;   // null = lobby default
  const useDefault = h('input', { type: 'checkbox', checked: own == null,
    onchange: (e) => send({ t: 'admin', action: 'perms', steamid: p.steamid, perms: e.target.checked ? null : lobby.default_perms }) });
  const isAdmin = (p.perms & P.ADMIN) !== 0;
  const boxes = PERM_NAMES.map(([bit, label]) => h('label', {},
    h('input', { type: 'checkbox', checked: (p.perms & bit) !== 0, disabled: own == null || isAdmin,
      onchange: (e) => send({ t: 'admin', action: 'perms', steamid: p.steamid, perms: e.target.checked ? (own | bit) : (own & ~bit) }) }), label));
  return h('div', { class: 'panel' },
    isAdmin ? null : h('label', { class: 'check' }, useDefault, 'Default'),
    isAdmin ? null : h('div', { class: 'perms' }, boxes),
    mine & P.OWNER ? h('div', { class: 'row' },
      h('button', { onclick: () => send({ t: 'admin', action: 'admin', steamid: p.steamid, on: !isAdmin }) }, isAdmin ? 'Remove admin' : 'Make admin')) : null,
    p.muted
      ? h('div', { class: 'timeouts' }, h('span', {}, 'Timed out'), h('button', { onclick: () => send({ t: 'admin', action: 'mute', steamid: p.steamid, minutes: 0 }) }, 'Lift timeout'))
      : h('div', { class: 'timeouts' }, h('span', {}, 'Timeout'), [[1, '1m'], [10, '10m'], [60, '1h'], [1440, '24h']].map(([mins, label]) =>
          h('button', { title: `No chat for ${label}; removes their messages`, onclick: () => send({ t: 'admin', action: 'mute', steamid: p.steamid, minutes: mins }) }, label))),
    h('div', { class: 'row' },
      p.online ? h('button', { onclick: () => send({ t: 'admin', action: 'kick', steamid: p.steamid }) }, 'Kick') : null,
      h('button', { class: 'danger', onclick: () => send({ t: 'admin', action: 'ban', steamid: p.steamid }) }, 'Ban')));
}

let openPerson = null;
function renderPeople() {
  if (!lobby || !els.people) return;
  const mine = lobby.you.perms;
  const online = lobby.members.filter((p) => p.online).length + lobby.guests;
  els.peopleCount.textContent = online ? String(online) : '';
  const rows = lobby.members.map((p) => {
    const manage = !!(mine & P.ADMIN) && !(p.perms & P.OWNER) && p.steamid !== lobby.you.steamid && (!(p.perms & P.ADMIN) || !!(mine & P.OWNER));
    const tags = h('span', { class: 'tags' },
      p.perms & P.OWNER ? h('span', { class: 'badge held' }, 'Owner') : p.perms & P.ADMIN ? h('span', { class: 'badge' }, 'Admin') : null,
      p.muted ? h('span', { class: 'badge live' }, 'Timed out') : null,
      p.game ? h('span', { title: p.where ? 'In game: ' + p.where : 'In game' }, icon(ICONS.game)) : null,
      p.web ? h('span', { title: 'Website' }, icon(ICONS.web)) : null);
    return h('div', { class: 'person' + (p.online ? '' : ' off') + (manage ? ' manage' : '') },
      h('div', { class: 'head', onclick: manage ? () => { openPerson = openPerson === p.steamid ? null : p.steamid; renderPeople(); } : null },
        h('span', { class: 'dot' + (p.online ? ' on' : '') }),
        p.avatar ? h('img', { src: p.avatar, alt: '' }) : h('span', { class: 'noav' }),
        h('span', { class: 'nm', title: p.where ? p.steamid + ' · in game on ' + p.where : p.steamid }, p.name + (p.steamid === lobby.you.steamid ? ' (you)' : '')), tags),
      manage && openPerson === p.steamid ? personPanel(p) : null);
  });
  put(els.people, ...rows,
    // guests (not signed in) watch only: listed for everyone, kick / ban for admins
    lobby.guests ? h('div', { class: 'guests' }, h('span', { class: 'label' }, `Guests ${lobby.guests}`),
      (lobby.guest_list || []).map((g) => h('div', { class: 'row' },
        h('span', { style: 'flex:1' }, g.game ? icon(ICONS.game) : icon(ICONS.web), ' ', g.name),
        mine & P.ADMIN ? h('button', { onclick: () => send({ t: 'admin', action: 'kick', conn: g.id }) }, 'Kick') : null,
        mine & P.ADMIN ? h('button', { class: 'danger', onclick: () => send({ t: 'admin', action: 'ban', conn: g.id }) }, 'Ban') : null))) : null,
    mine & P.ADMIN && lobby.banned.length ? h('div', { class: 'guests' }, h('span', { class: 'label' }, 'Banned'),
      lobby.banned.map((b) => h('div', { class: 'row' }, h('span', { style: 'flex:1' }, b.name),
        h('button', { onclick: () => send({ t: 'admin', action: 'unban', steamid: b.steamid }) }, 'Unban')))) : null);
}

let deleteArmed = false;
function renderSettings() {
  if (!lobby || !els.settings || !can(P.ADMIN)) return;
  const name = h('input', { value: lobby.name, maxlength: 40 });
  const emoteInput = h('input', { value: lobby.emotes || '', maxlength: 25, placeholder: 'Twitch channel' });
  const pw = h('input', { type: 'text', value: (lobby.you && lobby.you.password) || '', maxlength: 64, placeholder: 'None', autocomplete: 'off' });
  const sites = h('select', { onchange: (e) => send({ t: 'admin', action: 'settings', sites: e.target.value }) },
    h('option', { value: 'any', selected: lobby.sites === 'any' }, 'Any site'),
    h('option', { value: 'yt_twitch', selected: lobby.sites === 'yt_twitch' }, 'Only YouTube + Twitch'));
  const def = PERM_NAMES.map(([bit, label]) => h('label', {}, h('input', { type: 'checkbox', checked: (lobby.default_perms & bit) !== 0,
    onchange: (e) => send({ t: 'admin', action: 'default_perms', perms: e.target.checked ? lobby.default_perms | bit : lobby.default_perms & ~bit }) }), label));
  const del = h('button', { class: 'danger', onclick: () => {
    if (!deleteArmed) { deleteArmed = true; del.textContent = 'Confirm delete'; setTimeout(() => { deleteArmed = false; renderSettings(); }, 4000); return; }
    send({ t: 'delete', code: lobby.code });
  } }, 'Delete lobby');
  const field = (label, ...kids) => h('div', { class: 'f' }, h('span', { class: 'label' }, label), ...kids);
  put(els.settings,
    field('Name', h('form', { class: 'row', onsubmit: (e) => { e.preventDefault(); send({ t: 'admin', action: 'settings', name: name.value }); } },
      name, h('button', {}, 'Save'))),
    field('Password', h('form', { class: 'row', onsubmit: (e) => { e.preventDefault(); send({ t: 'admin', action: 'settings', password: pw.value }); } },
      pw, h('button', {}, 'Save'), lobby.locked ? h('button', { type: 'button', class: 'danger', onclick: () => send({ t: 'admin', action: 'settings', password: '' }) }, 'Remove') : null)),
    field('Allowed sites', sites),
    field('Slow mode', h('select', { onchange: (e) => send({ t: 'admin', action: 'settings', slow: +e.target.value }) },
      [[0, 'Off'], [3, '3s'], [5, '5s'], [10, '10s'], [30, '30s'], [60, '1m'], [120, '2m']].map(([v, l]) => h('option', { value: v, selected: lobby.slow === v }, l)))),
    me.gifs ? field('GIFs', h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: !!lobby.gifs,
      onchange: (e) => send({ t: 'admin', action: 'settings', gifs: e.target.checked }) }), 'On')) : null,
    field('Emotes channel', h('form', { class: 'row', onsubmit: (e) => { e.preventDefault(); send({ t: 'admin', action: 'settings', emotes: emoteInput.value }); } },
      emoteInput, h('button', {}, 'Save'))),
    field('Everyone can', h('div', { class: 'perms' }, def)),
    lobby.you.perms & P.OWNER ? field('Lobby', h('div', {}, del)) : null);
}

// ---------------------------------------------------------------- quality (each viewer's own)
// 1440p and up are VP9 / AV1 only on YouTube (1080p is too in browsers that can play those)
const QUALITIES = [['1440', '1440p'], ['1080', '1080p'], ['720', '720p'], ['480', '480p'], ['360', '360p'], ['auto', 'Auto']];
function quality() {
  const q = store('cstv_quality');
  return QUALITIES.some(([v]) => v === q) ? q : '1440';
}
// YouTube's embed has no quality setting any more: it picks one for the size it's drawn at (in device
// pixels). So it's drawn at the picked quality's size and scaled to fit the box. EMBED_CROP: its title bar
// and bottom bar, cut off above and below the box (in the embed's own pixels, whatever the scale).
const EMBED_CROP = 72;
function fitEmbed(p = player) {
  if (!p || p.kind !== 'yt' || !p.frame || !els.player) return;
  const bw = els.player.clientWidth;
  if (!bw) return;
  const q = quality();
  const vw = q === 'auto' ? bw : (+q * 16 / 9) / (window.devicePixelRatio || 1);
  Object.assign(p.frame.style, { left: '0', top: '0', width: vw + 'px', height: vw * 9 / 16 + 2 * EMBED_CROP + 'px',
    transformOrigin: '0 0', transform: `scale(${bw / vw}) translateY(${-EMBED_CROP}px)` });
  els.player.style.setProperty('--yt-k', bw / vw);   // the embed's pixels -> ours (sizes the centre blur)
}
function applyQuality(p = player) {
  if (!p) return;
  if (p.kind === 'yt') fitEmbed(p);
  else if (p.setQuality) { p.qualityUntil = performance.now() + 30000; p.setQuality(); }
}
const YT_QUALITY = { highres: '4K+', hd2160: '2160p', hd1440: '1440p', hd1080: '1080p', hd720: '720p', large: '480p', medium: '360p', small: '240p', tiny: '144p' };
let boxWatch = null;   // refits the YouTube embed when the box changes size (window, full screen)

// ---------------------------------------------------------------- now playing
function expectedSec() {
  if (!st || !st.url) return 0;
  let p = st.pos;
  if (st.running && !st.paused) p += (hubNow() - st.at) * st.speed / 100;
  return Math.max(0, p / 1000);
}
let durRev = -1;
function reportDuration(p) {   // the hub moves on by itself when the clock passes the end, even with nobody watching
  const d = p.duration();
  if (d > 0 && !p.live && durRev !== st.rev) { durRev = st.rev; send({ t: 'duration', rev: st.rev, ms: Math.round(d * 1000) }); }
}
function siteLabel(v) { return v.kind === 'yt' ? 'YouTube' : v.kind === 'twc' || v.kind === 'twv' ? 'Twitch' : v.kind === 'stream' ? 'CS:TV' : 'in game only'; }

function onState() {
  if (!lobby || !els.title) return;
  const v = st.url ? parseVideo(st.url) : null;
  els.title.textContent = st.url ? (st.title || st.url) : 'Nothing playing';
  put(els.pause, icon(st.paused ? ICONS.play : ICONS.pause), st.paused ? 'Resume' : 'Pause');
  els.loop.classList.toggle('on', !!st.loop);
  els.speed.value = String(st.speed);
  ensurePlayer();
  updateMeta();
  updateXchat();
  loadEmotes();
}
function updateMeta() {
  if (!els.meta || !st) return;
  const v = st.url ? parseVideo(st.url) : null;
  const dur = player && player.ready ? player.duration() : 0;
  const parts = [];
  if (st.url) {
    parts.push(h('span', {}, fmtTime(dur ? Math.min(expectedSec(), dur) : expectedSec()) + (dur ? ' / ' + fmtTime(dur) : '')));
    parts.push(h('span', { class: 'badge' + (player && player.live ? ' live' : '') }, player && player.live ? 'Live' : siteLabel(v)));
    const qn = player && player.ready && player.qualityNow ? player.qualityNow() : '';
    // the video's best is below the pick: say so, or it looks like the pick didn't work
    const top = qn && player.maxQuality ? player.maxQuality() : '';
    const isMax = qn && qn === top && quality() !== 'auto' && parseInt(qn, 10) < +quality();
    if (qn) parts.push(h('span', { class: 'badge', title: isMax ? "This video's best quality" : 'Quality playing now' }, isMax ? qn + ' max' : qn));
    if (st.paused) parts.push(h('span', { class: 'badge' }, 'Paused'));
    if (!st.running) parts.push(h('span', { class: 'badge held' }, 'Waiting for viewers to load'));
    if (st.loop) parts.push(h('span', { class: 'badge' }, 'Looping'));
    if (st.speed !== 100) parts.push(h('span', { class: 'badge' }, st.speed / 100 + '×'));
  } else {
    parts.push(h('span', {}, ''));
  }
  if (st.url && st.by) parts.push(h('span', {}, 'added by ' + st.by));
  parts.push(h('span', { class: 'watching' }, `${st.viewers || 0} watching`));
  put(els.meta, ...parts);
}

// ---- seek bar: drag (or click) to a time, sent on release; hover shows the time. Live: none.
function makeSeek() {
  const fill = h('div', { class: 'fill' }), knob = h('div', { class: 'knob' }), tip = h('div', { class: 'tip', hidden: true });
  const el = h('div', { class: 'seek' }, h('div', { class: 'track' }, fill), knob, tip);
  let drag = null;   // where the knob is held (0-1)
  const dur = () => (player && player.ready && !player.live ? player.duration() : 0);
  const usable = () => can(P.SEEK) && dur() > 0;
  const at = (e) => { const r = el.getBoundingClientRect(); return Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)); };
  const showTip = (f) => { tip.hidden = false; tip.textContent = fmtTime(f * dur()); tip.style.left = f * 100 + '%'; };
  el.addEventListener('pointerdown', (e) => {
    if (e.button || !usable()) return;
    drag = at(e);
    el.setPointerCapture(e.pointerId);
    el.classList.add('drag');
    showTip(drag);
  });
  el.addEventListener('pointermove', (e) => {
    if (!dur()) return;
    if (drag != null) drag = at(e);
    showTip(drag ?? at(e));
  });
  el.addEventListener('pointerleave', () => { if (drag == null) tip.hidden = true; });
  const end = (commit) => {
    if (drag == null) return;
    const f = drag;
    drag = null;
    el.classList.remove('drag');
    tip.hidden = true;
    if (commit && dur()) send({ t: 'seek', ms: Math.round(f * dur() * 1000) });
  };
  el.addEventListener('pointerup', () => end(true));
  el.addEventListener('pointercancel', () => end(false));
  return {
    el,
    draw() {   // every animation frame, so it moves smoothly
      el.hidden = !st || !st.url || !!(player && player.live) || !!st.live;
      const d = dur(), f = drag ?? (d ? Math.min(1, expectedSec() / d) : 0);
      fill.style.transform = `scaleX(${f})`;
      knob.style.left = f * 100 + '%';
      el.classList.toggle('can', usable());
    },
  };
}
(function frame() {
  if (lobby && els.seek && els.seek.el.isConnected) els.seek.draw();
  requestAnimationFrame(frame);
})();

// ---- cinema (each viewer's own): the page around the picture goes dark; moving the mouse or typing brings it
// back, and it goes dark again after 5 s of neither. Esc or the button ends it.
const cinemaOn = () => store('cstv_cinema') === '1';
let cinemaDim = null, cinemaTimer = null, cinemaWoke = 0;
function applyCinema() {
  const on = cinemaOn() && document.body.dataset.view === 'lobby' && !!els.lobby && els.lobby.isConnected;
  document.body.classList.toggle('cinema', on);
  if (els.cinema) els.cinema.classList.toggle('on', on);
  if (on && !cinemaDim) document.body.append(cinemaDim = h('div', { class: 'cinema-dim', 'aria-hidden': 'true' }));
  if (!cinemaDim) return;
  clearTimeout(cinemaTimer);
  cinemaDim.classList.remove('dark');
  cinemaWoke = 0;
  if (on) cinemaWake();
}
function setCinema(on) { store('cstv_cinema', on ? '1' : '0'); applyCinema(); }
function cinemaWake() {
  if (!cinemaDim || !document.body.classList.contains('cinema')) return;
  const now = performance.now();
  if (now - cinemaWoke < 150 && !cinemaDim.classList.contains('dark')) return;   // mousemove fires a lot
  cinemaWoke = now;
  cinemaDim.classList.remove('dark');
  clearTimeout(cinemaTimer);
  cinemaTimer = setTimeout(() => { if (document.body.classList.contains('cinema')) cinemaDim.classList.add('dark'); }, 5000);
}
for (const ev of ['mousemove', 'pointerdown', 'keydown', 'wheel', 'touchstart', 'focusin', 'input'])
  document.addEventListener(ev, cinemaWake, { passive: true, capture: true });
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && document.body.classList.contains('cinema') && !e.target.closest('input, textarea, select, .modal-back')) setCinema(false);
});

// ---- the stream's own chat next to ours (each viewer's own): a Twitch channel's is Twitch's own chat (embedded, with
// all of Twitch's features, as whoever is signed in on twitch.tv in this browser), a YouTube live stream's comes
// through the hub (YouTube has no open chat feed; the hub reads it while the panel is open and says whether the
// video has a live chat at all).
const yt = { status: {}, v: null, items: [], queue: [], timer: null, log: null };   // the YouTube chat shown
function xchatSource() {
  const v = st && st.url ? parseVideo(st.url) : null;
  if (v && v.kind === 'twc') {
    const ch = v.channel.toLowerCase();
    return { key: 'tw:' + ch, label: 'Twitch chat', twitch: ch,
      frame: `https://www.twitch.tv/embed/${ch}/chat?parent=${location.hostname}&darkpopout` };
  }
  if (v && v.kind === 'yt' && yt.status[v.id] && yt.status[v.id].ok) return { key: 'yt:' + v.id, label: 'YouTube chat', yt: v.id };
  return null;
}
function updateXchat() {
  if (!els.xchat) return;
  const src = xchatSource(), on = !!src && store('cstv_xchat') === '1';
  els.xchatBtn.hidden = !src;
  const label = src ? src.label : '';
  if (els.xchatBtn.lastChild.textContent !== label) els.xchatBtn.lastChild.textContent = label;
  els.xchatBtn.classList.toggle('on', on);
  els.lobby.classList.toggle('xchat-on', on);
  els.xchat.hidden = !on;
  const head = on ? (src.twitch ? '#' + src.twitch : label) : '';
  if (els.xchatHead.textContent !== head) els.xchatHead.textContent = head;
  if (on && src.yt) ytStart(src.yt);
  else ytStop();
  els.xchatLog.hidden = !(on && src.yt);
  const frame = on && src.frame ? src.frame : '';
  els.xchatFrame.hidden = !frame;
  if (els.xchatFrame.dataset.src !== frame) {   // only another channel (or closing the panel) reloads Twitch's chat
    els.xchatFrame.dataset.src = frame;
    els.xchatFrame.src = frame || 'about:blank';
  }
  const tools = on ? src.key : '';
  if (els.xchat.dataset.tools !== tools) {
    els.xchat.dataset.tools = tools;
    put(els.xchatTools, !on ? null : src.yt
      ? h('button', { class: 'ghost', title: 'Open the chat on YouTube',
        onclick: () => window.open(`https://www.youtube.com/live_chat?is_popout=1&v=${src.yt}`, 'cstv_ytchat', 'popup,width=400,height=720') }, icon(ICONS.out))
      : h('button', { class: 'ghost', title: 'Open the chat on Twitch',
        onclick: () => window.open(`https://www.twitch.tv/popout/${src.twitch}/chat?popout=`, 'cstv_twchat', 'popup,width=400,height=720') }, icon(ICONS.out)));
  }
}

// ---- YouTube: the hub's reader; a batch is spread over ~1.5 s so the chat flows
function ytStatus(m) {
  yt.status[m.v] = m;
  if (yt.v === m.v && !m.ok && yt.log) ytSys(m.note || 'No live chat');
  updateXchat();
}
function ytStart(v) {
  if (yt.v === v && yt.log === els.xchatLog) return;
  if (yt.v !== v) {
    yt.v = v;
    yt.items = [];
    yt.queue = [];
    send({ t: 'xchat', on: true });
  }
  yt.log = els.xchatLog;
  put(yt.log, yt.items.map((x) => x.el));
  yt.log.scrollTop = yt.log.scrollHeight;
}
function ytStop() {
  if (!yt.v) return;
  yt.v = null;
  yt.items = [];
  yt.queue = [];
  clearTimeout(yt.timer);
  yt.timer = null;
  send({ t: 'xchat', on: false });
}
const YT_KIND = { o: '#ffd600', m: '#7fa0ff', s: '#4fd18b' };   // the streamer, moderators, members
function ytEl(x) {
  const color = YT_KIND[x.k] || nameColor(x.ch || x.n);
  return h('div', { class: 'm' + (x.p ? ' paid' : '') },
    x.p ? h('span', { class: 'from' }, x.p) : null,
    h('span', { class: 'n', style: 'color:' + color }, x.n),
    (x.r || []).map((r) => (r.length > 1 ? h('img', { class: 'emote', src: r[1], alt: r[0], title: r[0], loading: 'lazy' }) : r[0])));
}
function ytPush(x) {
  const log = yt.log;
  if (!log) return;
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 60;
  x.el = ytEl(x);
  yt.items.push(x);
  log.append(x.el);
  while (yt.items.length > 200) yt.items.shift().el.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
}
function ytSys(text) {
  if (!yt.log) return;
  const last = yt.items[yt.items.length - 1];
  if (last && last.sys === text) return;
  const x = { sys: text, el: h('div', { class: 'sys' }, text) };
  yt.items.push(x);
  yt.log.append(x.el);
  yt.log.scrollTop = yt.log.scrollHeight;
}
function ytAdd(m) {
  if (m.v !== yt.v) return;
  if (m.reset) {   // what came before the panel opened: at once
    yt.items = [];
    yt.queue = [];
    if (yt.log) put(yt.log);
    for (const x of m.items) ytPush(x);
    return;
  }
  yt.queue.push(...m.items);
  const drain = () => {
    yt.timer = null;
    const x = yt.queue.shift();
    if (!x || m.v !== yt.v) return;
    ytPush(x);
    if (yt.queue.length) yt.timer = setTimeout(drain, Math.max(15, Math.min(400, 1500 / (yt.queue.length + 1))));
  };
  if (!yt.timer) drain();
}
function ytRemove(v, which) {
  if (v !== yt.v) return;
  yt.queue = yt.queue.filter((x) => !which(x));
  yt.items = yt.items.filter((x) => { if (!x.sys && which(x)) { x.el.remove(); return false; } return true; });
}

// ---------------------------------------------------------------- players (YouTube / Twitch embeds)
function parseVideo(url) {
  if (/^rtmp:\/\//.test(url || '')) return { kind: 'stream', id: url.split('/').pop() };   // a player's CS:TV stream
  try {
    const u = new URL(url);
    const host = u.hostname.replace(/^(www|m|music)\./, '');
    if (host === 'youtu.be') return { kind: 'yt', id: u.pathname.slice(1, 12) };
    if (host === 'youtube.com') {
      if (u.pathname === '/watch' && u.searchParams.get('v')) return { kind: 'yt', id: u.searchParams.get('v').slice(0, 11) };
      const m = u.pathname.match(/^\/(shorts|live|embed)\/([\w-]{11})/);
      if (m) return { kind: 'yt', id: m[2] };
    }
    if (host === 'twitch.tv') {
      const p = u.pathname.split('/').filter(Boolean);
      if (p[0] === 'videos' && /^\d+$/.test(p[1] || '')) return { kind: 'twv', id: p[1] };
      if (p.length === 1 && /^\w+$/.test(p[0])) return { kind: 'twc', channel: p[0] };
    }
  } catch { /* not a URL */ }
  return { kind: 'other' };
}

let player = null;           // { key, kind, ready, live, time(), seek(), ... }
let soundOK = false;         // the browser lets us play sound (after a click)
let startedKey = '', endedRev = -1, lastSeek = 0;
// The hub holds its clock after a new link or a seek until a viewer has the video there: tell it once per (rev, seek).
function reportStarted() {
  const key = st.rev + ':' + st.seek;
  if (st && st.url && !st.running && startedKey !== key) { startedKey = key; send({ t: 'started', rev: st.rev, seek: st.seek }); }
}

function destroyPlayer() {
  if (player) { try { player.destroy(); } catch { /* gone */ } }
  player = null;
}
function playerMsg(title, sub) {
  put(els.player, h('div', { class: 'player-msg' }, h('b', {}, title), sub ? h('span', {}, sub) : null));
}
function setWatching(on) {
  if (setWatching.on === on) return;
  setWatching.on = on;
  send({ t: 'watching', on });
}

function ensurePlayer() {
  if (!els.player) return;
  const v = st && st.url ? parseVideo(st.url) : null;
  const key = v ? `${v.kind}:${v.id || v.channel || ''}:${st.rev}` : 'none:' + (st ? st.rev : 0);
  if (player && player.key === key) return;
  if (!player && els.player.dataset.key === key) return;
  destroyPlayer();
  els.player.dataset.key = key;
  if (!v) { playerMsg('Nothing playing'); setWatching(false); return; }
  if (v.kind === 'other') {
    playerMsg(st.title || 'Playing', 'In game only');
    setWatching(false);
    return;
  }
  put(els.player, );
  els.cover = null;
  const mk = v.kind === 'yt' ? makeYT : v.kind === 'stream' ? makeStream : makeTwitch;
  mk(v, key).then((p) => {
    if (els.player.dataset.key !== key) { p.destroy(); return; }
    player = p;
    applyVolume(p);
    setTimeout(() => applyVolume(p), 800);   // YouTube sometimes ignores a volume set right at "ready"
    setWatching(true);
  }).catch((e) => {
    playerMsg(v.kind === 'stream' ? 'Stream' : 'Player error', String(e.message || e));
    setWatching(false);
    if (v.kind === 'stream') retryPlayer(key, 5000);   // the streamer may still be starting
  });
}
// build the player again (same link) unless something else is playing by then
function retryPlayer(key, ms) {
  setTimeout(() => {
    if (els.player && els.player.dataset.key === key && (!player || player.key === key)) {
      destroyPlayer();
      delete els.player.dataset.key;
      ensurePlayer();
    }
  }, ms);
}

// ---- a player's CS:TV stream from the stream relay (mediamtx). WebRTC first: the offer / answer go over our
// WebSocket through the hub (WHEP), the video straight from the relay; live only, no clock to follow. Browsers
// whose WebRTC can't show H.264 (Vivaldi has no H.264 decoder of its own: data arrives, no picture) and places
// where WebRTC can't connect get low-latency HLS instead, through the hub (/hls/<token>/..., hls.js), ~1-2 s later.
let whepWait = null, hlsWait = null;
let streamHLS = new URLSearchParams(location.search).get('hls') === '1';   // ?hls=1, or once WebRTC didn't work here
function whepAnswer(m) {
  const w = whepWait;
  whepWait = null;
  if (w) (m.t === 'whep' ? w.ok(m.answer) : w.fail(new Error(m.msg || 'stream unavailable')));
}
function hlsAnswer(m) {
  const w = hlsWait;
  hlsWait = null;
  if (w) (m.t === 'hls' ? w.ok(m.url) : w.fail(new Error(m.msg || 'stream unavailable')));
}
function rtcH264() {
  try { return (RTCRtpReceiver.getCapabilities('video').codecs || []).some((c) => /h264/i.test(c.mimeType)); } catch { return false; }
}
function toHLS(key, why) {   // this browser: HLS from now on
  if (streamHLS) return;
  streamHLS = true;
  console.info('stream: HLS instead of WebRTC (' + why + ')');
  retryPlayer(key, 0);
}
async function makeStream(v, key) {
  if (streamHLS || !window.RTCPeerConnection || !rtcH264()) return makeHLS(v, key);
  try {
    return await makeRTC(v, key);
  } catch (e) {
    if (/isn't live/.test(e.message)) throw e;   // too early: the retry tries again
    streamHLS = true;
    return makeHLS(v, key);
  }
}
async function makeRTC(v, key) {
  const video = h('video', { class: 'stream-video', autoplay: true, playsInline: true, muted: true });
  els.player.append(video);
  addShield();
  const obj = { key, kind: 'stream', mode: 'webrtc', ready: false, live: true, fps: 0 };
  const pc = obj.pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });   // obj.pc: for looking at it in the console
  pc.addTransceiver('video', { direction: 'recvonly' });
  pc.addTransceiver('audio', { direction: 'recvonly' });
  const media = new MediaStream();
  pc.ontrack = (e) => { media.addTrack(e.track); video.srcObject = media; };
  let stats = 0;
  const cleanup = () => { clearInterval(stats); stopVStats(obj); try { pc.close(); } catch { /* closed */ } video.remove(); };
  try {
    await pc.setLocalDescription(await pc.createOffer());
    await new Promise((res) => {   // all our candidates in the offer (no trickle)
      if (pc.iceGatheringState === 'complete') { res(); return; }
      const t = setTimeout(res, 2000);
      pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); res(); } });
    });
    const answer = await new Promise((ok, fail) => {
      if (whepWait) whepWait.fail(new Error('replaced'));
      whepWait = { ok, fail };
      send({ t: 'whep', offer: pc.localDescription.sdp });
      setTimeout(() => { if (whepWait && whepWait.ok === ok) { whepWait = null; fail(new Error("the stream didn't answer")); } }, 10000);
    });
    await pc.setRemoteDescription({ type: 'answer', sdp: answer });
  } catch (e) {
    cleanup();
    throw e;
  }
  // HLS instead when it doesn't connect within 10 s, or bytes come in but nothing is decoded for 4 s (no H.264)
  const t0 = performance.now();
  let bytesSince = 0;
  stats = setInterval(async () => {
    if (player && player !== obj && player.key !== key) return;
    let r = null;
    try {
      for (const x of (await pc.getStats()).values()) if (x.type === 'inbound-rtp' && x.kind === 'video') r = x;
    } catch { return; }
    if (r && r.framesPerSecond) obj.fps = Math.round(r.framesPerSecond);
    const now = performance.now();
    if (r && r.bytesReceived > 0 && !r.framesDecoded) {
      bytesSince = bytesSince || now;
      if (now - bytesSince > 4000) toHLS(key, 'no frames decoded');
    } else if (!r || !r.bytesReceived) {
      if (pc.connectionState !== 'connected' && now - t0 > 10000) toHLS(key, 'no connection');
    }
  }, 1000);
  pc.addEventListener('connectionstatechange', () => {
    if (pc.connectionState === 'failed') toHLS(key, 'connection failed');
  });
  video.addEventListener('playing', onPlaying);
  startVStats(obj, async () => {
    let r = null, codec = '';
    const all = await pc.getStats();
    for (const x of all.values()) if (x.type === 'inbound-rtp' && x.kind === 'video') r = x;
    if (!r) return null;
    const cd = r.codecId && all.get(r.codecId);
    if (cd) codec = cd.mimeType.replace(/^video\//, '');
    return { codec, res: r.frameWidth ? r.frameWidth + 'x' + r.frameHeight : '', fps: r.framesPerSecond, fd: r.framesDecoded,
      fdrop: r.framesDropped, fz: r.freezeCount, fzd: r.totalFreezesDuration, lost: r.packetsLost,
      jit: r.jitter != null ? r.jitter * 1000 : null, bytes: r.bytesReceived };
  });
  return Object.assign(obj, {
    ready: true,
    time: () => video.currentTime,
    duration: () => 0,
    seek: () => {},
    play: () => { video.play().catch(() => {}); },
    pause: () => {},
    playing: () => !video.paused && video.readyState >= 2,
    buffering: () => video.readyState < 3,
    ended: () => false,
    rate: () => {},
    volume: (x) => { video.volume = x / 100; },
    mute: (m) => { video.muted = m; },
    qualityNow: () => (video.videoHeight ? video.videoHeight + 'p' + (obj.fps || '') : ''),
    destroy: cleanup,
  });
}
async function makeHLS(v, key) {
  await loadScript('/static/hls-1.7.3.min.js', () => window.Hls);
  const url = await new Promise((ok, fail) => {
    if (hlsWait) hlsWait.fail(new Error('replaced'));
    hlsWait = { ok, fail };
    send({ t: 'hls' });
    setTimeout(() => { if (hlsWait && hlsWait.ok === ok) { hlsWait = null; fail(new Error("the stream didn't answer")); } }, 10000);
  });
  const video = h('video', { class: 'stream-video', autoplay: true, playsInline: true, muted: true });
  els.player.append(video);
  addShield();
  const obj = { key, kind: 'stream', mode: 'hls', ready: false, live: true, fps: 0 };
  let hls = null, stalls = 0, loaded = 0;
  video.addEventListener('waiting', () => { stalls++; });
  if (window.Hls && Hls.isSupported()) {
    // low-latency HLS: ~1-2 s behind the game; the transmux worker isn't needed for fMP4 (and the page's CSP has no blob: workers)
    hls = obj.hls = new Hls({ lowLatencyMode: true, enableWorker: false, backBufferLength: 10, maxLiveSyncPlaybackRate: 1.15 });
    // low-latency HLS loads parts (each FRAG_LOADED of a part carries its own stats; the segment's grow with its parts)
    hls.on(Hls.Events.FRAG_LOADED, (_, d) => { const x = d.part || d.frag; loaded += (x && x.stats && x.stats.loaded) || 0; });
    hls.on(Hls.Events.ERROR, (_, d) => {
      if (!d.fatal) return;
      if (d.type === Hls.ErrorTypes.MEDIA_ERROR && !obj.recovered) { obj.recovered = true; hls.recoverMediaError(); return; }
      if (player === obj) retryPlayer(key, 3000);   // the stream ended or its link did: the hub knows what's on now
    });
    hls.loadSource(url);
    hls.attachMedia(video);
  } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
    video.src = url;   // Safari: HLS of its own
  } else {
    video.remove();
    throw new Error("this browser can't play the stream");
  }
  video.addEventListener('playing', onPlaying);
  let lastFrames = 0, lastAt = performance.now();
  startVStats(obj, async () => {
    const q = video.getVideoPlaybackQuality ? video.getVideoPlaybackQuality() : null;
    const now = performance.now(), frames = q ? q.totalVideoFrames : 0;
    obj.fps = Math.round(((frames - lastFrames) * 1000) / Math.max(1, now - lastAt));
    lastFrames = frames;
    lastAt = now;
    const lv = hls && hls.levels && hls.levels[hls.currentLevel];
    return { codec: lv ? lv.videoCodec || '' : 'native', res: video.videoWidth ? video.videoWidth + 'x' + video.videoHeight : '',
      fps: obj.fps, fd: frames, fdrop: q ? q.droppedVideoFrames : null, bytes: loaded, stalls,
      lat: hls && hls.latency != null ? hls.latency : null };
  });
  return Object.assign(obj, {
    ready: true,
    time: () => video.currentTime,
    duration: () => 0,
    seek: () => {},
    play: () => { video.play().catch(() => {}); },
    pause: () => {},
    playing: () => !video.paused && video.readyState >= 2,
    buffering: () => video.readyState < 3,
    ended: () => false,
    rate: () => {},
    volume: (x) => { video.volume = x / 100; },
    mute: (m) => { video.muted = m; },
    qualityNow: () => (video.videoHeight ? video.videoHeight + 'p' + (obj.fps || '') : ''),
    destroy: () => { stopVStats(obj); if (hls) hls.destroy(); video.remove(); },
  });
}
// every 10 s while a stream plays: how it goes, to the hub's log (where frame drops come from)
function uaBrief() {
  const d = navigator.userAgentData;
  if (d && d.brands) {
    const b = d.brands.filter((x) => !/not.?a.?brand/i.test(x.brand)).map((x) => x.brand + ' ' + x.version).join(', ');
    return (b + '; ' + (d.platform || '')).slice(0, 60);
  }
  const m = /(Firefox|Edg|OPR|Vivaldi|Chrome|Safari)\/([\d.]+)/.exec(navigator.userAgent);
  return (m ? m[1] + ' ' + m[2].split('.')[0] : navigator.userAgent.slice(0, 40)) + '; ' + (navigator.platform || '');
}
function startVStats(obj, read) {
  let lastBytes = null, lastAt = performance.now();
  obj.vstats = setInterval(async () => {
    if (player !== obj) return;
    let r = null;
    try { r = await read(); } catch { return; }
    if (!r) return;
    const now = performance.now();
    const kbps = lastBytes != null && r.bytes != null ? ((r.bytes - lastBytes) * 8) / Math.max(1, now - lastAt) : null;
    lastBytes = r.bytes;
    lastAt = now;
    send({ t: 'vstats', mode: obj.mode, codec: r.codec, ua: uaBrief(), res: r.res, fps: r.fps, fd: r.fd, fdrop: r.fdrop,
      fz: r.fz, fzd: r.fzd, lost: r.lost, jit: r.jit, kbps: kbps != null ? Math.round(kbps) : null, stalls: r.stalls });
  }, 10000);
}
function stopVStats(obj) { clearInterval(obj.vstats); }

function soundButton() {
  if (soundOK || !player) return;
  const b = h('button', { class: 'overlay-btn primary', onclick: () => { soundOK = true; b.remove(); applyVolume(); player && player.play(); } }, 'Sound on');
  els.player.append(b);
}
// p: the player to set (a new one isn't `player` yet while it gets ready: that used to reset the volume to 100)
function applyVolume(p = player) {
  if (!p || !p.ready || !els.vol) return;
  p.volume(+els.vol.value);
  p.mute(!soundOK || +els.vol.value === 0);
}
function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  else if (els.player) els.player.requestFullscreen().catch(() => {});
}
// Clicks on the video go nowhere: pausing and seeking happen with the lobby's buttons, for everyone.
function addShield() {
  blurHeld = false;
  clearTimeout(blurTimer);
  blurTimer = null;
  els.blur = h('div', { class: 'player-bezel', 'aria-hidden': 'true' }, icon(ICONS.pause));
  els.player.append(els.blur, h('div', { class: 'player-shield', title: 'Double-click for full screen', ondblclick: toggleFullscreen }));
}
let blurTimer = null, blurHeld = false;
function blurCentre() {
  if (!els.blur) return;
  if (!blurHeld) put(els.blur, icon(ICONS.pause));
  els.blur.classList.add('on');
  clearTimeout(blurTimer);
  blurTimer = setTimeout(() => { blurTimer = null; if (els.blur && !blurHeld) els.blur.classList.remove('on'); }, 4800);
}
// paused (after it played): the paused picture shows, our square with a play icon stays on YouTube's middle
function holdBezel(on) {
  if (!els.blur || blurHeld === on) return;
  blurHeld = on;
  put(els.blur, icon(on ? ICONS.play : ICONS.pause));
  if (on) els.blur.classList.add('on');
  else if (!blurTimer) els.blur.classList.remove('on');
}
// While the embed isn't playing (loading, paused, at the end) YouTube draws its title bar, buttons and
// suggestions over the picture. A cover with the video's thumbnail sits on top then (with a play icon when
// paused); it goes as soon as the video plays (picture and sound together). YouTube's new embed controls still
// put their pause button in the middle from the moment it buffers until ~4.3 s after it plays, after every
// start, resume and seek (measured 2026-10-19; controls=0 doesn't remove it, no setting does, and a
// cross-origin iframe can't be styled): our own square with a pause icon sits on that spot meanwhile
// (blurCentre, 4.8 s from the last buffering / playing). The iframe never gets the mouse (the shield), which
// would bring YouTube's controls back.
function addCover(v) {
  const label = h('span', { class: 'state' });
  const el = h('div', { class: 'player-cover' }, label);
  if (v.kind === 'yt') {
    const img = new Image();
    img.onload = () => {   // YouTube answers a missing maxres thumbnail with a 120x90 placeholder
      el.style.backgroundImage = `url(https://i.ytimg.com/vi/${v.id}/${img.naturalWidth > 120 ? 'maxresdefault' : 'hqdefault'}.jpg)`;
    };
    img.src = `https://i.ytimg.com/vi/${v.id}/maxresdefault.jpg`;
  }
  els.player.append(el);
  els.cover = { el, label, shown: '', playingSince: 0 };
  setCover('loading');
}
function setCover(state) {   // '' = hidden, 'loading' | 'paused' | 'ended'
  const c = els.cover;
  if (!c || c.shown === state) return;
  c.shown = state;
  c.el.classList.toggle('gone', !state);
  if (state === 'paused') put(c.label, icon(ICONS.play), 'Paused');
  else if (state === 'ended') put(c.label, 'Ended');
  else if (state === 'loading') put(c.label, h('span', { class: 'spin', 'aria-hidden': 'true' }), 'Loading');
}
function updateCover(p) {
  if (!els.cover) return;
  if (!p || !p.ready) { setCover('loading'); holdBezel(false); return; }
  if (st && st.paused && !p.live) {
    els.cover.playingSince = 0;
    // YouTube shows nothing over a paused picture once it has played (checked 2026-10-19, controls=0, no mouse):
    // the picture stays. Before its first play it has its big button and title up: the cover then.
    if (p.kind === 'yt' && p.played) { setCover(''); holdBezel(true); } else setCover('paused');
    return;
  }
  holdBezel(false);
  if (p.ended()) { els.cover.playingSince = 0; setCover('ended'); return; }
  if (p.playing()) {
    setCover('');
  } else if (!els.cover.shown) {
    // buffering mid-video: YouTube shows only a spinner there, keep the picture
  } else {
    els.cover.playingSince = 0;
  }
}
document.addEventListener('pointerdown', () => { if (!soundOK) { soundOK = true; applyVolume(); const b = $('.overlay-btn'); if (b) b.remove(); } }, { once: true });

async function makeYT(v, key) {
  await loadScript('https://www.youtube.com/iframe_api', () => window.YT && window.YT.Player);
  const id = 'yt' + Math.random().toString(36).slice(2);
  const holder = h('div', { class: 'embed', id });
  els.player.append(holder);
  addCover(v);
  addShield();
  return new Promise((resolve) => {
    const obj = { key, kind: 'yt', ready: false, live: false };
    const yt = new YT.Player(holder, {
      width: '100%', height: '100%', videoId: v.id,
      playerVars: { autoplay: 1, start: Math.floor(expectedSec()), playsinline: 1, rel: 0, origin: location.origin,
        controls: 0, disablekb: 1, fs: 0, iv_load_policy: 3 },
      events: {
        onReady: () => {
          obj.ready = true;
          const d = yt.getVideoData && yt.getVideoData();
          obj.live = !!(d && d.isLive);
          applyVolume(obj);
          yt.playVideo();
          soundButton();
          resolve(obj);
        },
        onStateChange: (e) => {
          if (e.data === YT.PlayerState.BUFFERING) blurCentre();
          if (e.data === YT.PlayerState.PLAYING) {
            obj.played = true;
            blurCentre();
            // auto-captions turn themselves on for muted autoplay; the site has no captions button
            if (!obj.noCaptions && yt.unloadModule) { obj.noCaptions = true; try { yt.unloadModule('captions'); yt.unloadModule('cc'); } catch { /* fine */ } }
            onPlaying();
          }
          if (e.data === YT.PlayerState.ENDED) onEnded();
        },
        onError: (e) => playerMsg('Can\'t play here (YouTube ' + e.data + ')', 'In game only'),
      },
    });
    obj.frame = document.getElementById(id);   // the iframe that took the holder's place: sized before the video loads
    fitEmbed(obj);
    Object.assign(obj, {
      time: () => yt.getCurrentTime() || 0,
      duration: () => (obj.live ? 0 : yt.getDuration() || 0),
      seek: (s) => { blurCentre(); yt.seekTo(s, true); },
      play: () => yt.playVideo(),
      pause: () => yt.pauseVideo(),
      playing: () => yt.getPlayerState() === YT.PlayerState.PLAYING,
      buffering: () => [YT.PlayerState.BUFFERING, -1, YT.PlayerState.CUED].includes(yt.getPlayerState()),
      ended: () => yt.getPlayerState() === YT.PlayerState.ENDED,
      rate: (r) => { if (yt.getPlaybackRate() !== r) yt.setPlaybackRate(r); },
      qualityNow: () => YT_QUALITY[yt.getPlaybackQuality && yt.getPlaybackQuality()] || '',
      maxQuality: () => YT_QUALITY[((yt.getAvailableQualityLevels && yt.getAvailableQualityLevels()) || []).find((q) => q !== 'auto')] || '',
      volume: (x) => yt.setVolume(x),
      mute: (m) => (m ? yt.mute() : yt.unMute()),
      destroy: () => yt.destroy(),
    });
  });
}

async function makeTwitch(v, key) {
  await loadScript('https://player.twitch.tv/js/embed/v1.js', () => window.Twitch && window.Twitch.Player);
  const id = 'tw' + Math.random().toString(36).slice(2);
  const holder = h('div', { class: 'embed', id });
  els.player.append(holder);
  addCover(v);
  addShield();
  const opts = { width: '100%', height: '100%', parent: [location.hostname], autoplay: true, muted: true, controls: false };
  if (v.kind === 'twc') opts.channel = v.channel;
  else { opts.video = v.id; const s = Math.floor(expectedSec()); opts.time = `${Math.floor(s / 3600)}h${Math.floor(s / 60) % 60}m${s % 60}s`; }
  const tw = new Twitch.Player(id, opts);
  // qualityUntil: Twitch lists its qualities a few seconds after READY, so the main loop keeps asking till then
  const obj = { key, kind: 'tw', ready: false, live: v.kind === 'twc', state: 'idle', qualityUntil: performance.now() + 30000, qualityAt: 0 };
  Object.assign(obj, {
    time: () => tw.getCurrentTime() || 0,
    duration: () => (obj.live ? 0 : tw.getDuration() || 0),
    seek: (s) => tw.seek(s),
    play: () => tw.play(),
    pause: () => tw.pause(),
    playing: () => obj.state === 'playing',
    buffering: () => obj.state === 'idle',
    ended: () => obj.state === 'ended',
    rate: () => {},
    qualityNow: () => {
      try {
        const g = tw.getQuality(), x = (tw.getQualities() || []).find((y) => y && y.group === g);
        return g === 'chunked' ? 'Source' : x && x.group !== 'auto' ? x.name : '';
      } catch { return ''; }
    },
    volume: (x) => tw.setVolume(x / 100),
    mute: (m) => tw.setMuted(m),
    destroy: () => { holder.remove(); },
    // the best at or under the picked height (Twitch lists source as the "chunked" group, with its height)
    setQuality: () => {
      let list = [];
      try { list = (tw.getQualities() || []).filter((x) => x && x.group && x.group !== 'auto'); } catch { return; }
      if (!list.length) return;
      const q = quality(), ht = (x) => x.height || parseInt(x.name, 10) || 0;
      let want = 'auto';
      if (q !== 'auto') {
        const fit = list.filter((x) => ht(x) <= +q);
        want = (fit.length ? fit.sort((a, b) => ht(b) - ht(a) || (b.framerate || 0) - (a.framerate || 0))
          : list.sort((a, b) => ht(a) - ht(b)))[0].group;
      }
      try { if (tw.getQuality() !== want) tw.setQuality(want); } catch { /* not ready */ }
    },
  });
  tw.addEventListener(Twitch.Player.PLAYING, () => {   // Twitch may go back to auto as a stream starts: set it again
    obj.state = 'playing';
    obj.setQuality();
    setTimeout(obj.setQuality, 3000);
    onPlaying();
  });
  tw.addEventListener(Twitch.Player.PAUSE, () => { obj.state = 'paused'; });
  tw.addEventListener(Twitch.Player.ENDED, () => { obj.state = 'ended'; onEnded(); });
  return new Promise((resolve) => {
    tw.addEventListener(Twitch.Player.READY, () => { obj.ready = true; applyVolume(obj); obj.setQuality(); soundButton(); resolve(obj); });
  });
}

function onPlaying() {
  updateCover(player);
  if (player && player.ready && !player.live && Math.abs(player.time() - expectedSec()) > 2) return;   // not there yet: the loop seeks
  if (st) reportStarted();
}
function onEnded() {
  if (st && st.url && endedRev !== st.rev) { endedRev = st.rev; send({ t: 'ended', rev: st.rev }); }
}

// Follow the hub's clock: the embeds can't change speed finely, so only seek when far off.
setInterval(() => {
  if (!lobby || !st) return;
  updateMeta();
  updateXchat();
  const p = player;
  updateCover(p);
  if (!p || !p.ready || !st.url) return;
  const t0 = performance.now();
  if (p.setQuality && t0 < p.qualityUntil && t0 - p.qualityAt > 2000) { p.qualityAt = t0; p.setQuality(); }
  if (p.live) {
    if (st.paused && p.playing()) p.pause();
    else if (!st.paused && !p.playing() && !p.buffering()) p.play();
    if (p.playing()) reportStarted();
    return;
  }
  reportDuration(p);
  const exp = expectedSec(), t = p.time(), now = performance.now();
  const dur = p.duration();
  if (dur > 0 && exp > dur + 1.5 && !st.paused && st.running) { onEnded(); return; }   // the clock is past the end
  if (st.paused) {
    if (p.playing()) p.pause();
    if (Math.abs(t - exp) > 0.6 && now - lastSeek > 1000) { p.seek(exp); lastSeek = now; }
    return;
  }
  if (p.ended()) return;
  p.rate(st.speed / 100);
  if (!p.playing() && !p.buffering()) p.play();
  if (!st.running) {   // the hub waits for a viewer to have the video at its position: get there, then say so
    if (Math.abs(t - exp) > 1.5 && now - lastSeek > 1500) { p.seek(exp); lastSeek = now; }
    else if (p.playing() && Math.abs(t - exp) <= 1.5) reportStarted();
    return;
  }
  const tol = p.kind === 'yt' ? 1.0 : 2.0;
  if (p.playing() && Math.abs(t - exp) > tol && now - lastSeek > 2500) { p.seek(exp); lastSeek = now; }
}, 500);

// ---------------------------------------------------------------- start
const startQ = new URLSearchParams(location.search);
if (startQ.get('login') === 'failed') {
  history.replaceState(null, '', location.pathname);
  setTimeout(() => toast("Steam sign-in didn't work. Try again.", true), 300);
}
if (startQ.get('twitch') === 'failed') {
  history.replaceState(null, '', location.pathname);
  setTimeout(() => toast("Twitch sign-in didn't work. Try again.", true), 300);
}
else connect();
