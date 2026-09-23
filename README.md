# usearch-q4

A minimal Nim wrapper around [USearch](https://github.com/unum-cloud/usearch)
for 4-bit-quantized vectors, with an exact integer cosine metric. It does not
expose USearch's general vector API.

```nim
import usearch_q4

proc sampleVector(s: int): Q4Record =
  # Coordinates are signed 4-bit values
  # we just generate fake data here.
  var coords = newSeq[int8](Q4Dimensions)
  for i in 0 ..< Q4Dimensions:
    coords[i] = int8((i * (s + 1)) mod 15 - 7)
  packQ4(coords)

var index = createQ4Index(1000)
for id in 0 ..< 1000:
  doAssert index.add(sampleVector(id)) == uint32(id)  # IDs assigned 0, 1, 2, ...

for n in index.searchById(0, 3):   # nearest to a stored record, itself excluded
  echo n.id, "  ", n.similarity    # similarity is 1 - distance

# Query with a vector that is not in the index. Pack it the same way, then
# hand the record to `search`; nothing is excluded, so all 3 hits are stored
# records.
let probe = sampleVector(4242)
for n in index.search(probe, 3):
  echo n.id, "  ", n.similarity

# `exact = true` scores every record instead of walking the graph, giving the
# true top 3 in linear time. Both search procs take it.
for n in index.search(probe, 3, exact = true):
  echo n.id, "  ", n.similarity

echo index.cosineById(0, 1)        # exact cosine, not the graph's estimate

index.save("vectors.usearch")
var reopened = openQ4Index("vectors.usearch", memoryMap = true)
```

## Packed record

Each record holds `Q4Dimensions` coordinates, two per byte — low nibble first,
values `-7` to `+7` (`0x8` is invalid) — followed by a little-endian `uint32`
sum of squares. The metric accumulates the dot product in `int32` and returns
`1 - dot/sqrt(sumsq_a*sumsq_b)`. USearch stores records as opaque `i8` bytes;
its own `i8` quantizer and cosine metric are never used.

`Q4Dimensions` defaults to 4096 and is set at build time with
`-d:usearchQ4Dimensions=N`, which must be positive and even. One define drives
both the Nim and C++ sides. It is fixed per build rather than per index,
because records are fixed-size values and the metric compiles around the count;
an index is rejected on open by a build configured differently.

## Behavior

**Concurrent search.** Searches draw from a fixed pool sized by `searchThreads`
at create/open time, and one finding no free slot fails rather than queuing.
The default is one slot per core (~33 KB each, independent of index size); read
the effective value from `index.searchThreads`. Adding is single-threaded.

**Exact search** (`exact = true` on either search proc) scans the whole index
in linear time, returning the true top `wanted` — for validation or small
sets. Defaults are `M=32`, `efConstruction=400`, `efSearch=80`.

## SIMD

**No compiler flags are needed for AVX2.** The kernel carries its own
`__attribute__((target("avx2")))` and is selected once per process via
`__builtin_cpu_supports`, so one binary runs AVX2 where available and a scalar
fallback elsewhere. Both compute the same integer dot product, on every path —
approximate, exact, and `cosine`. `metricImplementation()` reports which is
live.

Do **not** add `-march=native` or `-mavx2`. They gain nothing and break the
fallback: on an AVX-512 host `-march=native` auto-vectorizes the *scalar* kernel
into AVX-512, so dispatch still picks "scalar" on an older processor and then
faults with SIGILL on exactly the machines that path exists to serve.

Build with `-d:UsearchScalarQ4` to force the scalar kernel for validation.

## Build

Compiles the C++ adapter and links the C++ standard library (`-lc++` on macOS,
`-lstdc++` elsewhere). No installed USearch is required, and the module stays
importable from a C-backend Nim project.

```bash
nimble test     # test suite
nimble bench    # synthetic sizing and throughput benchmark
```

Both write under `build/`. No compiler is pinned: `config.nims` uses whatever
Nim is configured for, falling back to Clang only where there is no `g++`.

A 10,000-sample run built in 34.3 s and queried all 40-neighbor lists in 15.5 s,
producing an 18,122,748-byte index — about 1,812 bytes per sample. Random
vectors, so these are smoke-test figures.

## Persistence

USearch serializes a metric identifier rather than a function pointer, so
`openQ4Index` restores the packed-Q4 metric after each load or view, having
first checked record size, scalar type, and sequential IDs. Save to a temporary
file and rename atomically. Recording how the vectors were produced, alongside
the HNSW parameters, is the caller's job.

## License

The wrapper — everything outside `src/vendor/` — is MIT; see [`LICENSE`](LICENSE).

`src/vendor/usearch/` holds three Apache-2.0 headers from USearch v2.26.2,
which stay under that license. Apache-2.0 permits this inside a larger MIT
work, and the conditions are met: the full license ships as
[`src/vendor/USearch-LICENSE`](src/vendor/USearch-LICENSE) (§4a); the headers
are unmodified, so no change notices are required (§4b) and their notices are
intact (§4c) — per-file SHA-256 digests in
[`src/vendor/README.md`](src/vendor/README.md) make that checkable; and USearch
ships no `NOTICE` file at this version, so §4d does not apply. When bumping the
pinned version, refresh those digests and recheck for a `NOTICE`.
