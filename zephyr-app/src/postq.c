/*
 * FIFO of HTTP POST records, saved in flash (settings partition).
 *
 * - Records are sent strictly in order (trip start before trip end).
 * - push/peek/pop only change RAM, so the UI never waits for a flash write.
 * - postq_flush() writes the changes. The net thread calls it when no key is being used.
 * - Flash layout: "taxiq/s<N>" = record text of slot N, "taxiq/m" = {head, count}.
 *   A slot is written BEFORE the meta, so a power loss never leaves a half record visible.
 * - When the queue is full, the oldest "start"/"service" record is dropped first.
 *   Trip ends (they carry the money) are kept.
 */
#include "postq.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(postq, LOG_LEVEL_INF);

struct pq_meta {
	int32_t head;
	int32_t count;
};

static char queue[QUEUE_LEN][URL_MAX];
static struct pq_meta pq;
static uint32_t slot_dirty; /* bit i: slot i must be written to flash */
static bool meta_dirty;
static K_MUTEX_DEFINE(q_lock);

static int pq_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int idx;

	if (strcmp(name, "m") == 0 && len == sizeof(pq)) {
		struct pq_meta m;

		if (read_cb(cb_arg, &m, sizeof(m)) == sizeof(m) && m.head >= 0 &&
		    m.head < QUEUE_LEN && m.count >= 0 && m.count <= QUEUE_LEN) {
			pq = m;
		}
	} else if (name[0] == 's' && sscanf(name + 1, "%d", &idx) == 1 && idx >= 0 &&
		   idx < QUEUE_LEN && len < URL_MAX) {
		read_cb(cb_arg, queue[idx], len);
		queue[idx][len] = '\0';
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(taxiq, "taxiq", NULL, pq_set, NULL, NULL);

static bool is_trip_end(const char *url)
{
	return strstr(url, "/SetTaxiEnd") != NULL;
}

bool postq_push(const char *url)
{
	bool ok = false;

	if (strlen(url) >= URL_MAX - 1) {
		LOG_ERR("record too long, dropped");
		return false;
	}

	k_mutex_lock(&q_lock, K_FOREVER);
	if (pq.count >= QUEUE_LEN) {
		/* full: drop the oldest record that is NOT a trip end */
		for (int n = 0; n < pq.count; n++) {
			if (is_trip_end(queue[(pq.head + n) % QUEUE_LEN])) {
				continue;
			}
			/* move the later records one place down */
			for (int m = n; m < pq.count - 1; m++) {
				int a = (pq.head + m) % QUEUE_LEN;
				int b = (pq.head + m + 1) % QUEUE_LEN;

				memcpy(queue[a], queue[b], URL_MAX); /* a != b */
				slot_dirty |= BIT(a);
			}
			pq.count--;
			meta_dirty = true;
			LOG_WRN("queue full: dropped an old start/service record");
			break;
		}
	}
	if (pq.count < QUEUE_LEN) {
		int i = (pq.head + pq.count) % QUEUE_LEN;

		strncpy(queue[i], url, URL_MAX - 1);
		queue[i][URL_MAX - 1] = '\0';
		pq.count++;
		slot_dirty |= BIT(i);
		meta_dirty = true;
		ok = true;
		LOG_INF("queued (%d waiting)", (int)pq.count);
	} else {
		LOG_ERR("queue full of trip ends, new record dropped");
	}
	k_mutex_unlock(&q_lock);
	return ok;
}

bool postq_peek(char *out, size_t size)
{
	bool ok = false;

	k_mutex_lock(&q_lock, K_FOREVER);
	if (pq.count > 0) {
		strncpy(out, queue[pq.head], size - 1);
		out[size - 1] = '\0';
		ok = true;
	}
	k_mutex_unlock(&q_lock);
	return ok;
}

void postq_pop(void)
{
	k_mutex_lock(&q_lock, K_FOREVER);
	if (pq.count > 0) {
		queue[pq.head][0] = '\0';
		pq.head = (pq.head + 1) % QUEUE_LEN;
		pq.count--;
		meta_dirty = true;
	}
	k_mutex_unlock(&q_lock);
}

int postq_count(void)
{
	return pq.count;
}

bool postq_dirty(void)
{
	return slot_dirty != 0 || meta_dirty;
}

void postq_flush(void)
{
	char name[16];
	char copy[URL_MAX];
	uint32_t dirty;
	bool meta;
	struct pq_meta m;

	k_mutex_lock(&q_lock, K_FOREVER);
	dirty = slot_dirty;
	meta = meta_dirty;
	slot_dirty = 0;
	meta_dirty = false;
	k_mutex_unlock(&q_lock);

	for (int i = 0; i < QUEUE_LEN; i++) {
		if (!(dirty & BIT(i))) {
			continue;
		}
		k_mutex_lock(&q_lock, K_FOREVER);
		memcpy(copy, queue[i], sizeof(copy));
		k_mutex_unlock(&q_lock);
		copy[sizeof(copy) - 1] = '\0';

		snprintf(name, sizeof(name), "taxiq/s%d", i);
		if (settings_save_one(name, copy, strlen(copy)) != 0) {
			LOG_ERR("queue slot %d save failed, will retry", i);
			k_mutex_lock(&q_lock, K_FOREVER);
			slot_dirty |= BIT(i);
			meta_dirty |= meta; /* do not publish the meta before its slots */
			k_mutex_unlock(&q_lock);
			return;
		}
	}
	if (meta) {
		k_mutex_lock(&q_lock, K_FOREVER);
		m = pq;
		k_mutex_unlock(&q_lock);
		if (settings_save_one("taxiq/m", &m, sizeof(m)) != 0) {
			LOG_ERR("queue meta save failed, will retry");
			k_mutex_lock(&q_lock, K_FOREVER);
			meta_dirty = true;
			k_mutex_unlock(&q_lock);
		}
	}
}
