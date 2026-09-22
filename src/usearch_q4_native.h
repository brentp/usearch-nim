#ifndef USEARCH_Q4_NATIVE_H
#define USEARCH_Q4_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    USEARCH_Q4_DIMENSIONS = 3072,
    USEARCH_Q4_PACKED_BYTES = 1536,
    USEARCH_Q4_RECORD_BYTES = 1540,
};

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

int usearch_q4_search_record(
    usearch_q4_index_t const* index, uint8_t const* record,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity);

int usearch_q4_exact_search_record(
    usearch_q4_index_t const* index, uint8_t const* record,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity);

int usearch_q4_search_by_id(
    usearch_q4_index_t const* index, uint32_t id,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity);

int usearch_q4_exact_search_by_id(
    usearch_q4_index_t const* index, uint32_t id,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
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
