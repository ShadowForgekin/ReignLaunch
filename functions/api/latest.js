/**
 * EdgeOne Pages Function: GET /api/latest
 *
 * 代理 GitHub Releases API，给官网首页提供最新版本号与下载直链。
 * 好处：
 *  - 同域请求，不受浏览器 CORS 限制
 *  - 边缘缓存 10 分钟，避免 GitHub API 限流（未登录 60 次/小时/IP）
 *  - 隐藏仓库名（想改仓库只改这里）
 *
 * 部署：EdgeOne Pages 会自动识别 functions/ 目录，文件路径即路由。
 * 若平台未启用函数，前端 app.js 会自动回退到直连 GitHub API。
 */

const REPO = 'ShadowForgekin/ReignLaunch';
const ASSET_NAME = /^ReignLaunch\.exe$/i;

export async function onRequest(context) {
  const { request } = context;

  if (request.method === 'OPTIONS') {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }
  if (request.method !== 'GET') {
    return json({ error: 'method not allowed' }, 405);
  }

  const cacheKey = new Request('https://edgeone.internal/reignlaunch/latest', { method: 'GET' });
  const cache = typeof caches !== 'undefined' ? caches.default : null;

  if (cache) {
    const hit = await cache.match(cacheKey);
    if (hit) return hit;
  }

  let payload;
  try {
    const upstream = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
      headers: {
        accept: 'application/vnd.github+json',
        'user-agent': 'ReignLaunch-Website',
      },
    });

    if (!upstream.ok) {
      return json({ error: `github ${upstream.status}` }, 502);
    }

    const rel = await upstream.json();
    const asset = (rel.assets || []).find((a) => ASSET_NAME.test(a.name));

    payload = {
      tag: rel.tag_name,
      name: rel.name || rel.tag_name,
      published: rel.published_at,
      notes: rel.body || '',
      url: asset ? asset.browser_download_url : rel.html_url,
      size: asset ? asset.size : 0,
      downloads: asset ? asset.download_count : 0,
      page: rel.html_url,
    };
  } catch (err) {
    return json({ error: String(err && err.message ? err.message : err) }, 502);
  }

  const res = json(payload, 200, {
    // 边缘缓存 10 分钟，浏览器缓存 5 分钟
    'cache-control': 'public, max-age=300, s-maxage=600',
  });

  if (cache) {
    context.waitUntil(cache.put(cacheKey, res.clone()));
  }
  return res;
}

function corsHeaders() {
  return {
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET, OPTIONS',
    'access-control-allow-headers': 'content-type',
  };
}

function json(obj, status = 200, extra = {}) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      ...corsHeaders(),
      ...extra,
    },
  });
}
