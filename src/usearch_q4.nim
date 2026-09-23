import std/[cpuinfo, os]

const Q4Dimensions* {.intdefine: "usearchQ4Dimensions".} = 4096
  ## Coordinates per record, set at build time with
  ## `-d:usearchQ4Dimensions=N`. Must be positive and even, since two
  ## coordinates share each packed byte.
  ##
  ## The value is fixed per build, not per index: records are fixed-size values
  ## and the C++ metric is compiled around this count. An index written by one
  ## build fails to open under a build with a different setting, because
  ## `openQ4Index` checks the stored record size.

when Q4Dimensions <= 0 or Q4Dimensions mod 2 != 0:
  {.error: "usearchQ4Dimensions must be a positive even number".}

const sourceDir = currentSourcePath.parentDir
{.compile: sourceDir / "usearch_q4_native.cpp".}
{.passC: "-I\"" & sourceDir / "vendor" & "\"".}
# One value drives both sides. If Nim and the adapter disagreed on the record
# size the mismatch would be silent memory corruption, so the C++ dimension is
# always derived from the Nim constant rather than set independently.
{.passC: "-DUSEARCH_Q4_DIMENSIONS=" & $Q4Dimensions.}
# Apple dropped libstdc++ from the SDK; clang there links the C++ runtime as
# -lc++. Everywhere else the adapter needs libstdc++ explicitly, because the
# module is meant to be importable from a plain C-backend Nim project.
when defined(macosx):
  {.passL: "-lc++".}
else:
  {.passL: "-lstdc++".}
when defined(UsearchScalarQ4):
  {.passC: "-DUSEARCH_Q4_DISABLE_AVX2=1".}

const
  Q4PackedBytes* = Q4Dimensions div 2
  Q4RecordBytes* = Q4PackedBytes + sizeof(uint32)
  DefaultConnectivity* = 32
  DefaultExpansionAdd* = 400
  DefaultExpansionSearch* = 80
  ErrorBufferBytes = 512

type
  UsearchError* = object of CatchableError

  Q4Record* = object
    bytes: array[Q4RecordBytes, uint8]

  Neighbor* = object
    id*: uint32
    distance*: float32

  MemoryStats* {.bycopy.} = object
    totalBytes*: csize_t
    graphAllocatedBytes*: csize_t
    graphReservedBytes*: csize_t
    vectorsAllocatedBytes*: csize_t
    vectorsReservedBytes*: csize_t

  Q4Index* = object
    handle: pointer

proc cVersion(): cstring {.importc: "usearch_q4_version", cdecl.}
proc cMetricImplementation(): cstring
  {.importc: "usearch_q4_metric_implementation", cdecl.}
proc cCreate(capacity, connectivity, expansionAdd, expansionSearch,
             searchThreads: csize_t; error: ptr char; errorCapacity: csize_t): pointer
  {.importc: "usearch_q4_create", cdecl.}
proc cOpen(path: cstring; memoryMap: cint; expansionAdd, expansionSearch,
           searchThreads: csize_t; error: ptr char; errorCapacity: csize_t): pointer
  {.importc: "usearch_q4_open", cdecl.}
proc cFree(handle: pointer) {.importc: "usearch_q4_free", cdecl.}
proc cReserve(handle: pointer; capacity: csize_t; error: ptr char;
              errorCapacity: csize_t): cint
  {.importc: "usearch_q4_reserve", cdecl.}
proc cAdd(handle: pointer; record: ptr uint8; assignedId: ptr uint32;
          error: ptr char; errorCapacity: csize_t): cint
  {.importc: "usearch_q4_add", cdecl.}
proc cSearchRecord(handle: pointer; record: ptr uint8; wanted: csize_t;
                   exact: cint; ids: ptr uint32; distances: ptr float32;
                   found: ptr csize_t; error: ptr char;
                   errorCapacity: csize_t): cint
  {.importc: "usearch_q4_search_record", cdecl.}
proc cSearchById(handle: pointer; id: uint32; wanted: csize_t; exact: cint;
                 ids: ptr uint32; distances: ptr float32; found: ptr csize_t;
                 error: ptr char; errorCapacity: csize_t): cint
  {.importc: "usearch_q4_search_by_id", cdecl.}
proc cCosineById(handle: pointer; first, second: uint32; cosine: ptr float32;
                 error: ptr char; errorCapacity: csize_t): cint
  {.importc: "usearch_q4_cosine_by_id", cdecl.}
