#ifndef TOON_PWA_SERVER_H
#define TOON_PWA_SERVER_H

/* Tiny HTTP server bolted onto toonui. Serves a static PWA from
 * /mnt/data/pwa/ and exposes the in-process toon_state via a JSON API,
 * so the same data the LVGL screens render is also reachable from a
 * browser on the phone (and any other device on the LAN).
 *
 * No WebSocket yet — long-poll style: clients GET /api/state every 2s.
 * Cheap on a single-user install; trivially upgraded to SSE/WS later
 * without changing the frontend's render path.
 *
 * Port 10081 (next to the existing happ_thermstat HTTP on 10080). */

/* Start the server thread. Returns 0 on success. */
int pwa_start(void);

#endif
