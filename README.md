# Minimal packed-Q4 USearch wrapper

This directory is self-contained and can be moved into another repository. It
wraps USearch 2.26.2 for a packed-Q4 nearest-neighbor prefilter and
intentionally does not expose USearch's general vector API.

## Packed record

Each record contains exactly 3,072 signed Q4 coordinates:

- two two's-complement values from `-7` through `+7` per byte;
- low nibble first, then high nibble;
- nibble `0x8` is invalid because `-8` is outside the quantizer;
- 1,536 coordinate bytes followed by a little-endian `uint32` sum of squares;
- 1,540 bytes total.

The native metric accumulates the signed dot product in `int32` and returns
`1 - dot/sqrt(sumsq_a*sumsq_b)`. USearch stores the record as opaque `i8`
data; its built-in `i8` quantizer and cosine metric are never used.

## SIMD dispatch

**No compiler flags are needed to get AVX2.** The AVX2 kernel carries its own
`__attribute__((target("avx2")))`, so it compiles under any x86 GCC or Clang
whatever the `-march` baseline, and `choose_packed_dot` picks it once per
process via `__builtin_cpu_supports("avx2")`. A plain `nim c -d:release` build
therefore runs AVX2 on an AVX2 host and the scalar kernel elsewhere, from one
binary. Both kernels compute the same integer dot product, and every distance
goes through the same selection: approximate search, exact search, `cosine`,
and `cosineById` alike.

Call `metricImplementation()` to see which kernel is live; it returns `"avx2"`
or `"scalar"`.

Do **not** add `-march=native`, `-mavx2`, or similar. They buy nothing here and
they break the fallback: on an AVX-512 host, `-march=native` auto-vectorizes
`packed_dot_scalar` itself into AVX-512, so the runtime dispatch still selects
"scalar" on an older processor and then faults with SIGILL on the very machines
that path exists to serve. Leave the architecture baseline alone.

Compile with `-d:UsearchScalarQ4` to force the scalar kernel for validation;
that is how the two implementations get cross-checked on an AVX2 host. The
equivalent macro for building `usearch_q4_native.cpp` directly is
`-DUSEARCH_Q4_DISABLE_AVX2=1`. On non-x86 targets, and on compilers other than
GCC and Clang (MSVC included), the dispatch compiles out and the scalar kernel
is always used.

## API

`src/usearch_q4.nim` provides:

- `packQ4`, `unpackQ4`, `fromBytes`, `toBytes`, and exact packed cosine;
- `createQ4Index` and `openQ4Index`;
- sequential `add`, assigning zero-based `uint32` sample IDs;
- approximate `search` and `searchById`, with self removed from ID queries;
- exhaustive `exactSearch` and `exactSearchById`, also with self removed from
  ID queries;
- exact `cosineById` for candidate admission;
- `metricImplementation` and `version` for the live SIMD kernel and pinned
  USearch release;
- reserve, save, load, memory-mapped view, and memory/size metadata, including
  `searchThreads` for the effective concurrent-search limit.

Sequential IDs let the wrapper disable USearch's key-to-slot hash table. They
also allow `searchById` without retaining a second copy of all packed records.
Indexes containing nonsequential keys are rejected on open.

Construction uses one add thread and fixed insertion order. Every insertion is
pinned to USearch thread context 0, because each context owns a private RNG
that chooses a node's HNSW level: letting an add draw any free slot would make
the serialized graph a function of `searchThreads`, and so of the building
host's core count. The tests require byte-identical serialized indexes from
repeated builds and across `searchThreads` values. Cross-toolchain
reproducibility still requires validation.

## Concurrent search

USearch serves each in-flight search from a fixed pool of slots sized at
create/open time by `searchThreads`. A search that finds no free slot fails
outright rather than queuing, so an undersized pool turns into a flood of
errors rather than contention. `createQ4Index` and `openQ4Index` therefore
default `searchThreads` to `defaultSearchThreads()`, one slot per detected
core; a slot costs roughly 33 KB regardless of index size. Raise it when more
threads than that search one index at once, and read back the effective value
with `index.searchThreads`. Adding is always single-threaded.

The selected defaults are `M=32`, `efConstruction=400`, and `efSearch=80`.
Exact searches scan the full index and are intended for validation or small
cohorts; they do not use the HNSW graph and take linear time per query.
Reciprocal top-40 filtering, the cosine floors, positional rescue, candidate
deduplication, panel metadata, and exact relatedness scoring remain outside this
wrapper.

## Build and test

The Nim module compiles the C++ adapter and links the C++ standard library
(`-lc++` on macOS, `-lstdc++` elsewhere). No installed USearch library is
required, and the module stays importable from a C-backend Nim project.

```bash
cd usearch-nim
nimble test
```

The test task selects Clang because this development host has `clang++` but no
`g++` command. The module itself does not depend on Clang; remove `--cc:clang`
from the task when using Nim's default configured C++ compiler.

A synthetic sizing and throughput smoke benchmark is available as:

```bash
nim c --cc:clang -d:release -r bench/bench_q4.nim 10000
```

On the development host, the 10,000-sample synthetic run with the selected
production parameters took 37.585 seconds to build and 16.486 seconds to query
all 40-neighbor lists. USearch reported 33,634,560 bytes in memory and an
18,122,748-byte serialized index, or about 1,812 serialized bytes per sample.
The workload uses random sketches, so these are implementation smoke-test
figures rather than a production throughput forecast.

The three vendored USearch headers and license are under `src/vendor/`, inside
`srcDir` so that `nimble install` ships them with the module. The pinned
upstream version is recorded in `src/vendor/USearch-VERSION`.

## Persistence constraints

USearch serializes its built-in metric identifier, not a custom function
pointer. `openQ4Index` therefore restores the packed-Q4 metric after every load
or memory-mapped view and verifies the record size, scalar type, and sequential
IDs before returning the index.

The caller should save to a temporary file and atomically rename it. A
caller-side sidecar still needs to record and validate the panel, allele
frequencies, projection version and seed, caller settings, sample manifest,
HNSW parameters, and checksums.

USearch is Apache-2.0 licensed; the wrapper source is MIT-licensed. Retain
`src/vendor/USearch-LICENSE` when redistributing it.