proc cQ4Cosine(first, second: ptr uint8): float32
  {.importc: "usearch_q4_cosine", cdecl.}
proc cQ4Validate(record: ptr uint8; error: ptr char; errorCapacity: csize_t): cint
  {.importc: "usearch_q4_validate", cdecl.}
proc cSave(handle: pointer; path: cstring; error: ptr char;
           errorCapacity: csize_t): cint
  {.importc: "usearch_q4_save", cdecl.}
proc cSize(handle: pointer): csize_t {.importc: "usearch_q4_size", cdecl.}
proc cCapacity(handle: pointer): csize_t {.importc: "usearch_q4_capacity", cdecl.}
proc cConnectivity(handle: pointer): csize_t
  {.importc: "usearch_q4_connectivity", cdecl.}
proc cExpansionAdd(handle: pointer): csize_t
  {.importc: "usearch_q4_expansion_add", cdecl.}
proc cExpansionSearch(handle: pointer): csize_t
  {.importc: "usearch_q4_expansion_search", cdecl.}
proc cSerializedBytes(handle: pointer): csize_t
  {.importc: "usearch_q4_serialized_bytes", cdecl.}
proc cMemory(handle: pointer): MemoryStats
  {.importc: "usearch_q4_memory", cdecl.}

proc cSearchThreads(handle: pointer): csize_t
  {.importc: "usearch_q4_search_threads", cdecl.}

proc cDimensions(): csize_t {.importc: "usearch_q4_dimensions", cdecl.}
proc cRecordBytes(): csize_t {.importc: "usearch_q4_record_bytes", cdecl.}

# A caller who forces -DUSEARCH_Q4_DIMENSIONS through their own --passC would
# override what this module sets, and the two sides would lay records out
# differently. That is silent memory corruption, so check once at startup
# rather than trusting the build.
if cDimensions().int != Q4Dimensions or cRecordBytes().int != Q4RecordBytes:
  raise newException(UsearchError,
    "usearch_q4 build mismatch: Nim was built for " & $Q4Dimensions &
    " dimensions (" & $Q4RecordBytes & "-byte records) but the C++ adapter " &
    "reports " & $cDimensions() & " (" & $cRecordBytes() & "-byte records); " &
    "set the dimension with -d:usearchQ4Dimensions=N only")

proc nativeDimensions*(): int = cDimensions().int
  ## Dimension count the C++ adapter was compiled with. Equals `Q4Dimensions`
  ## in a correctly built module; the check above enforces that at startup.

proc nativeRecordBytes*(): int = cRecordBytes().int
  ## Record size the C++ adapter was compiled with. Equals `Q4RecordBytes`.

proc defaultSearchThreads*(): int =
  ## Search-slot count used when `searchThreads` is not given.
  ##
  ## USearch hands each in-flight search a slot from a fixed pool sized at
  ## create/open time; a search that finds no free slot fails outright rather
  ## than queuing. One slot per detected core costs about 33 KB each regardless
  ## of index size, so sizing for the machine is far cheaper than the failures
  ## a too-small pool causes.
  max(1, countProcessors())

proc `=copy`(dest: var Q4Index; source: Q4Index) {.error:
  "Q4Index owns native memory and cannot be copied".}

proc `=destroy`(index: var Q4Index) =
  if index.handle != nil:
    cFree(index.handle)
    index.handle = nil

proc close*(index: var Q4Index) =
  `=destroy`(index)

proc errorText(buffer: var array[ErrorBufferBytes, char]): string =
  $cast[cstring](addr buffer[0])

template checked(call: untyped; buffer: var array[ErrorBufferBytes, char]) =
  if (call) == 0:
    raise newException(UsearchError, errorText(buffer))

proc requireOpen(index: Q4Index) =
  if index.handle == nil:
    raise newException(UsearchError, "USearch index is closed")

template raw(record: Q4Record): ptr uint8 =
  cast[ptr uint8](unsafeAddr record.bytes[0])

proc putU32Le(bytes: var array[Q4RecordBytes, uint8]; offset: int; value: uint32) =
  bytes[offset] = uint8(value and 0xff)
  bytes[offset + 1] = uint8((value shr 8) and 0xff)
  bytes[offset + 2] = uint8((value shr 16) and 0xff)
  bytes[offset + 3] = uint8((value shr 24) and 0xff)

