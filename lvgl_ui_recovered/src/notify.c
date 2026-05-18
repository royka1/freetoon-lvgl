/* See notify.h for the why. */
#include "notify.h"
#include <stdio.h>
#include <string.h>

/* Defined in boxtalk.c. */
extern int boxtalk_send_raw_xml(const char * xml);

/* Same toonui UUID used by inbox.c's DeleteNotification action — keeps
 * the sender field consistent across all happ_usermsg verbs. */
#define TOONUI_UUID    "qb-659918000101-2011A0LOHI:toonui"
#define USERMSG_UUID   "qb-659918000101-2011A0LOHI:happ_usermsg"
#define NOTIF_SVC      "urn:hcb-hae-com:serviceId:Notification"
#define NOTIF_NS       "urn:hcb-hae-com:service:Notification:1"

/* Minimal XML-attribute escaping for the body text. Notifications are
 * short single-line strings so this stays simple — & < > only. */
static void xml_escape(const char * in, char * out, size_t outsz) {
    size_t o = 0;
    for (; *in && o < outsz - 6; in++) {
        switch (*in) {
            case '&': memcpy(out + o, "&amp;",  5); o += 5; break;
            case '<': memcpy(out + o, "&lt;",   4); o += 4; break;
            case '>': memcpy(out + o, "&gt;",   4); o += 4; break;
            default:  out[o++] = *in;
        }
    }
    out[o] = 0;
}

int notify_show(const char * type, const char * subType, const char * text) {
    if (!type || !subType || !text) return -1;
    char esc[256];
    xml_escape(text, esc, sizeof(esc));
    char xml[768];
    snprintf(xml, sizeof(xml),
        "<action class=\"invoke\" uuid=\"" TOONUI_UUID "\" "
        "destuuid=\"" USERMSG_UUID "\" serviceid=\"" NOTIF_SVC "\">"
        "<u:CreateNotification xmlns:u=\"" NOTIF_NS "\">"
        "<type>%s</type><subType>%s</subType><plainText>%s</plainText>"
        "</u:CreateNotification></action>",
        type, subType, esc);
    int rc = boxtalk_send_raw_xml(xml);
    if (rc != 0) fprintf(stderr, "[notify] show %s/%s failed rc=%d\n",
                         type, subType, rc);
    return rc;
}

int notify_clear(const char * type, const char * subType) {
    if (!type || !subType) return -1;
    char xml[512];
    snprintf(xml, sizeof(xml),
        "<action class=\"invoke\" uuid=\"" TOONUI_UUID "\" "
        "destuuid=\"" USERMSG_UUID "\" serviceid=\"" NOTIF_SVC "\">"
        "<u:DeleteNotification xmlns:u=\"" NOTIF_NS "\">"
        "<type>%s</type><subType>%s</subType>"
        "</u:DeleteNotification></action>",
        type, subType);
    return boxtalk_send_raw_xml(xml);
}
