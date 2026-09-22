#include "usearch_q4_native.h"

#include <usearch/index_dense.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

#if !defined(USEARCH_Q4_DISABLE_AVX2) && (defined(__GNUC__) || defined(__clang__)) &&                     \
    (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define USEARCH_Q4_CAN_DISPATCH_AVX2 1
#else
#define USEARCH_Q4_CAN_DISPATCH_AVX2 0
#endif

using namespace unum::usearch;

using usearch_q4_dense_t = index_dense_gt<std::uint32_t, std::uint32_t>;

struct usearch_q4_index {
    usearch_q4_dense_t index;
    bool memory_mapped = false;
};

namespace {

// Thread context every insertion uses; see `usearch_q4_add`.
static constexpr std::size_t usearch_q4_add_thread_k = 0;

static void clear_error(char* error, std::size_t capacity) noexcept {
    if (error && capacity)
        error[0] = '\0';
}

static void set_error(char* error, std::size_t capacity, char const* message) noexcept {
    if (!error || !capacity)
        return;
    std::snprintf(error, capacity, "%s", message ? message : "unknown USearch error");
}

// USearch hands out a fixed pool of search slots sized by `threads_search`, and
// reports an exhausted pool with a message that blames index capacity instead of
// the real cause. Translate it, naming the limit the caller actually has to
// raise. The literal is stable for the pinned 2.26.2 headers under `vendor/`;
// any other message passes through untouched.
static char const* const usearch_thread_pool_message = "Reserve capacity ahead of searches!";

static void set_search_error(char const* message, std::size_t search_threads, char* error,
                             std::size_t error_capacity) noexcept {
    if (!message || std::strcmp(message, usearch_thread_pool_message) != 0) {
        set_error(error, error_capacity, message);
        return;
    }
    if (!error || !error_capacity)
        return;
    std::snprintf(error, error_capacity,
                  "no free search slot: this index allows %zu concurrent search%s; open it "
                  "with search_threads at least as large as the number of threads that "
                  "search at the same time",
                  search_threads, search_threads == 1 ? "" : "es");
}

static int decode_nibble(std::uint8_t value) noexcept {
    // Sign-extend a four-bit two's-complement integer without a branch.
    return static_cast<std::int8_t>(value << 4) >> 4;
}

static std::uint32_t read_u32_le(std::uint8_t const* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

static std::int32_t packed_dot_scalar(std::uint8_t const* first,
                                     std::uint8_t const* second) noexcept {
    std::int32_t dot = 0;
    for (std::size_t i = 0; i != USEARCH_Q4_PACKED_BYTES; ++i) {
        std::uint8_t a = first[i];
        std::uint8_t b = second[i];
        dot += decode_nibble(a & 0x0f) * decode_nibble(b & 0x0f);
        dot += decode_nibble(a >> 4) * decode_nibble(b >> 4);
    }
    return dot;
}

#if USEARCH_Q4_CAN_DISPATCH_AVX2
__attribute__((target("avx2")))
static std::int32_t packed_dot_avx2(std::uint8_t const* first,
                                   std::uint8_t const* second) noexcept {
    __m256i total = _mm256_setzero_si256();
    __m256i const nibble_mask = _mm256_set1_epi8(0x0f);
    __m256i const sign_bit = _mm256_set1_epi8(0x08);

    for (std::size_t offset = 0; offset != USEARCH_Q4_PACKED_BYTES; offset += 32) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(first + offset));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(second + offset));
        __m256i a_low = _mm256_and_si256(a, nibble_mask);
        __m256i b_low = _mm256_and_si256(b, nibble_mask);
        __m256i a_high = _mm256_and_si256(_mm256_srli_epi16(a, 4), nibble_mask);
        __m256i b_high = _mm256_and_si256(_mm256_srli_epi16(b, 4), nibble_mask);

        a_low = _mm256_sub_epi8(_mm256_xor_si256(a_low, sign_bit), sign_bit);
        b_low = _mm256_sub_epi8(_mm256_xor_si256(b_low, sign_bit), sign_bit);
        a_high = _mm256_sub_epi8(_mm256_xor_si256(a_high, sign_bit), sign_bit);
        b_high = _mm256_sub_epi8(_mm256_xor_si256(b_high, sign_bit), sign_bit);

#define USEARCH_Q4_ACCUMULATE_16(a8, b8)                                                               \
    total = _mm256_add_epi32(total, _mm256_madd_epi16(_mm256_cvtepi8_epi16((a8)),                    \
                                                       _mm256_cvtepi8_epi16((b8))))
        USEARCH_Q4_ACCUMULATE_16(_mm256_castsi256_si128(a_low), _mm256_castsi256_si128(b_low));
        USEARCH_Q4_ACCUMULATE_16(_mm256_extracti128_si256(a_low, 1), _mm256_extracti128_si256(b_low, 1));
        USEARCH_Q4_ACCUMULATE_16(_mm256_castsi256_si128(a_high), _mm256_castsi256_si128(b_high));
        USEARCH_Q4_ACCUMULATE_16(_mm256_extracti128_si256(a_high, 1), _mm256_extracti128_si256(b_high, 1));
#undef USEARCH_Q4_ACCUMULATE_16
    }

    __m128i sum = _mm_add_epi32(_mm256_castsi256_si128(total), _mm256_extracti128_si256(total, 1));
    sum = _mm_hadd_epi32(sum, sum);
    sum = _mm_hadd_epi32(sum, sum);
    return _mm_cvtsi128_si32(sum);
}
#endif

