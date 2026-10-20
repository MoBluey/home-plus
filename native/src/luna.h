/* Home+ native: calls on the webOS luna bus from inside the app sandbox.
 *
 * The launcher runs in the app sandbox, where `luna-send` can't reach other
 * services, but the app's own bus connection can. libluna-service2 and glib are
 * the TV's own; they're loaded with dlopen, so the binary still starts (and
 * falls back to luna-send) where they're missing, e.g. on the desktop. */
#ifndef HOMEPLUS_LUNA_H
#define HOMEPLUS_LUNA_H

/* One call, waiting up to timeout_ms for the reply. Returns 1 when the reply
 * has returnValue true, 0 for any other reply, and -1 when the bus can't be
 * used at all. *out (malloc'd, may be NULL) is the reply or an error text. */
/* Call once before any worker thread uses luna_call. */
void luna_setup(void);

int luna_call(const char *uri, const char *payload, int timeout_ms, char **out);

/* Appends a line to the launcher's own log (/tmp/homeplus-native.log), for
 * things that happen before the service can take log lines. */
void luna_diag(const char *fmt, ...);

#endif
