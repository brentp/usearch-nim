import std/[monotimes, os, strformat, strutils, times]

import ../src/usearch_q4

proc sketch(sample: int): Q4Record =
  var values = newSeq[int8](Q4Dimensions)
  var state = uint64(sample + 1) * 0x9e3779b97f4a7c15'u64
  for i in 0 ..< values.len:
    state = state xor (state shr 12)
    state = state xor (state shl 25)
    state = state xor (state shr 27)
    values[i] = int8(int(state mod 15) - 7)
  result = packQ4(values)

let count = if paramCount() == 0: 10_000 else: parseInt(paramStr(1))
var index = createQ4Index(count)
let buildStart = getMonoTime()
for sample in 0 ..< count:
  discard index.add(sketch(sample))
let buildSeconds = (getMonoTime() - buildStart).inNanoseconds.float / 1e9

let queryStart = getMonoTime()
var returned = 0
for sample in 0 ..< count:
  returned += index.searchById(uint32(sample), min(40, count - 1)).len
let querySeconds = (getMonoTime() - queryStart).inNanoseconds.float / 1e9
let memory = index.memoryStats

echo &"samples\t{count}"
echo &"metric_implementation\t{metricImplementation()}"
echo &"build_seconds\t{buildSeconds:.3f}"
echo &"query_seconds\t{querySeconds:.3f}"
echo &"neighbors_returned\t{returned}"
echo &"memory_total_bytes\t{memory.totalBytes}"
echo &"memory_graph_allocated_bytes\t{memory.graphAllocatedBytes}"
echo &"memory_graph_reserved_bytes\t{memory.graphReservedBytes}"
echo &"memory_vectors_allocated_bytes\t{memory.vectorsAllocatedBytes}"
echo &"memory_vectors_reserved_bytes\t{memory.vectorsReservedBytes}"
echo &"serialized_bytes\t{index.serializedBytes}"
