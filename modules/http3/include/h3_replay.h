/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef H3_REPLAY_H
#define H3_REPLAY_H

#include "apr_pools.h"
#include "apr_thread_mutex.h"
#include "apr_time.h"

#define H3_REPLAY_KEY_SIZE 32

typedef struct h3_replay_cache h3_replay_cache;

typedef enum
{
    /* Remove the oldest unexpired token when the fixed table is full. */
    H3_REPLAY_FULL_EVICT_OLDEST = 0,
    /* Refuse additional early data rather than forget an in-window token. */
    H3_REPLAY_FULL_REJECT_EARLY = 1
} h3_replay_full_policy;

typedef enum
{
    /* The token was not present and has been inserted. */
    H3_REPLAY_SEEN_NEW = 1,
    /* The same token was seen inside the time window. */
    H3_REPLAY_SEEN_DUPLICATE = 0,
    /* The cache was full and the policy is to fail closed. */
    H3_REPLAY_SEEN_FULL = -1
} h3_replay_seen_result;

typedef struct h3_replay_stats
{
    apr_size_t capacity;
    apr_size_t entries;
    apr_uint64_t hits;
    apr_uint64_t misses;
    apr_uint64_t expired;
    apr_uint64_t evicted;
    apr_uint64_t rejected_full;
} h3_replay_stats;

/**
 * Create a fixed-capacity replay cache.
 *
 * The table and all entries are allocated from @p pool; successful creation
 * performs no per-token allocations, so capacity is also the memory bound.
 * Each token expires @p window after its first accepted sighting.
 */
apr_status_t h3_replay_cache_create(h3_replay_cache** cache,
                                    apr_pool_t* pool,
                                    apr_size_t capacity,
                                    apr_interval_time_t window,
                                    h3_replay_full_policy full_policy);

/**
 * Record or look up one replay token.
 *
 * Expired entries are treated as absent. With the eviction policy this always
 * inserts a new token after removing expired or oldest state; with the reject
 * policy it returns H3_REPLAY_SEEN_FULL when no slot is available.
 *
 * @param now Time for the lookup, or zero to use apr_time_now(). Tests may
 *            pass an explicit monotonic/apr time to advance the window.
 */
h3_replay_seen_result h3_replay_cache_see(h3_replay_cache* cache,
                                          const unsigned char* key,
                                          apr_size_t key_len,
                                          apr_time_t now);

/** Copy a consistent snapshot of cache counters. Safe with NULL cache. */
void h3_replay_cache_get_stats(h3_replay_cache* cache,
                               h3_replay_stats* stats);

/** Remove every token. Useful for tests and explicit operator reset hooks. */
void h3_replay_cache_clear(h3_replay_cache* cache);

#endif /* H3_REPLAY_H */
