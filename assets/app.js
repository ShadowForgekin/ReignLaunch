// ReignLaunch 官网脚本
//
// ── 版本 ─────────────────────────────────────────────
// 只从 GitHub 读取（Releases 列表，含 pre-release），显示 tag 与「发布于 X」。
// 站点不保存、也不发布任何版本信息。
//
// ── 下载 ─────────────────────────────────────────────
// 两个来源，加载时实测速度，自动选最快的；结果在 sessionStorage 缓存 10 分钟。
//   ① 站点文件   download/ReignLaunch.exe
//   ② 加速代理   gh-proxy
// 页面上不显示任何来源名称或地址，只有一个下载按钮。

const REPO = 'ShadowForgekin/ReignLaunch';
const BRANCH = 'site';
const LOCAL_EXE = 'download/ReignLaunch.exe';
const RAW_EXE = `https://raw.githubusercontent.com/${REPO}/${BRANCH}/download/ReignLaunch.exe`;
const PROXY_EXE = `https://gh-proxy.com/${RAW_EXE}`;   // 代理指向仓库里的同一份文件

// 只有两条：站点本地文件 + 加速代理（GitHub 直连太慢，不用）
const CANDIDATES = [
  { id: 'site', url: LOCAL_EXE },
  { id: 'proxy', url: PROXY_EXE },
];

const CACHE_KEY = 'rl_fast_src';
const CACHE_TTL = 10 * 60 * 1000;

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];

const timeOf = (iso) => {
  const t = Date.parse(iso || '');
  return isNaN(t) ? 0 : t;
};

// ── 版本（GitHub only）─────────────
async function fetchReleaseMeta() {
  for (const u of [
    `https://api.github.com/repos/${REPO}/releases?per_page=20`,
    `https://gh-proxy.com/https://api.github.com/repos/${REPO}/releases?per_page=20`,
  ]) {
    try {
      const r = await fetch(u, { headers: { accept: 'application/vnd.github+json' } });
      if (!r.ok) continue;
      const list = await r.json();
      if (!Array.isArray(list)) continue;

      const usable = list
        .filter((x) => x && !x.draft && x.published_at)
        .sort((a, b) => timeOf(b.published_at) - timeOf(a.published_at));
      if (!usable.length) continue;

      const rel = usable[0];
      const asset = (rel.assets || []).find((a) => /^ReignLaunch\.exe$/i.test(a.name));
      return {
        tag: rel.tag_name,
        published: rel.published_at,
        size: asset ? asset.size : 0,
      };
    } catch (_) {}
  }
  return null;
}

// ── 展示 ─────────────────────────────
function fmtDate(ms) {
  if (!ms) return '';
  return new Date(ms).toLocaleDateString('zh-CN', { year: 'numeric', month: 'long', day: 'numeric' });
}

function fmtAgo(ms) {
  if (!ms) return '';
  const d = Date.now() - ms;
  if (d < 0) return '刚刚';
  const day = Math.floor(d / 86400000);
  if (day < 1) {
    const h = Math.floor(d / 3600000);
    return h < 1 ? '刚刚' : `${h} 小时前`;
  }
  if (day < 30) return `${day} 天前`;
  const mo = Math.floor(day / 30);
  return mo < 12 ? `${mo} 个月前` : `${Math.floor(mo / 12)} 年前`;
}

function renderMeta(info) {
  const tag = $('#verTag');
  const vd = $('#verDate');
  const dd = $('#dlDate');
  const meta = $('#dlMeta');

  if (!info) {
    if (tag) tag.textContent = '版本信息获取失败';
    if (vd) vd.textContent = '无法连接 GitHub';
    if (dd) dd.textContent = '';
    if (meta) meta.textContent = 'Windows 7 SP1 及以上 · 64 位';
    return;
  }

  if (tag) tag.textContent = info.tag;

  const t = timeOf(info.published);
  const date = fmtDate(t);

  if (vd) vd.textContent = date ? `发布于 ${date}` : '发布时间未知';
  if (dd) {
    dd.innerHTML = date
      ? `版本 <b>${info.tag}</b> · 发布于 <b>${date}</b>（${fmtAgo(t)}）`
      : `版本 <b>${info.tag}</b>`;
  }
  if (meta) {
    const parts = ['Windows 7 SP1 及以上', '64 位'];
    if (info.size) parts.push(`文件大小 ${(info.size / 1024 / 1024).toFixed(1)} MB`);
    meta.textContent = parts.join(' · ');
  }
}

// 记住当前最快源（不写进 DOM，避免暴露地址）
let bestSrc = null;

