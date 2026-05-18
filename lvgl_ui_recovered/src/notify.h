#ifndef TOON_NOTIFY_H
#define TOON_NOTIFY_H

/* Tiny wrapper around boxtalk_send_raw_xml() that surfaces toon-UI alerts
 * through happ_usermsg's CreateNotification / DeleteNotification verbs.
 * These notifications land in the Inbox tile and the dim screen's alert
 * banner — same path Eneco's qt-gui uses for its own system messages.
 *
 * type + subType together form the dedup key:
 *   notify_show("system","ha_offline","HA niet bereikbaar");
 *   notify_clear("system","ha_offline");   // when it recovers
 *
 * Safe to call at any time — internally just queues an XML write on the
 * BoxTalk client socket. */

/* Create or refresh a notification. Returns 0 on success. */
int notify_show(const char * type, const char * subType, const char * text);

/* Remove an active notification keyed on (type,subType). */
int notify_clear(const char * type, const char * subType);

#endif
