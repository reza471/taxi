#ifndef NET_H
#define NET_H
#include <stdbool.h>
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
/* Last HTTPS result: 1 = server answered, -1 = failed, 0 = not tried yet. */
int net_server_state(void);
#endif