proc getU32Le(bytes: array[Q4RecordBytes, uint8]; offset: int): uint32 =
  uint32(bytes[offset]) or
    (uint32(bytes[offset + 1]) shl 8) or
    (uint32(bytes[offset + 2]) shl 16) or
    (uint32(bytes[offset + 3]) shl 24)

proc packQ4*(values: openArray[int8]): Q4Record =
  if values.len != Q4Dimensions:
    raise newException(ValueError,
      "packed Q4 sketch requires exactly " & $Q4Dimensions & " coordinates")
  var norm2 = 0'u32
  for i in countup(0, Q4Dimensions - 1, 2):
    let low = int(values[i])
    let high = int(values[i + 1])
    if low < -7 or low > 7 or high < -7 or high > 7:
      raise newException(ValueError, "Q4 coordinates must be between -7 and 7")
    let lowCode = cast[uint8](values[i]) and 0x0f'u8
    let highCode = cast[uint8](values[i + 1]) and 0x0f'u8
    result.bytes[i div 2] = lowCode or (highCode shl 4)
    norm2 += uint32(low * low + high * high)
  if norm2 == 0:
    raise newException(ValueError, "Q4 sketch must have nonzero norm")
  result.bytes.putU32Le(Q4PackedBytes, norm2)

proc unpackQ4*(record: Q4Record): seq[int8] =
  result = newSeq[int8](Q4Dimensions)
  for i in 0 ..< Q4PackedBytes:
    let packed = record.bytes[i]
    let low = packed and 0x0f'u8
    let high = packed shr 4
    result[2 * i] = if low < 8: int8(low) else: int8(int(low) - 16)
    result[2 * i + 1] = if high < 8: int8(high) else: int8(int(high) - 16)

proc squaredNorm*(record: Q4Record): uint32 =
  record.bytes.getU32Le(Q4PackedBytes)

proc toBytes*(record: Q4Record): array[Q4RecordBytes, uint8] =
  record.bytes

proc fromBytes*(bytes: openArray[uint8]): Q4Record =
  if bytes.len != Q4RecordBytes:
    raise newException(ValueError,
      "packed Q4 record requires exactly " & $Q4RecordBytes & " bytes")
  copyMem(addr result.bytes[0], unsafeAddr bytes[0], Q4RecordBytes)
  var error: array[ErrorBufferBytes, char]
  checked cQ4Validate(result.raw, addr error[0], ErrorBufferBytes.csize_t), error

proc cosine*(first, second: Q4Record): float32 =
  cQ4Cosine(first.raw, second.raw)

proc similarity*(neighbor: Neighbor): float32 {.inline.} =
  1.0'f32 - neighbor.distance

proc version*(): string = $cVersion()
proc metricImplementation*(): string = $cMetricImplementation()

proc createQ4Index*(capacity: int; connectivity = DefaultConnectivity;
                    expansionAdd = DefaultExpansionAdd;
                    expansionSearch = DefaultExpansionSearch;
                    searchThreads = defaultSearchThreads()): Q4Index =
  if capacity < 0 or connectivity <= 0 or expansionAdd <= 0 or
      expansionSearch <= 0 or searchThreads <= 0:
    raise newException(ValueError,
      "capacity must be nonnegative and HNSW parameters must be positive")
  var error: array[ErrorBufferBytes, char]
  result.handle = cCreate(capacity.csize_t, connectivity.csize_t,
    expansionAdd.csize_t, expansionSearch.csize_t, searchThreads.csize_t,
    addr error[0], ErrorBufferBytes.csize_t)
  if result.handle == nil:
    raise newException(UsearchError, errorText(error))

proc openQ4Index*(path: string; memoryMap = false;
                  expansionAdd = DefaultExpansionAdd;
                  expansionSearch = DefaultExpansionSearch;
                  searchThreads = defaultSearchThreads()): Q4Index =
  if expansionAdd <= 0 or expansionSearch <= 0 or searchThreads <= 0:
    raise newException(ValueError, "HNSW parameters must be positive")
  var error: array[ErrorBufferBytes, char]
  result.handle = cOpen(path.cstring, cint(memoryMap), expansionAdd.csize_t,
    expansionSearch.csize_t, searchThreads.csize_t,
    addr error[0], ErrorBufferBytes.csize_t)
  if result.handle == nil:
    raise newException(UsearchError, errorText(error))

