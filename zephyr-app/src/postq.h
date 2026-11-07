#ifndef POSTQ_H
#define POSTQ_H
#include <stdbool.h>
#include <stddef.h>

#define QUEUE_LEN 16
#define URL_MAX 384

/* FIFO of HTTP POST records (path + query). Saved in the settings partition (flash),
 * so trips survive a power loss and a long server outage.
 * push/peek/pop/count only touch RAM and are safe from any thread.
 * postq_flush() writes to flash: call it from ONE thread (the net thread) when idle. */
bool postq_push(const char *url);          /* false if the record was dropped */
bool postq_peek(char *out, size_t size);   /* oldest record, false if empty */
void postq_pop(void);                      /* remove the oldest record */
int postq_count(void);
bool postq_dirty(void);                    /* RAM and flash differ */
void postq_flush(void);                    /* write changes to flash */

#endif
