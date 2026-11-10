// Cross-browser smoke test for the cstv.xyz site: each engine x viewport opens the home page (and a lobby page),
// collects console errors, uncaught exceptions, failed requests and sideways scrolling, takes a full-page
// screenshot, and reports what each engine can play (WebRTC H.264 / MSE for hls.js) = which viewers fall back to HLS.
//
//   node run.mjs <base-url> [--lobby CODE] [--only chromium,firefox,webkit,chrome-sys] [--out results]
//   node run.mjs --local [--channel auto|<login>] [--only ...]   (starts local_hub.py: a throwaway hub + a lobby playing the top live Twitch channel)
//   node run.mjs --local --admin   (also the /admin page, signed in as a site admin, with sample visits)
//
// Engines: Playwright's chromium / firefox / webkit (~/.cache/ms-playwright; `npx playwright install chromium firefox webkit`)
// and chrome-sys = the system Chromium (/usr/sbin/chromium or $CHROMIUM_PATH), which has the real H.264 decoders like
// Chrome / Edge / Vivaldi; Playwright's own Chromium build doesn't. An engine that can't start is listed as skipped.
// Read-only on a live site: page loads as a guest, nothing posted. Each run is a fresh browser profile.
import { chromium, firefox, webkit, devices } from 'playwright';
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (name, dflt) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : dflt; };
const flag = name => argv.includes(name);
const VALUED = ['--lobby', '--only', '--out', '--channel'];   // options followed by a value
const positional = argv.filter((a, i) => !a.startsWith('--') && !VALUED.includes(argv[i - 1]));

const OUT = path.resolve(HERE, opt('--out', 'results'));
const IDLE_CAP_MS = 10000;
const SYS_CHROMIUM = process.env.CHROMIUM_PATH || ['/usr/sbin/chromium', '/usr/bin/chromium'].find(p => existsSync(p));

// third-party requests/console lines that are someone else's tracking/ads, not ours (listed apart, not counted)
const NOISE = /doubleclick|googlesyndication|google-analytics|googletagmanager|googleads|adservice|imasdk|amazon-adsystem|scorecardresearch|spade\.twitch|countess\.twitch|sentry|newrelic|nr-data|hotjar|branch\.io|cdn-cgi\/rum|pubads|youtube\.com\/(api\/stats|ptracking|generate_204|youtubei\/v1\/log_event|pagead)|play\.google\.com\/log|static\.doubleclick|jnn-pa\.googleapis|client-event-reporter|gql\.twitch\.tv.*integrity|k\.twitchcdn\.net|iabtechnologylab|omsdk|GraphQL errors were detected|getLayoutMap\(\)/i;

const ENGINES = {
  chromium: { type: chromium, phone: { name: 'Pixel 7', ctx: devices['Pixel 7'] } },
  firefox: { type: firefox, phone: { name: '390x844', ctx: { viewport: { width: 390, height: 844 } } } },   // no isMobile in Firefox
  webkit: { type: webkit, phone: { name: 'iPhone 13', ctx: devices['iPhone 13'] } },
  'chrome-sys': { type: chromium, launch: { executablePath: SYS_CHROMIUM }, phone: { name: 'Pixel 7', ctx: devices['Pixel 7'] }, needs: SYS_CHROMIUM },
};
const VIEWPORTS = eng => [
  { key: 'desktop', label: '1920x1080', ctx: { viewport: { width: 1920, height: 1080 } } },
  { key: 'laptop', label: '1366x768', ctx: { viewport: { width: 1366, height: 768 } } },
  { key: 'phone', label: eng.phone.name, ctx: eng.phone.ctx },
];

function startLocalHub() {
  const args = [path.join(HERE, 'local_hub.py')];
  if (opt('--channel')) args.push('--channel', opt('--channel'));
  if (flag('--admin')) args.push('--admin');
  const p = spawn('python3', args, { stdio: ['ignore', 'pipe', 'inherit'] });
  return new Promise((resolve, reject) => {
    let buf = '';
    const t = setTimeout(() => { p.kill('SIGTERM'); reject(new Error('local_hub.py: no READY in 30 s')); }, 30000);
    p.stdout.on('data', d => {
      buf += d;
      const m = buf.match(/^READY (\S+) (\S+)$/m);
      const a = buf.match(/^ADMIN (\S+)$/m);
      if (m) { clearTimeout(t); resolve({ proc: p, base: m[1], lobby: m[2], admin: a ? a[1] : null }); }
    });
    p.on('exit', code => { clearTimeout(t); reject(new Error(`local_hub.py exited (${code})`)); });
  });
}

