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
/* Nobat (queue at a taxi station). The UI calls these; the net thread does the network work.
 *   begin: look for the station WiFi (SSID "Usefi"), then tell the server "ready" (NobatReady)
 *          and ask for the place in the queue every 5 s (NobatStatus).
 *   board: the passenger is in. Tells the server (NobatBoard, through the upload queue) and
 *          remembers the queue id: the next single-meter trip end carries it (nobat_id).
 *   leave: leave the queue (NobatLeave, only if the car had joined). */
typedef enum { NB_OFF = 0, NB_SEARCH, NB_JOIN, NB_WAIT, NB_TURN } nb_phase_t;
void net_nobat_begin(void);
void net_nobat_leave(void);
void net_nobat_board(void);
nb_phase_t net_nobat_phase(void);
int net_nobat_position(void); /* 1 = you are next */

const char *net_serial(void);

/* Download https://host:port/path. Every piece of the body goes to `sink`; a negative return
 * from the sink stops the download. Returns the HTTP status (200 = whole file was given to the
 * sink), -ECANCELED if the sink stopped it, or another negative error. Net thread only. */
typedef int (*net_sink_t)(const uint8_t *data, size_t len, void *user);
int net_download(const char *host, uint16_t port, const char *path, net_sink_t sink, void *user);
/* Last HTTPS result: 1 = server answered, -1 = failed, 0 = not tried yet. */
int net_server_state(void);
#endif
