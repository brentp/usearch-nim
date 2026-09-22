#ifndef USEARCH_Q4_NATIVE_H
#define USEARCH_Q4_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Coordinates per record. Override at build time, for example
   -DUSEARCH_Q4_DIMENSIONS=4096. Must be positive and even, because two
   coordinates share each packed byte. When building through the Nim module use
   -d:usearchQ4Dimensions=N instead: it sets both sides from one value.

   The choice is fixed per build, not per index. Records are fixed-size values
   and the metric is compiled around this count. An index serialized by one
   build is rejected by a build with a different setting, because the record
   size is checked on open. */
#ifndef USEARCH_Q4_DIMENSIONS
#define USEARCH_Q4_DIMENSIONS 3072
#endif

#if (USEARCH_Q4_DIMENSIONS) <= 0 || ((USEARCH_Q4_DIMENSIONS) % 2) != 0
#error "USEARCH_Q4_DIMENSIONS must be a positive even number"
#endif

enum {
    USEARCH_Q4_PACKED_BYTES = (USEARCH_Q4_DIMENSIONS) / 2,
    USEARCH_Q4_RECORD_BYTES = USEARCH_Q4_PACKED_BYTES + 4,
};

/* Report the values this build was compiled with, so a caller can verify that
   its own constants agree. */
size_t usearch_q4_dimensions(void);
size_t usearch_q4_record_bytes(void);

typedef struct usearch_q4_index usearch_q4_index_t;

typedef struct usearch_q4_memory_stats {
    size_t total_bytes;
    size_t graph_allocated_bytes;
    size_t graph_reserved_bytes;
    size_t vectors_allocated_bytes;
    size_t vectors_reserved_bytes;
} usearch_q4_memory_stats_t;

char const* usearch_q4_version(void);
char const* usearch_q4_metric_implementation(void);

usearch_q4_index_t* usearch_q4_create(
    size_t capacity, size_t connectivity, size_t expansion_add,
    size_t expansion_search, size_t search_threads,
    char* error, size_t error_capacity);

usearch_q4_index_t* usearch_q4_open(
    char const* path, int memory_map, size_t expansion_add,
    size_t expansion_search, size_t search_threads,
    char* error, size_t error_capacity);

void usearch_q4_free(usearch_q4_index_t* index);

int usearch_q4_reserve(
    usearch_q4_index_t* index, size_t capacity,
    char* error, size_t error_capacity);

int usearch_q4_add(
    usearch_q4_index_t* index, uint8_t const* record,
    uint32_t* assigned_id, char* error, size_t error_capacity);

/* exact != 0 scans every record instead of walking the HNSW graph. */
int usearch_q4_search_record(
    usearch_q4_index_t const* index, uint8_t const* record,
    size_t wanted, int exact, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity);

int usearch_q4_search_by_id(
    usearch_q4_index_t const* index, uint32_t id,
    size_t wanted, int exact, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity);

int usearch_q4_cosine_by_id(
    usearch_q4_index_t const* index, uint32_t first, uint32_t second,
    float* cosine, char* error, size_t error_capacity);

float usearch_q4_cosine(uint8_t const* first, uint8_t const* second);
int usearch_q4_validate(uint8_t const* record, char* error, size_t error_capacity);

int usearch_q4_save(
    usearch_q4_index_t const* index, char const* path,
    char* error, size_t error_capacity);

size_t usearch_q4_size(usearch_q4_index_t const* index);
size_t usearch_q4_capacity(usearch_q4_index_t const* index);
size_t usearch_q4_connectivity(usearch_q4_index_t const* index);
size_t usearch_q4_expansion_add(usearch_q4_index_t const* index);
size_t usearch_q4_expansion_search(usearch_q4_index_t const* index);
size_t usearch_q4_search_threads(usearch_q4_index_t const* index);
size_t usearch_q4_serialized_bytes(usearch_q4_index_t const* index);
usearch_q4_memory_stats_t usearch_q4_memory(
    usearch_q4_index_t const* index);

#ifdef __cplusplus
}
#endif

#endif
