#ifndef NET_H
#define NET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERVICE_MAX 10

void net_init(void);
bool net_wifi_connected(void);
/* Queue a POST. `path_query` is the part after the host, e.g. "/api/taxi/SetTaxiStart?serial=..". */
bool net_post(const char *path_query);
void net_service_send(int service_number, int start); /* start: 1 = begin, 2 = end */

/* Samar page data (from the tariff JSON) */
int cfg_service_count(void);
int cfg_service(int idx);

/* Reserve ("nobat") page: scan for the dispatch AP */
void net_reserve_scan_request(void);
bool net_reserve_found(void);
void net_reserve_reset(void);

const char *net_serial(void);

/* Download https://host:port/path. Every piece of the body goes to `sink`; a negative return
 * from the sink stops the download. Returns the HTTP status (200 = whole file was given to the
 * sink), -ECANCELED if the sink stopped it, or another negative error. Net thread only. */
typedef int (*net_sink_t)(const uint8_t *data, size_t len, void *user);
int net_download(const char *host, uint16_t port, const char *path, net_sink_t sink, void *user);
/* Last HTTPS result: 1 = server answered, -1 = failed, 0 = not tried yet. */
int net_server_state(void);
#endif
