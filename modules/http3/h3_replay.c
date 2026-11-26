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

#include <string.h>

#include "h3_replay.h"

#define H3_REPLAY_INDEX_NONE ((apr_uint32_t) -1)

typedef struct h3_replay_entry
{
    unsigned char key[H3_REPLAY_KEY_SIZE];
    apr_size_t key_len;
    apr_time_t expires;
    apr_uint32_t hash_next;
    apr_uint32_t order_prev;
    apr_uint32_t order_next;
    apr_uint32_t free_next;
} h3_replay_entry;

struct h3_replay_cache
{
    apr_pool_t* pool;
    apr_thread_mutex_t* lock;
    h3_replay_entry* entries;
    apr_uint32_t* table;
    apr_size_t capacity;
    apr_size_t table_size;
    apr_interval_time_t window;
    h3_replay_full_policy full_policy;
    apr_size_t size;
    apr_uint32_t order_head;
    apr_uint32_t order_tail;
    apr_uint32_t free_head;
    h3_replay_stats stats;
};

static apr_uint64_t replay_hash(const unsigned char* key, apr_size_t key_len)
{
    apr_uint64_t hash = 1469598103934665603ULL;
    apr_size_t i;

    for (i = 0; i < key_len; i++)
    {
        hash ^= key[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static apr_size_t replay_bucket(const h3_replay_cache* cache,
                                const unsigned char* key,
                                apr_size_t key_len)
{
    return (apr_size_t)(replay_hash(key, key_len) & ((apr_uint64_t)cache->table_size - 1));
}

static void order_unlink(h3_replay_cache* cache, apr_uint32_t idx)
{
    h3_replay_entry* entry = &cache->entries[idx];

    if (entry->order_prev != H3_REPLAY_INDEX_NONE)
    {
        cache->entries[entry->order_prev].order_next = entry->order_next;
    }
    else
    {
        cache->order_head = entry->order_next;
    }

    if (entry->order_next != H3_REPLAY_INDEX_NONE)
    {
        cache->entries[entry->order_next].order_prev = entry->order_prev;
    }
    else
    {
        cache->order_tail = entry->order_prev;
    }

    entry->order_prev = H3_REPLAY_INDEX_NONE;
    entry->order_next = H3_REPLAY_INDEX_NONE;
}

static void order_push_head(h3_replay_cache* cache, apr_uint32_t idx)
{
    h3_replay_entry* entry = &cache->entries[idx];

    entry->order_prev = H3_REPLAY_INDEX_NONE;
    entry->order_next = cache->order_head;
    if (cache->order_head != H3_REPLAY_INDEX_NONE)
    {
        cache->entries[cache->order_head].order_prev = idx;
    }
    cache->order_head = idx;
    if (cache->order_tail == H3_REPLAY_INDEX_NONE)
    {
        cache->order_tail = idx;
    }
}

static void hash_unlink(h3_replay_cache* cache, apr_uint32_t idx)
{
    h3_replay_entry* entry = &cache->entries[idx];
    apr_size_t bucket = replay_bucket(cache, entry->key, entry->key_len);
    apr_uint32_t* link = &cache->table[bucket];

    while (*link != H3_REPLAY_INDEX_NONE)
    {
        if (*link == idx)
        {
            *link = entry->hash_next;
            entry->hash_next = H3_REPLAY_INDEX_NONE;
            return;
        }
        link = &cache->entries[*link].hash_next;
    }
}

static void release_index(h3_replay_cache* cache, apr_uint32_t idx)
{
    h3_replay_entry* entry = &cache->entries[idx];

    hash_unlink(cache, idx);
    order_unlink(cache, idx);
    memset(entry, 0, sizeof(*entry));
    entry->hash_next = H3_REPLAY_INDEX_NONE;
    entry->order_prev = H3_REPLAY_INDEX_NONE;
    entry->order_next = H3_REPLAY_INDEX_NONE;
    entry->free_next = cache->free_head;
    cache->free_head = idx;
    cache->size--;
}

static apr_uint32_t find_entry(h3_replay_cache* cache,
                               const unsigned char* key,
                               apr_size_t key_len,
                               apr_size_t bucket)
{
    apr_uint32_t idx;

    for (idx = cache->table[bucket];
         idx != H3_REPLAY_INDEX_NONE;
         idx = cache->entries[idx].hash_next)
    {
        h3_replay_entry* entry = &cache->entries[idx];
        if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0)
        {
            return idx;
        }
    }
    return H3_REPLAY_INDEX_NONE;
}

static void expire_oldest(h3_replay_cache* cache, apr_time_t now)
{
    while (cache->order_tail != H3_REPLAY_INDEX_NONE)
    {
        apr_uint32_t idx = cache->order_tail;
        if (cache->entries[idx].expires > now)
        {
            return;
        }
        cache->stats.expired++;
        release_index(cache, idx);
    }
}

static h3_replay_seen_result insert_new(h3_replay_cache* cache,
                                        const unsigned char* key,
                                        apr_size_t key_len,
                                        apr_time_t now)
{
    apr_uint32_t idx;
    apr_size_t bucket;

    expire_oldest(cache, now);

    if (cache->free_head != H3_REPLAY_INDEX_NONE)
    {
        idx = cache->free_head;
        cache->free_head = cache->entries[idx].free_next;
    }
    else if (cache->full_policy == H3_REPLAY_FULL_EVICT_OLDEST
             && cache->order_tail != H3_REPLAY_INDEX_NONE)
    {
        idx = cache->order_tail;
        cache->stats.evicted++;
        release_index(cache, idx);
        idx = cache->free_head;
        cache->free_head = cache->entries[idx].free_next;
    }
    else
    {
        cache->stats.rejected_full++;
        return H3_REPLAY_SEEN_FULL;
    }

    h3_replay_entry* entry = &cache->entries[idx];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->key, key, key_len);
    entry->key_len = key_len;
    entry->expires = now + cache->window;
    entry->hash_next = H3_REPLAY_INDEX_NONE;
    entry->order_prev = H3_REPLAY_INDEX_NONE;
    entry->order_next = H3_REPLAY_INDEX_NONE;
    entry->free_next = H3_REPLAY_INDEX_NONE;

    bucket = replay_bucket(cache, key, key_len);
    entry->hash_next = cache->table[bucket];
    cache->table[bucket] = idx;
    order_push_head(cache, idx);
    cache->size++;
    cache->stats.entries = cache->size;
    cache->stats.misses++;
    return H3_REPLAY_SEEN_NEW;
}

apr_status_t h3_replay_cache_create(h3_replay_cache** out_cache,
                                    apr_pool_t* pool,
                                    apr_size_t capacity,
                                    apr_interval_time_t window,
                                    h3_replay_full_policy full_policy)
{
    h3_replay_cache* cache;
    apr_size_t table_size = 1;
    apr_size_t i;
    apr_status_t rv;

    if (!out_cache || !pool || capacity == 0 || window <= 0)
    {
        return APR_EINVAL;
    }

    while (table_size < capacity)
    {
        if (table_size > ((apr_size_t)1 << (sizeof(apr_size_t) * 8 - 1)))
        {
            return APR_EINVAL;
        }
        table_size <<= 1;
    }

    cache = apr_pcalloc(pool, sizeof(*cache));
    cache->pool = pool;
    cache->capacity = capacity;
    cache->table_size = table_size;
    cache->window = window;
    cache->full_policy = full_policy;
    cache->order_head = H3_REPLAY_INDEX_NONE;
    cache->order_tail = H3_REPLAY_INDEX_NONE;
    cache->free_head = 0;
    cache->stats.capacity = capacity;

    rv = apr_thread_mutex_create(&cache->lock, APR_THREAD_MUTEX_DEFAULT, pool);
    if (rv != APR_SUCCESS)
    {
        return rv;
    }

    cache->entries = apr_pcalloc(pool, capacity * sizeof(cache->entries[0]));
    cache->table = apr_pcalloc(pool, table_size * sizeof(cache->table[0]));
    if (!cache->entries || !cache->table)
    {
        return APR_ENOMEM;
    }

    for (i = 0; i < table_size; i++)
    {
        cache->table[i] = H3_REPLAY_INDEX_NONE;
    }
    for (i = 0; i < capacity; i++)
    {
        cache->entries[i].hash_next = H3_REPLAY_INDEX_NONE;
        cache->entries[i].order_prev = H3_REPLAY_INDEX_NONE;
        cache->entries[i].order_next = H3_REPLAY_INDEX_NONE;
        cache->entries[i].free_next = (i + 1 < capacity) ? (apr_uint32_t)(i + 1)
                                                        : H3_REPLAY_INDEX_NONE;
    }

    *out_cache = cache;
    return APR_SUCCESS;
}

h3_replay_seen_result h3_replay_cache_see(h3_replay_cache* cache,
                                          const unsigned char* key,
                                          apr_size_t key_len,
                                          apr_time_t now)
{
    apr_size_t bucket;
    apr_uint32_t idx;
    h3_replay_seen_result result;

    if (!cache || !key || key_len == 0 || key_len > H3_REPLAY_KEY_SIZE)
    {
        return H3_REPLAY_SEEN_FULL;
    }
    if (now == 0)
    {
        now = apr_time_now();
    }

    apr_thread_mutex_lock(cache->lock);
    bucket = replay_bucket(cache, key, key_len);
    idx = find_entry(cache, key, key_len, bucket);
    if (idx != H3_REPLAY_INDEX_NONE)
    {
        if (cache->entries[idx].expires > now)
        {
            cache->stats.hits++;
            result = H3_REPLAY_SEEN_DUPLICATE;
            goto done;
        }
        cache->stats.expired++;
        release_index(cache, idx);
    }

    result = insert_new(cache, key, key_len, now);

done:
    cache->stats.entries = cache->size;
    apr_thread_mutex_unlock(cache->lock);
    return result;
}

void h3_replay_cache_get_stats(h3_replay_cache* cache, h3_replay_stats* stats)
{
    if (!stats)
    {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!cache)
    {
        return;
    }
    apr_thread_mutex_lock(cache->lock);
    *stats = cache->stats;
    stats->entries = cache->size;
    apr_thread_mutex_unlock(cache->lock);
}

void h3_replay_cache_clear(h3_replay_cache* cache)
{
    apr_size_t i;

    if (!cache)
    {
        return;
    }
    apr_thread_mutex_lock(cache->lock);
    cache->size = 0;
    cache->order_head = H3_REPLAY_INDEX_NONE;
    cache->order_tail = H3_REPLAY_INDEX_NONE;
    cache->free_head = 0;
    for (i = 0; i < cache->table_size; i++)
    {
        cache->table[i] = H3_REPLAY_INDEX_NONE;
    }
    for (i = 0; i < cache->capacity; i++)
    {
        memset(&cache->entries[i], 0, sizeof(cache->entries[i]));
        cache->entries[i].hash_next = H3_REPLAY_INDEX_NONE;
        cache->entries[i].order_prev = H3_REPLAY_INDEX_NONE;
        cache->entries[i].order_next = H3_REPLAY_INDEX_NONE;
        cache->entries[i].free_next = (i + 1 < cache->capacity)
                                          ? (apr_uint32_t)(i + 1)
                                          : H3_REPLAY_INDEX_NONE;
    }
    cache->stats.entries = 0;
    apr_thread_mutex_unlock(cache->lock);
}
