/**
 * EdgeOne Pages Function: GET /api/download
 *
 * 让用户从本站域名直接下载最新版 ReignLaunch.exe，不再跳转 github.com。
 * 边缘节点回源 GitHub Releases，并把文件缓存在边缘（1 小时），
 * 后续用户命中缓存，速度取决于 EdgeOne CDN 而不是 GitHub。
 *
 * 支持 Range 请求（断点续传 / 下载器多线程）。
 * 若平台未启用函数，请改用 /download/ReignLaunch.exe 静态托管方案（见 README）。
 */

const REPO = 'ShadowForgekin/ReignLaunch';
const ASSET_NAME = /^ReignLaunch\.exe$/i;

export async function onRequest(context) {
  const { request } = context;

  if (request.method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: baseHeaders() });
  }
  if (request.method !== 'GET' && request.method !== 'HEAD') {
    return json({ error: 'method not allowed' }, 405);
  }

  // 1) 先拿最新 release 的资产地址（这里也做一次短缓存）
  const metaKey = new Request('https://edgeone.internal/reignlaunch/latest-asset');
  const cache = typeof caches !== 'undefined' ? caches.default : null;

  let asset = null;
  if (cache) {
    const hit = await cache.match(metaKey);
    if (hit) asset = await hit.json();
  }

  if (!asset) {
    try {
      const r = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
        headers: { accept: 'application/vnd.github+json', 'user-agent': 'ReignLaunch-Website' },
      });
      if (!r.ok) return json({ error: `github ${r.status}` }, 502);
      const rel = await r.json();
      const a = (rel.assets || []).find((x) => ASSET_NAME.test(x.name));
      if (!a) return json({ error: 'asset not found' }, 404);
      asset = { tag: rel.tag_name, url: a.browser_download_url, size: a.size };
      if (cache) {
        context.waitUntil(cache.put(metaKey, json(asset, 200, { 'cache-control': 'public, max-age=600' })));
      }
    } catch (err) {
      return json({ error: String(err && err.message ? err.message : err) }, 502);
    }
  }

  // 2) 回源下载（把 Range 透传过去，支持续传）
  const range = request.headers.get('range');
  const upstream = await fetch(asset.url, {
    // GitHub Releases 会 302 到 CDN，fetch 默认跟随
    headers: range ? { range } : {},
    redirect: 'follow',
  });

  if (!(upstream.status === 200 || upstream.status === 206)) {
    return json({ error: `upstream ${upstream.status}` }, 502);
  }

  const headers = new Headers(baseHeaders());
  headers.set('content-type', 'application/octet-stream');
  headers.set('content-disposition',
    `attachment; filename="ReignLaunch-${asset.tag || 'latest'}.exe"`);
  headers.set('cache-control', 'public, max-age=3600, s-maxage=86400');
  if (upstream.headers.get('content-length'))
    headers.set('content-length', upstream.headers.get('content-length'));
  if (upstream.headers.get('content-range'))
    headers.set('content-range', upstream.headers.get('content-range'));
  headers.set('accept-ranges', 'bytes');
  headers.set('x-reignlaunch-tag', asset.tag || '');

  // 3) 流式返回（HEAD 只要头）
  if (request.method === 'HEAD') {
    return new Response(null, { status: upstream.status, headers });
  }
  return new Response(upstream.body, { status: upstream.status, headers });
}

function baseHeaders() {
  return {
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET, HEAD, OPTIONS',
    'access-control-allow-headers': 'range, content-type',
    'access-control-expose-headers': 'content-length, content-range, x-reignlaunch-tag',
  };
}

function json(obj, status = 200, extra = {}) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8', ...baseHeaders(), ...extra },
  });
}