// 下载：先 fetch 成 blob，再用本地 blob 触发保存
// 这样地址栏/下载器里看不到真实来源地址
async function downloadVia(btn) {
  const label = btn.dataset.label || btn.textContent;

  // 直接交给浏览器下载（保留用户手势，不会出现「已取消」）
  // 地址只在内存里，DOM / 状态栏都不出现
  const order = [];
  if (bestSrc) order.push(bestSrc);
  for (const c of CANDIDATES) if (!bestSrc || c.id !== bestSrc.id) order.push(c);

  const target = order[0] || CANDIDATES[0];

  btn.textContent = '开始下载…';
  setTimeout(() => { btn.textContent = label; }, 1600);

  // 用隐藏 iframe 触发，页面不跳转、地址栏不变
  const fr = document.createElement('iframe');
  fr.style.display = 'none';
  fr.src = target.url;
  document.body.appendChild(fr);
  setTimeout(() => { try { fr.remove(); } catch (_) {} }, 60000);
}

function bindDownloadButtons() {
  for (const id of ['#dlBtn', '#dlBtn2']) {
    const b = $(id);
    if (!b || b.dataset.bound) continue;
    b.dataset.bound = '1';
    b.dataset.label = b.textContent.trim();
    b.addEventListener('click', (e) => { e.preventDefault(); downloadVia(b); });
    b.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); downloadVia(b); }
    });
  }
}

// ── 测速 ─────────────────────────────
// 只拉前 128KB 估算速度，避免把整个 exe 下两遍
async function measure(c, want = 131072, limitMs = 3200) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), limitMs);
  const t0 = performance.now();
  try {
    const r = await fetch(c.url, {
      headers: { Range: `bytes=0-${want - 1}` },
      signal: ctrl.signal,
      cache: 'no-store',
    });
    if (!r.ok && r.status !== 206) return null;
    if (!r.body) return null;

    const reader = r.body.getReader();
    let got = 0;
    while (got < want) {
      const { done, value } = await reader.read();
      if (done) break;
      got += value.length;
    }
    try { await reader.cancel(); } catch (_) {}
    clearTimeout(timer);

    if (got < 8192) return null;
    const ms = Math.max(1, performance.now() - t0);
    return { id: c.id, url: c.url, kbps: (got / 1024) / (ms / 1000), ms };
  } catch (_) {
    clearTimeout(timer);
    return null;
  }
}

function readCache() {
  try {
    const raw = sessionStorage.getItem(CACHE_KEY);
    if (!raw) return null;
    const o = JSON.parse(raw);
    if (!o || Date.now() - o.at > CACHE_TTL) return null;
    return CANDIDATES.find((x) => x.id === o.id) || null;
  } catch (_) {
    return null;
  }
}

function writeCache(id) {
  try { sessionStorage.setItem(CACHE_KEY, JSON.stringify({ id, at: Date.now() })); } catch (_) {}
}

async function pickFastest() {
  const cached = readCache();
  if (cached) return cached;

  const results = (await Promise.all(CANDIDATES.map((c) => measure(c)))).filter(Boolean);
  if (!results.length) return CANDIDATES[0];

  results.sort((a, b) => b.kbps - a.kbps);
  writeCache(results[0].id);
  return CANDIDATES.find((c) => c.id === results[0].id) || CANDIDATES[0];
}

// 站点文件的真实大小
async function localSize() {
  try {
    const r = await fetch(LOCAL_EXE, { method: 'HEAD', cache: 'no-store' });
    const len = parseInt(r.headers.get('content-length') || '0', 10);
    return isNaN(len) ? 0 : len;
  } catch (_) {
    return 0;
  }
}

// ── 启动 ─────────────────────────────
async function init() {
  const info = await fetchReleaseMeta();
  renderMeta(info);

  const size = await localSize();
  if (info && size) {
    info.size = size;
    renderMeta(info);
  }

  bindDownloadButtons();
  bestSrc = await pickFastest();
}

// ── 其他 ─────────────────────────────
const y = $('#year');
if (y) y.textContent = new Date().getFullYear();

const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
if (!reduce && 'IntersectionObserver' in window) {
  const io = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (e.isIntersecting) {
        e.target.style.opacity = '1';
        e.target.style.transform = 'translateY(0)';
        io.unobserve(e.target);
      }
    }
  }, { threshold: 0.12 });

  $$('.card, .shot, .steps li').forEach((el) => {
    el.style.opacity = '0';
    el.style.transform = 'translateY(14px)';
    el.style.transition = 'opacity .5s ease, transform .5s ease';
    io.observe(el);
  });
}

init();