const hostOf = u => { try { return new URL(u).host; } catch { return ''; } };
const short = (u, n = 140) => { try { const x = new URL(u); u = x.host + x.pathname + (x.search ? '?…' : ''); } catch {} return u.length > n ? u.slice(0, n) + '…' : u; };

// what the page can play, asked inside it (the site's own test: WebRTC without H.264 -> HLS via hls.js)
function mediaCaps() {
  const caps = window.RTCRtpReceiver && RTCRtpReceiver.getCapabilities ? RTCRtpReceiver.getCapabilities('video') : null;
  const codecs = caps ? [...new Set(caps.codecs.map(c => c.mimeType.replace(/^video\//i, '')))].filter(c => !/^(rtx|red|ulpfec|flexfec-03)$/i.test(c)) : null;
  const MS = window.MediaSource || window.ManagedMediaSource;
  const ts = t => { try { return MS ? MS.isTypeSupported(t) : null; } catch { return null; } };
  return {
    webrtc: !!window.RTCPeerConnection, rtcCodecs: codecs, rtcH264: codecs ? codecs.some(c => /h264/i.test(c)) : null,
    mse: !!window.MediaSource, managedMse: !!window.ManagedMediaSource,
    mseAvc: ts('video/mp4; codecs="avc1.64002a"'), mseAvcOpus: ts('video/mp4; codecs="avc1.64002a,opus"'),
    nativeHls: document.createElement('video').canPlayType('application/vnd.apple.mpegurl') || '',
  };
}

// sideways scrolling, and the outermost elements sticking out past the right edge
function overflow() {
  const se = document.scrollingElement || document.documentElement, W = window.innerWidth;
  const out = [];
  for (const el of document.body.querySelectorAll('*')) {
    const r = el.getBoundingClientRect();
    if (r.width > 0 && r.height > 0 && r.right > W + 1 && !(el.parentElement && out.includes(el.parentElement))) out.push(el);
  }
  const desc = el => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + [...el.classList].slice(0, 3).map(c => '.' + c).join('');
  const top = out.filter(el => !out.includes(el.parentElement)).slice(0, 5).map(el => `${desc(el)} (right ${Math.round(el.getBoundingClientRect().right)})`);
  return { hscroll: se.scrollWidth > W, scrollWidth: se.scrollWidth, innerWidth: W, sticking: top };
}

// the stream-chat area and every iframe on the page
function chatArea() {
  const x = document.querySelector('.xchat');
  const box = el => { const r = el.getBoundingClientRect(); return `${Math.round(r.width)}x${Math.round(r.height)}`; };
  return {
    xchat: x ? { shown: !x.hidden && x.getBoundingClientRect().height > 0, size: box(x), head: (x.querySelector('.xchat-head') || {}).textContent || '' } : null,
    iframes: [...document.querySelectorAll('iframe')].map(f => ({ src: f.src, cls: f.className, hidden: f.hidden, size: box(f) })),
  };
}

async function frameInfo(page) {
  const out = [];
  for (const f of page.frames()) {
    if (f === page.mainFrame()) continue;
    const info = { url: short(f.url()), ready: null, text: '' };
    try {
      const r = await Promise.race([f.evaluate(() => [document.readyState, (document.body && document.body.innerText || '').replace(/\s+/g, ' ').trim().slice(0, 100), !!document.querySelector('textarea, [data-a-target="chat-input"], [contenteditable="true"]')]),
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 3000))]);
      [info.ready, info.text, info.input] = r;
    } catch (e) { info.ready = 'n/a (' + String(e.message).split('\n')[0].slice(0, 60) + ')'; }
    out.push(info);
  }
  return out;
}

async function runPage(browser, engName, vp, pageKey, url) {
  const ctx = await browser.newContext({ ...vp.ctx, ignoreHTTPSErrors: false });
  // open the stream-chat panel (the viewer's own setting) so the lobby page shows it
  await ctx.addInitScript(() => { try { localStorage.setItem('cstv_xchat', '1'); } catch {} });
  if (pageKey === 'admin' && adminCookie) await ctx.addCookies([{ name: 'cstv_session', value: adminCookie, url }]);
  const page = await ctx.newPage();
  const origin = new URL(url).host;
  const r = { engine: engName, viewport: vp.key, viewportLabel: vp.label, page: pageKey, url, status: null,
    consoleErrors: [], consoleErrors3p: [], pageErrors: [], failed: [], failed3p: [], noise: [], loadMs: null };
  page.on('console', m => {
    if (m.type() !== 'error') return;
    const src = (m.location() && m.location().url) || '';
    const line = m.text().slice(0, 300) + (src ? `  [${short(src, 80)}]` : '');
    if (NOISE.test(src) || NOISE.test(m.text())) r.noise.push('console: ' + line);
    else if (!src || hostOf(src) === origin) r.consoleErrors.push(line);
    else r.consoleErrors3p.push(line);
  });
  page.on('pageerror', e => {   // Chromium reports embeds' (Twitch's) uncaught errors here too: theirs go with the noise
    const s = String(e && (e.stack || e.message) || e);
    const line = s.split('\n').slice(0, 3).join(' | ').slice(0, 400);
    if (NOISE.test(s) && !s.includes(origin)) r.noise.push('js: ' + line); else r.pageErrors.push(line);
  });
  const fail = (u, why) => {
    const line = `${why}  ${short(u)}`;
    const ours = hostOf(u) === origin;
    // an embed cancelling its own fetches (video segments on a quality switch, everything on close) is normal
    const aborted = /ERR_ABORTED|NS_BINDING_ABORTED|cancell?ed/i.test(why);
    if (NOISE.test(u) || (aborted && !ours)) r.noise.push(line); else if (ours) r.failed.push(line); else r.failed3p.push(line);
  };
  page.on('requestfailed', q => fail(q.url(), (q.failure() && q.failure().errorText) || 'failed'));
  page.on('response', s => { if (s.status() >= 400) fail(s.url(), String(s.status())); });

  const t0 = Date.now();
  try {
    const resp = await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });
    r.status = resp ? resp.status() : null;
    const left = () => Math.max(0, IDLE_CAP_MS - (Date.now() - t0));
    await page.waitForLoadState('load', { timeout: left() }).catch(() => {});
    await page.waitForLoadState('networkidle', { timeout: left() }).catch(() => {});
    r.loadMs = Date.now() - t0;
    Object.assign(r, await page.evaluate(overflow));
    if (pageKey === 'lobby') { r.chat = await page.evaluate(chatArea); r.frames = await frameInfo(page); }
    r.caps = await page.evaluate(mediaCaps);
    const file = `${engName}-${vp.key}-${pageKey}.png`;
    await page.screenshot({ path: path.join(OUT, file), fullPage: true, timeout: 20000 }).then(() => { r.screenshot = file; }, e => { r.screenshot = 'failed: ' + e.message.split('\n')[0]; });
  } catch (e) {
    r.error = String(e.message).split('\n')[0];
  }
  await ctx.close();
  return r;
}

