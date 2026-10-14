/* The Overlay app's network: SceNet in the app's own process (loaded and
 * initialized when an Anki action first needs it), a VjoPlatform for
 * core/anki.c over its sockets, and the LAN probe that finds AnkiConnect.
 * The socket code follows the Shell's (shell/platform_vita.c); the probe is
 * the one SceShell no longer runs. AnkiConnect is plain HTTP on the LAN; an
 * https word-audio source uses the same BearSSL TLS as the Shell's. */
#ifndef VJO_SETTINGS_NET_H
#define VJO_SETTINGS_NET_H
#include "../core/anki_app.h"

/* Loads and starts SceNet/SceNetCtl once; 0, or the native error. */
int vjo_app_net_init(void);
void vjo_app_net_term(void);
/* 1 when Wi-Fi is connected (SceNetCtl), else 0. */
int vjo_app_net_connected(void);
/* The platform for core/anki.c; a request in progress ends at once after
 * vjo_app_net_cancel, and every later connect fails until _clear. */
void vjo_app_net_platform(VjoPlatform *p);
void vjo_app_net_cancel(void);
void vjo_app_net_clear(void);
int vjo_app_net_cancelled(void);
/* VjoAnkiAppOps.local_ipv4 / .probe */
int vjo_app_net_local_ipv4(void *ud, uint32_t *ip, uint32_t *mask);
int vjo_app_net_probe(void *ud, const uint32_t *ips, int n, int port, int window_us, uint8_t *open);
#endif
