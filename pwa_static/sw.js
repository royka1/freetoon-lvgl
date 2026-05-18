// Minimal service worker. We don't cache the API; we cache the app shell
// so "Add to Home Screen" gives a fast cold launch even when the daemon
// is briefly unreachable. The data layer always goes to network.
const CACHE = 'toon-shell-v1';
const ASSETS = ['/', '/index.html', '/app.js', '/manifest.json'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(keys =>
    Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)))
  ));
});

self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  // Never cache API responses (always live)
  if (u.pathname.startsWith('/api/')) return;
  // Cache-first for the shell
  e.respondWith(
    caches.match(e.request).then(r => r || fetch(e.request).then(resp => {
      const copy = resp.clone();
      caches.open(CACHE).then(c => c.put(e.request, copy));
      return resp;
    }))
  );
});