async function runEngine(name, pages) {
  const eng = ENGINES[name];
  if (eng.needs === undefined ? false : !eng.needs) return { engine: name, skipped: 'not found (set CHROMIUM_PATH)', runs: [] };
  let browser;
  try {
    browser = await eng.type.launch({ ...(eng.launch || {}), timeout: 30000 });
  } catch (e) {
    const msg = String(e.message);
    const deps = msg.match(/install (lib[\w.\-\\ \n]+)/);
    return { engine: name, skipped: 'cannot launch: ' + (msg.includes('missing dependencies') ? 'host is missing libraries' + (deps ? ' (' + deps[1].replace(/[\\\n ]+/g, ' ').trim() + ')' : '') : msg.split('\n')[0]), runs: [] };
  }
  const out = { engine: name, version: browser.version(), runs: [] };
  for (const vp of VIEWPORTS(eng))
    for (const [pageKey, url] of pages) out.runs.push(await runPage(browser, name, vp, pageKey, url));
  await browser.close();
  return out;
}

function table(rows, cols) {
  const w = cols.map(c => Math.max(c.length, ...rows.map(r => String(r[c] ?? '').length)));
  const line = r => cols.map((c, i) => String(r[c] ?? '').padEnd(w[i])).join('  ');
  return [line(Object.fromEntries(cols.map(c => [c, c]))), w.map(n => '-'.repeat(n)).join('  '), ...rows.map(line)].join('\n');
}