using packed_dot_t = std::int32_t (*)(std::uint8_t const*, std::uint8_t const*) noexcept;

static packed_dot_t choose_packed_dot() noexcept {
#if USEARCH_Q4_CAN_DISPATCH_AVX2
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2"))
        return packed_dot_avx2;
#endif
    return packed_dot_scalar;
}

static packed_dot_t selected_packed_dot() noexcept {
    static packed_dot_t implementation = choose_packed_dot();
    return implementation;
}

static std::int32_t packed_dot(std::uint8_t const* first,
                              std::uint8_t const* second) noexcept {
    return selected_packed_dot()(first, second);
}

static float packed_cosine(std::uint8_t const* first, std::uint8_t const* second) noexcept {
    std::uint32_t first_norm2 = read_u32_le(first + USEARCH_Q4_PACKED_BYTES);
    std::uint32_t second_norm2 = read_u32_le(second + USEARCH_Q4_PACKED_BYTES);
    if (!first_norm2 || !second_norm2)
        return -1.0f;
    double denominator = std::sqrt(static_cast<double>(first_norm2) * static_cast<double>(second_norm2));
    return static_cast<float>(static_cast<double>(packed_dot(first, second)) / denominator);
}

static float packed_distance(void const* first, void const* second) noexcept {
    return 1.0f - packed_cosine(static_cast<std::uint8_t const*>(first),
                               static_cast<std::uint8_t const*>(second));
}

static metric_punned_t make_metric() noexcept {
    return metric_punned_t::stateless(
        USEARCH_Q4_RECORD_BYTES,
        reinterpret_cast<std::uintptr_t>(&packed_distance),
        metric_punned_signature_t::array_array_k,
        metric_kind_t::cos_k, scalar_kind_t::i8_k);
}

static bool validate_record(std::uint8_t const* record, char* error,
                            std::size_t error_capacity) noexcept {
    if (!record) {
        set_error(error, error_capacity, "packed Q4 record is null");
        return false;
    }
    std::uint32_t observed_norm2 = 0;
    for (std::size_t i = 0; i != USEARCH_Q4_PACKED_BYTES; ++i) {
        std::uint8_t packed = record[i];
        std::uint8_t low = packed & 0x0f;
        std::uint8_t high = packed >> 4;
        if (low == 8 || high == 8) {
            set_error(error, error_capacity, "packed Q4 record contains the unused -8 code");
            return false;
        }
        int low_value = decode_nibble(low);
        int high_value = decode_nibble(high);
        observed_norm2 += static_cast<std::uint32_t>(low_value * low_value + high_value * high_value);
    }
    std::uint32_t stored_norm2 = read_u32_le(record + USEARCH_Q4_PACKED_BYTES);
    if (!observed_norm2) {
        set_error(error, error_capacity, "packed Q4 record has zero norm");
        return false;
    }
    if (stored_norm2 != observed_norm2) {
        set_error(error, error_capacity, "packed Q4 record has an incorrect squared norm");
        return false;
    }
    return true;
}

