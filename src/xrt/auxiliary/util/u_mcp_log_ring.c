// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP log ring buffer implementation.
 * @ingroup aux_util
 */

#include "u_mcp_log_ring.h"

#include "util/u_logging.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RING_SIZE 512 /* must be power of two */
#if (RING_SIZE & (RING_SIZE - 1)) != 0
#error "RING_SIZE must be a power of two"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct u_mcp_log_entry g_ring[RING_SIZE];
static uint64_t g_next_seq = 1;
static bool g_installed = false;

static int64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
sink_cb(const char *file, int line, const char *func, enum u_logging_level level, const char *format, va_list args,
        void *data)
{
	(void)data;
	char text[U_MCP_LOG_MAX_TEXT];
	int n = vsnprintf(text, sizeof(text), format, args);
	if (n < 0) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	uint64_t seq = g_next_seq++;
	struct u_mcp_log_entry *e = &g_ring[seq & (RING_SIZE - 1)];
	e->seq = seq;
	e->timestamp_ns = mono_ns();
	e->level = level;
	// Truncate safely — text is already bounded by vsnprintf.
	size_t copy = (size_t)n < sizeof(e->text) ? (size_t)n : sizeof(e->text) - 1;
	memcpy(e->text, text, copy);
	e->text[copy] = '\0';
	(void)file;
	(void)line;
	(void)func;
	pthread_mutex_unlock(&g_lock);
}

void
u_mcp_log_ring_start(void)
{
	pthread_mutex_lock(&g_lock);
	bool already = g_installed;
	if (!g_installed) {
		g_installed = true;
	}
	pthread_mutex_unlock(&g_lock);
	if (!already) {
		u_log_set_sink(sink_cb, NULL);
	}
}

void
u_mcp_log_ring_stop(void)
{
	pthread_mutex_lock(&g_lock);
	bool was = g_installed;
	g_installed = false;
	pthread_mutex_unlock(&g_lock);
	if (was) {
		u_log_set_sink(NULL, NULL);
	}
}

void
u_mcp_log_ring_read(uint64_t since,
                    struct u_mcp_log_entry *out_entries,
                    size_t max_entries,
                    size_t *out_count,
                    uint64_t *out_next_cursor,
                    uint64_t *out_dropped)
{
	pthread_mutex_lock(&g_lock);
	uint64_t next = g_next_seq;
	uint64_t oldest_in_ring = (next > RING_SIZE) ? (next - RING_SIZE) : 1;
	uint64_t start = since + 1;
	uint64_t dropped = 0;
	if (start < oldest_in_ring) {
		dropped = oldest_in_ring - start;
		start = oldest_in_ring;
	}
	size_t count = 0;
	while (start < next && count < max_entries) {
		out_entries[count++] = g_ring[start & (RING_SIZE - 1)];
		start++;
	}
	pthread_mutex_unlock(&g_lock);
	*out_count = count;
	*out_next_cursor = start > 0 ? start - 1 : 0;
	*out_dropped = dropped;
}