let adminCookie = null;   // --local --admin: the site admin's website session
async function main() {
  let base = positional[0], lobby = opt('--lobby'), hub = null;
  if (flag('--local')) {
    hub = await startLocalHub();
    base = hub.base; lobby = lobby || hub.lobby; adminCookie = hub.admin;
    console.log(`local hub ${base}, lobby ${lobby}`);
  }
  if (!base) { console.error('usage: node run.mjs <base-url> [--lobby CODE] [--only chromium,firefox,webkit,chrome-sys] | node run.mjs --local'); process.exit(2); }
  base = base.replace(/\/+$/, '');
  const only = opt('--only');
  const names = only ? only.split(',').map(s => s.trim()).filter(Boolean) : Object.keys(ENGINES);
  for (const n of names) if (!ENGINES[n]) { console.error('unknown engine ' + n); process.exit(2); }
  const pages = [['home', base + '/']];
  if (lobby) pages.push(['lobby', `${base}/l/${lobby}`]);
  if (adminCookie) pages.push(['admin', `${base}/admin`]);
  mkdirSync(OUT, { recursive: true });

  const t0 = Date.now();
  let results;
  try {
    results = await Promise.all(names.map(n => runEngine(n, pages)));   // engines side by side, each one's pages in turn
  } finally {
    if (hub) hub.proc.kill('SIGTERM');
  }
  const secs = ((Date.now() - t0) / 1000).toFixed(1);

  const rows = [];
  for (const e of results) {
    if (e.skipped) { rows.push({ engine: e.engine, viewport: '-', page: '-', http: 'SKIPPED', note: e.skipped }); continue; }
    for (const r of e.runs) rows.push({
      engine: e.engine, viewport: r.viewportLabel, page: r.page, http: r.error ? 'ERR' : r.status, 'load s': r.loadMs != null ? (r.loadMs / 1000).toFixed(1) : '',
      'js err': r.pageErrors.length, 'console (own/3p)': `${r.consoleErrors.length}/${r.consoleErrors3p.length}`,
      'failed req (own/3p)': `${r.failed.length}/${r.failed3p.length}`, noise: r.noise.length,
      hscroll: r.hscroll ? `YES ${r.scrollWidth}>${r.innerWidth}` : r.hscroll === false ? 'no' : '?',
      chat: r.page === 'lobby' ? chatSummary(r) : '', note: r.error || '',
    });
  }
  console.log('\n' + table(rows, ['engine', 'viewport', 'page', 'http', 'load s', 'js err', 'console (own/3p)', 'failed req (own/3p)', 'noise', 'hscroll', 'chat', 'note']));

  const capRows = results.filter(e => !e.skipped).map(e => {
    const c = (e.runs.find(r => r.caps) || {}).caps || {};
    return { engine: e.engine, version: e.version, 'WebRTC H.264': fmt(c.rtcH264), 'MSE avc1': fmt(c.mseAvc), 'MSE avc1+opus': fmt(c.mseAvcOpus),
      'hls.js': fmt(c.mse || c.managedMse), 'native HLS': c.nativeHls || 'no',
      'stream path': c.rtcH264 ? 'WebRTC' : (c.mse || c.managedMse) && c.mseAvcOpus ? 'HLS (hls.js)' : c.nativeHls ? 'HLS (native)' : 'NONE',
      'rtc codecs': (c.rtcCodecs || []).join(' ') };
  });
  console.log('\n' + table(capRows, ['engine', 'version', 'WebRTC H.264', 'MSE avc1', 'MSE avc1+opus', 'hls.js', 'native HLS', 'stream path', 'rtc codecs']));

  // details: everything that went wrong, per run
  const details = [];
  for (const e of results) for (const r of e.runs) {
    const items = [...r.pageErrors.map(x => 'js: ' + x), ...r.consoleErrors.map(x => 'console: ' + x), ...r.failed.map(x => 'request: ' + x),
      ...r.consoleErrors3p.map(x => '3p console: ' + x), ...r.failed3p.map(x => '3p request: ' + x),
      ...(r.hscroll ? ['sticks out: ' + r.sticking.join(', ')] : []),
      ...(r.frames || []).map(f => `frame: ${f.url} ready=${f.ready}${f.input ? ' input=yes' : ''}${f.text ? ' text="' + f.text + '"' : ''}`)];
    if (items.length) details.push(`\n[${e.engine} ${r.viewportLabel} ${r.page}]\n  ` + [...new Set(items)].slice(0, 25).join('\n  '));
  }
  if (details.length) console.log('\nDetails:' + details.join(''));

  writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify({ base, lobby, seconds: Number(secs), at: new Date().toISOString(), results }, null, 2));
  console.log(`\n${secs} s; screenshots + summary.json in ${path.relative(process.cwd(), OUT) || OUT}/`);
}

function chatSummary(r) {
  if (!r.chat) return '';
  const x = r.chat.xchat;
  const tw = (r.frames || []).find(f => /twitch\.tv\/embed\/.+\/chat/.test(f.url));
  return (x ? (x.shown ? `panel ${x.size}` : 'panel hidden') : 'no panel') + (tw ? ` twitch-iframe ready=${tw.ready}${tw.input ? ' input' : ''}` : '');
}
const fmt = v => v === true ? 'yes' : v === false ? 'no' : v == null ? '-' : String(v);

main().catch(e => { console.error(e); process.exit(1); });