static bool restore_metric(usearch_q4_dense_t& index, char* error,
                           std::size_t error_capacity) noexcept {
    if (index.dimensions() != USEARCH_Q4_RECORD_BYTES || index.scalar_kind() != scalar_kind_t::i8_k) {
        set_error(error, error_capacity, "index is not a packed-Q4 index");
        return false;
    }
    if (!index.try_change_metric(make_metric())) {
        set_error(error, error_capacity, "failed to restore the packed-Q4 metric");
        return false;
    }
    return true;
}

static bool validate_sequential_ids(usearch_q4_dense_t const& index, char* error,
                                    std::size_t error_capacity) noexcept {
    std::uint32_t expected = 0;
    for (auto iterator = index.cbegin(); iterator != index.cend(); ++iterator) {
        auto member = *iterator;
        if (static_cast<std::uint32_t>(member.key) != expected) {
            set_error(error, error_capacity, "index IDs are not contiguous zero-based sample IDs");
            return false;
        }
        ++expected;
    }
    return true;
}

static usearch_q4_index_t* make_empty(
    std::size_t capacity, std::size_t connectivity, std::size_t expansion_add,
    std::size_t expansion_search, std::size_t search_threads,
    char* error, std::size_t error_capacity) noexcept {
    if (capacity >= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        set_error(error, error_capacity, "capacity exceeds the uint32 sample-ID limit");
        return nullptr;
    }
    index_dense_config_t config;
    config.connectivity = connectivity;
    config.expansion_add = expansion_add;
    config.expansion_search = expansion_search;
    config.multi = false;
    config.enable_key_lookups = false;

    index_limits_t limits;
    limits.members = capacity;
    limits.threads_add = 1;
    limits.threads_search = (std::max<std::size_t>)(1, search_threads);
    auto state = usearch_q4_dense_t::make(make_metric(), config,
                                        (std::numeric_limits<std::uint32_t>::max)(), limits);
    if (!state) {
        set_error(error, error_capacity, state.error.release());
        return nullptr;
    }
    usearch_q4_index_t* result = new (std::nothrow) usearch_q4_index_t;
    if (!result) {
        set_error(error, error_capacity, "out of memory allocating the index wrapper");
        return nullptr;
    }
    result->index = std::move(state.index);
    return result;
}

static std::uint8_t const* record_at(usearch_q4_dense_t const& index, std::uint32_t id) noexcept {
    usearch_q4_dense_t::values_proxy_t values(index);
    return reinterpret_cast<std::uint8_t const*>(values[static_cast<std::uint32_t>(id)]);
}

static bool dump_search(usearch_q4_dense_t::search_result_t& result, std::uint32_t excluded,
                        bool exclude, std::size_t wanted, std::uint32_t* ids,
                        float* distances, std::size_t* found) noexcept {
    std::size_t output_count = 0;
    for (std::size_t i = 0; i != result.size() && output_count != wanted; ++i) {
        auto match = result[i];
        std::uint32_t id = static_cast<std::uint32_t>(match.member.key);
        if (exclude && id == excluded)
            continue;
        ids[output_count] = id;
        distances[output_count] = match.distance;
        ++output_count;
    }
    *found = output_count;
    return true;
}

} // namespace

extern "C" {

char const* usearch_q4_version(void) {
    return "2.26.2";
}

char const* usearch_q4_metric_implementation(void) {
#if USEARCH_Q4_CAN_DISPATCH_AVX2
    return selected_packed_dot() == packed_dot_avx2 ? "avx2" : "scalar";
#else
    return "scalar";
#endif
}

usearch_q4_index_t* usearch_q4_create(
    size_t capacity, size_t connectivity, size_t expansion_add,
    size_t expansion_search, size_t search_threads,
    char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    try {
        return make_empty(capacity, connectivity, expansion_add, expansion_search,
                          search_threads, error, error_capacity);
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception creating USearch index");
        return nullptr;
    }
}

usearch_q4_index_t* usearch_q4_open(
    char const* path, int memory_map, size_t expansion_add,
    size_t expansion_search, size_t search_threads,
    char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!path) {
        set_error(error, error_capacity, "index path is null");
        return nullptr;
    }
    try {
        usearch_q4_index_t* result = make_empty(
            0, 32, expansion_add, expansion_search, search_threads, error, error_capacity);
        if (!result)
            return nullptr;
        auto loaded = memory_map ? result->index.view(path) : result->index.load(path);
        if (!loaded) {
            set_error(error, error_capacity, loaded.error.release());
            delete result;
            return nullptr;
        }
        result->index.change_expansion_add(expansion_add);
        result->index.change_expansion_search(expansion_search);
        if (!restore_metric(result->index, error, error_capacity) ||
            !validate_sequential_ids(result->index, error, error_capacity)) {
            delete result;
            return nullptr;
        }
        result->memory_mapped = memory_map != 0;
        return result;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return nullptr;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception opening USearch index");
        return nullptr;
    }
}