proc reserve*(index: var Q4Index; capacity: int) =
  index.requireOpen
  if capacity < 0:
    raise newException(ValueError, "capacity cannot be negative")
  var error: array[ErrorBufferBytes, char]
  checked cReserve(index.handle, capacity.csize_t, addr error[0],
                   ErrorBufferBytes.csize_t), error

proc add*(index: var Q4Index; record: Q4Record): uint32 =
  index.requireOpen
  var error: array[ErrorBufferBytes, char]
  checked cAdd(index.handle, record.raw, addr result, addr error[0],
               ErrorBufferBytes.csize_t), error

proc collectNeighbors(ids: seq[uint32]; distances: seq[float32]; found: int): seq[Neighbor] =
  result = newSeq[Neighbor](found)
  for i in 0 ..< found:
    result[i] = Neighbor(id: ids[i], distance: distances[i])

proc checkWanted(index: Q4Index; wanted: int) =
  index.requireOpen
  if wanted < 0:
    raise newException(ValueError, "wanted neighbor count cannot be negative")

proc search*(index: Q4Index; record: Q4Record; wanted: int;
             exact = false): seq[Neighbor] =
  ## Nearest neighbors of `record`, which need not be in the index. Nothing is
  ## excluded, so every hit is a stored record.
  ##
  ## `exact = false` walks the HNSW graph and returns an approximation.
  ## `exact = true` scores every record instead, returning the true top
  ## `wanted` in time linear in index size — for validation or small indexes.
  index.checkWanted(wanted)
  if wanted == 0:
    return @[]
  var ids = newSeq[uint32](wanted)
  var distances = newSeq[float32](wanted)
  var found: csize_t
  var error: array[ErrorBufferBytes, char]
  checked cSearchRecord(index.handle, record.raw, wanted.csize_t, cint(exact),
    addr ids[0], addr distances[0], addr found, addr error[0],
    ErrorBufferBytes.csize_t), error
  collectNeighbors(ids, distances, found.int)

proc searchById*(index: Q4Index; id: uint32; wanted: int;
                 exact = false): seq[Neighbor] =
  ## Nearest neighbors of the stored record `id`, which is excluded from its
  ## own results. `exact` behaves as in `search`.
  index.checkWanted(wanted)
  if wanted == 0:
    return @[]
  var ids = newSeq[uint32](wanted)
  var distances = newSeq[float32](wanted)
  var found: csize_t
  var error: array[ErrorBufferBytes, char]
  checked cSearchById(index.handle, id, wanted.csize_t, cint(exact),
    addr ids[0], addr distances[0], addr found, addr error[0],
    ErrorBufferBytes.csize_t), error
  collectNeighbors(ids, distances, found.int)

proc cosineById*(index: Q4Index; first, second: uint32): float32 =
  index.requireOpen
  var error: array[ErrorBufferBytes, char]
  checked cCosineById(index.handle, first, second, addr result,
    addr error[0], ErrorBufferBytes.csize_t), error

proc save*(index: Q4Index; path: string) =
  index.requireOpen
  var error: array[ErrorBufferBytes, char]
  checked cSave(index.handle, path.cstring, addr error[0],
                ErrorBufferBytes.csize_t), error

proc len*(index: Q4Index): int =
  index.requireOpen
  cSize(index.handle).int

proc capacity*(index: Q4Index): int =
  index.requireOpen
  cCapacity(index.handle).int

proc connectivity*(index: Q4Index): int =
  index.requireOpen
  cConnectivity(index.handle).int

proc expansionAdd*(index: Q4Index): int =
  index.requireOpen
  cExpansionAdd(index.handle).int

proc expansionSearch*(index: Q4Index): int =
  index.requireOpen
  cExpansionSearch(index.handle).int

proc searchThreads*(index: Q4Index): int =
  ## Number of searches this index can serve at once. Concurrent searches
  ## beyond this count fail rather than queue, so keep it at or above the
  ## number of threads that call `search` family procs simultaneously.
  index.requireOpen
  cSearchThreads(index.handle).int

proc serializedBytes*(index: Q4Index): int64 =
  index.requireOpen
  cSerializedBytes(index.handle).int64

proc memoryStats*(index: Q4Index): MemoryStats =
  index.requireOpen
  cMemory(index.handle)
