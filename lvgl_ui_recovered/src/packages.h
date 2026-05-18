#ifndef TOON_PACKAGES_H
#define TOON_PACKAGES_H

#include "lvgl/lvgl.h"

/* Poller thread + banner queue for the HA-side delivery tracker.
 *
 * - Background thread polls HA's sensor.pkg_state_map every 15s,
 *   diffs against the cached map, queues a banner per state advancement.
 * - packages_banner_attach(parent) creates a banner widget pinned to the
 *   top of `parent` (home screen + dim screen each get one). Widget is
 *   invisible when the queue is empty; tap dismisses the top entry. */

void packages_start(void);

/* Attach a banner overlay to a screen. Safe to call multiple times — each
 * call creates a fresh widget owned by `parent`. Use on screen-create. */
void packages_banner_attach(lv_obj_t * parent);

#endif