void usearch_q4_free(usearch_q4_index_t* index) {
    delete index;
}

int usearch_q4_reserve(usearch_q4_index_t* wrapper, size_t capacity,
                             char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper) {
        set_error(error, error_capacity, "index is null");
        return 0;
    }
    if (wrapper->memory_mapped) {
        set_error(error, error_capacity, "cannot reserve a memory-mapped index");
        return 0;
    }
    if (capacity >= static_cast<size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        set_error(error, error_capacity, "capacity exceeds the uint32 sample-ID limit");
        return 0;
    }
    try {
        index_limits_t limits = wrapper->index.limits();
        limits.members = capacity;
        if (!wrapper->index.try_reserve(limits)) {
            set_error(error, error_capacity, "failed to reserve index capacity");
            return 0;
        }
        return 1;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception reserving index capacity");
        return 0;
    }
}

int usearch_q4_add(usearch_q4_index_t* wrapper, uint8_t const* record,
                         uint32_t* assigned_id, char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper || !assigned_id) {
        set_error(error, error_capacity, "index or assigned-ID output is null");
        return 0;
    }
    if (wrapper->memory_mapped) {
        set_error(error, error_capacity, "cannot add to a memory-mapped index");
        return 0;
    }
    if (!validate_record(record, error, error_capacity))
        return 0;
    if (wrapper->index.size() >= wrapper->index.capacity()) {
        set_error(error, error_capacity, "index capacity exhausted; reserve more space first");
        return 0;
    }
    std::uint32_t id = static_cast<std::uint32_t>(wrapper->index.size());
    try {
        // Pin insertion to thread context 0 instead of letting USearch draw any
        // free slot. Each context owns a private `level_generator`, so the slot an
        // add happens to draw decides the node's HNSW level and therefore the
        // serialized graph. Drawing from the shared pool would make index bytes a
        // function of `search_threads` - and so of the host's core count. Adds are
        // single-threaded by construction (`threads_add = 1`), so context 0 is
        // uncontended as long as callers do not add while searches are in flight.
        auto added = wrapper->index.add(id, reinterpret_cast<i8_t const*>(record),
                                        usearch_q4_add_thread_k);
        if (!added) {
            set_error(error, error_capacity, added.error.release());
            return 0;
        }
        *assigned_id = id;
        return 1;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception adding to USearch index");
        return 0;
    }
}

static int search_record(
    usearch_q4_index_t const* wrapper, uint8_t const* record,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    bool exact, char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper || !found || (wanted && (!ids || !distances))) {
        set_error(error, error_capacity, "invalid search argument");
        return 0;
    }
    *found = 0;
    if (!wanted || !wrapper->index.size())
        return 1;
    if (!validate_record(record, error, error_capacity))
        return 0;
    try {
        std::size_t requested = (std::min)(wanted, wrapper->index.size());
        auto result = wrapper->index.search(
            reinterpret_cast<i8_t const*>(record), requested,
            usearch_q4_dense_t::any_thread(), exact);
        if (!result) {
            set_search_error(result.error.release(), wrapper->index.limits().threads_search,
                             error, error_capacity);
            return 0;
        }
        return dump_search(result, 0, false, wanted, ids, distances, found) ? 1 : 0;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception searching USearch index");
        return 0;
    }
}

int usearch_q4_search_record(
    usearch_q4_index_t const* wrapper, uint8_t const* record,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity) {
    return search_record(wrapper, record, wanted, ids, distances, found,
                         false, error, error_capacity);
}

int usearch_q4_exact_search_record(
    usearch_q4_index_t const* wrapper, uint8_t const* record,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity) {
    return search_record(wrapper, record, wanted, ids, distances, found,
                         true, error, error_capacity);
}

