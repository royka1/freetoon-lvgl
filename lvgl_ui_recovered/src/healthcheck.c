/* See healthcheck.h.
 *
 * Each watched service has:
 *   - probe_fn: returns 0 on healthy, -1 on fail. Should complete in <5s.
 *   - subType  : unique key for happ_usermsg dedup
 *   - text     : Dutch label shown when down (visible in Inbox)
 *
 * We don't notify on the first poll if the service was already down —
 * to avoid spamming on boot we require one good poll first before any
 * transition is considered. Recovery (DOWN→UP) always clears.
 *
 * Probes use http_fetch() where the body shape gives us a richer
 * "alive" signal than a TCP open, and a raw TCP connect (no
 * proto-specific traffic) for MQTT/DNS so we don't depend on the
 * libs/credentials those servers expect. */
#include "healthcheck.h"
#include "notify.h"
#include "http.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>

#define HC_POLL_S 60

typedef int (*probe_fn)(void);

typedef struct {
    const char * subType;   /* notification dedup key (e.g. "ha_offline") */
    const char * down_text; /* Dutch label, kept short for the Inbox row  */
    probe_fn     probe;
    /* Runtime state, mutated by the worker thread only. */
    int          seen_up;   /* don't notify until first healthy poll      */
    int          last_ok;   /* most recent probe result                   */
} check_t;

/* ---- low-level TCP open / HTTP-OK helpers ---- */

/* Non-blocking connect with a 3s SO_RCVTIMEO ceiling. Used for the
 * MQTT broker (TCP/1883) and the DNS open-port check. */
static int tcp_open(const char * host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(s); return -1; }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* Block but bounded — non-blocking + select is overkill for a 3s ceiling. */
    int rc = connect(s, (struct sockaddr *)&a, sizeof(a));
    close(s);
    return rc == 0 ? 0 : -1;
}

/* http_fetch() returns 0 on success + body in `out`. We don't care about
 * the body shape — for these probes any 2xx with non-empty body means
 * the service is responding. */
static int http_alive(const char * url) {
    char buf[4096];
    return http_fetch(url, buf, sizeof(buf)) == 0 ? 0 : -1;
}

/* ---- per-service probes ---- */
static int probe_ha(void)         { return http_alive("http://192.168.3.101:8123/auth/providers"); }
static int probe_otgw(void)       { return http_alive("http://192.168.99.21/api/v0/settings"); }
static int probe_buienradar(void) { return http_alive("https://data.buienradar.nl/2.0/feed/json"); }
static int probe_mqtt(void)       { return tcp_open("192.168.3.101", 1883); }
static int probe_p1_elec(void)    { return http_alive("http://192.168.99.69/api/v1/data"); }
static int probe_p1_water(void)   { return http_alive("http://192.168.99.115/api/v1/data"); }
static int probe_itho(void)       { return http_alive("http://192.168.3.236/"); }
static int probe_opnsense(void)   { return tcp_open("192.168.3.227", 443); }
static int probe_internet(void)   { return tcp_open("1.1.1.1", 53); }

static check_t g_checks[] = {
    { "ha_offline",         "Home Assistant niet bereikbaar",         probe_ha,          0, 1 },
    { "otgw_offline",       "OpenTherm Gateway niet bereikbaar",      probe_otgw,        0, 1 },
    { "buienradar_offline", "Buienradar (weer) niet bereikbaar",      probe_buienradar,  0, 1 },
    { "mqtt_offline",       "MQTT broker (192.168.3.101:1883) down",  probe_mqtt,        0, 1 },
    { "p1_elec_offline",    "P1 elektriciteitsmeter niet bereikbaar", probe_p1_elec,     0, 1 },
    { "p1_water_offline",   "P1 watermeter niet bereikbaar",          probe_p1_water,    0, 1 },
    { "itho_offline",       "Itho ventilatie niet bereikbaar",        probe_itho,        0, 1 },
    { "opnsense_offline",   "Router (OPNsense) niet bereikbaar",      probe_opnsense,    0, 1 },
    { "internet_offline",   "Geen internet (DNS niet bereikbaar)",    probe_internet,    0, 1 },
};
#define N_CHECKS (sizeof(g_checks) / sizeof(g_checks[0]))

static void * hc_thread(void * arg) {
    (void)arg;
    /* Stagger the first round by 15s so boxtalk + ha pollers settle. */
    sleep(15);
    while (1) {
        for (size_t i = 0; i < N_CHECKS; i++) {
            check_t * c = &g_checks[i];
            int ok = (c->probe() == 0);
            if (ok) {
                if (!c->seen_up) c->seen_up = 1;        /* first ever pass */
                if (!c->last_ok) notify_clear("system", c->subType);
                c->last_ok = 1;
            } else {
                /* Only signal a failure once we've seen the service work
                 * at least once — avoids "X offline" toasts at every boot
                 * if a host is permanently absent in this install. */
                if (c->seen_up && c->last_ok)
                    notify_show("system", c->subType, c->down_text);
                c->last_ok = 0;
            }
        }
        sleep(HC_POLL_S);
    }
    return NULL;
}

int healthcheck_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, hc_thread, NULL) != 0) return -1;
    pthread_detach(t);
    fprintf(stderr, "[hc] healthcheck thread started (%zu services, every %ds)\n",
            N_CHECKS, HC_POLL_S);
    return 0;
}