static int search_by_id(
    usearch_q4_index_t const* wrapper, uint32_t id,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    bool exact, char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper || !found || (wanted && (!ids || !distances))) {
        set_error(error, error_capacity, "invalid search argument");
        return 0;
    }
    *found = 0;
    if (id >= wrapper->index.size()) {
        set_error(error, error_capacity, "sample ID is outside the index");
        return 0;
    }
    if (!wanted || wrapper->index.size() <= 1)
        return 1;
    try {
        std::size_t requested = wanted >= wrapper->index.size() - 1
                                    ? wrapper->index.size()
                                    : wanted + 1;
        auto result = wrapper->index.search(
            reinterpret_cast<i8_t const*>(record_at(wrapper->index, id)), requested,
            usearch_q4_dense_t::any_thread(), exact);
        if (!result) {
            set_search_error(result.error.release(), wrapper->index.limits().threads_search,
                             error, error_capacity);
            return 0;
        }
        return dump_search(result, id, true, wanted, ids, distances, found) ? 1 : 0;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception searching USearch index");
        return 0;
    }
}

int usearch_q4_search_by_id(
    usearch_q4_index_t const* wrapper, uint32_t id,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity) {
    return search_by_id(wrapper, id, wanted, ids, distances, found,
                        false, error, error_capacity);
}

int usearch_q4_exact_search_by_id(
    usearch_q4_index_t const* wrapper, uint32_t id,
    size_t wanted, uint32_t* ids, float* distances, size_t* found,
    char* error, size_t error_capacity) {
    return search_by_id(wrapper, id, wanted, ids, distances, found,
                        true, error, error_capacity);
}

int usearch_q4_cosine_by_id(
    usearch_q4_index_t const* wrapper, uint32_t first, uint32_t second,
    float* cosine, char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper || !cosine) {
        set_error(error, error_capacity, "index or cosine output is null");
        return 0;
    }
    if (first >= wrapper->index.size() || second >= wrapper->index.size()) {
        set_error(error, error_capacity, "sample ID is outside the index");
        return 0;
    }
    *cosine = packed_cosine(record_at(wrapper->index, first), record_at(wrapper->index, second));
    return 1;
}

float usearch_q4_cosine(uint8_t const* first, uint8_t const* second) {
    return first && second ? packed_cosine(first, second) : -1.0f;
}

int usearch_q4_validate(uint8_t const* record, char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    return validate_record(record, error, error_capacity) ? 1 : 0;
}

int usearch_q4_save(usearch_q4_index_t const* wrapper, char const* path,
                          char* error, size_t error_capacity) {
    clear_error(error, error_capacity);
    if (!wrapper || !path) {
        set_error(error, error_capacity, "index or save path is null");
        return 0;
    }
    try {
        auto saved = wrapper->index.save(path);
        if (!saved) {
            set_error(error, error_capacity, saved.error.release());
            return 0;
        }
        return 1;
    } catch (std::exception const& exception) {
        set_error(error, error_capacity, exception.what());
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "unknown exception saving USearch index");
        return 0;
    }
}

size_t usearch_q4_size(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.size() : 0;
}

size_t usearch_q4_capacity(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.capacity() : 0;
}

size_t usearch_q4_connectivity(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.connectivity() : 0;
}

size_t usearch_q4_expansion_add(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.expansion_add() : 0;
}

size_t usearch_q4_expansion_search(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.expansion_search() : 0;
}

size_t usearch_q4_search_threads(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.limits().threads_search : 0;
}

size_t usearch_q4_serialized_bytes(usearch_q4_index_t const* wrapper) {
    return wrapper ? wrapper->index.serialized_length() : 0;
}

usearch_q4_memory_stats_t usearch_q4_memory(
    usearch_q4_index_t const* wrapper) {
    usearch_q4_memory_stats_t result{};
    if (!wrapper)
        return result;
    auto stats = wrapper->index.memory_stats();
    result.total_bytes = wrapper->index.memory_usage();
    result.graph_allocated_bytes = stats.graph_allocated;
    result.graph_reserved_bytes = stats.graph_reserved;
    result.vectors_allocated_bytes = stats.vectors_allocated;
    result.vectors_reserved_bytes = stats.vectors_reserved;
    return result;
}

} // extern "C"
