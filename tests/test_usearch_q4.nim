import std/[math, os, sequtils, sha1, unittest]

import ../src/usearch_q4

proc pattern(seed: int): seq[int8] =
  result = newSeq[int8](Q4Dimensions)
  for i in 0 ..< result.len:
    result[i] = int8(((i * 17 + seed * 31) mod 15) - 7)

suite "packed-Q4 USearch wrapper":
  test "Q4 packing preserves values and norm":
    var values = pattern(3)
    values[0] = -7
    values[1] = 7
    values[2] = 0
    let record = packQ4(values)
    check record.unpackQ4 == values
    var expected = 0'u32
    for value in values:
      expected += uint32(int(value) * int(value))
    check record.squaredNorm == expected
    check abs(record.cosine(record) - 1.0'f32) < 1e-6
    check fromBytes(record.toBytes).unpackQ4 == values
    var corrupt = record.toBytes
    corrupt[^1] = corrupt[^1] xor 1
    expect UsearchError:
      discard fromBytes(corrupt)

  test "packed cosine matches scalar cosine":
    for seed in 1 .. 16:
      let firstValues = pattern(seed)
      let secondValues = pattern(seed * 7 + 3)
      let first = packQ4(firstValues)
      let second = packQ4(secondValues)
      var dot = 0.0
      var normFirst = 0.0
      var normSecond = 0.0
      for i in 0 ..< Q4Dimensions:
        dot += float(firstValues[i]) * float(secondValues[i])
        normFirst += float(firstValues[i]) * float(firstValues[i])
        normSecond += float(secondValues[i]) * float(secondValues[i])
      let expected = dot / sqrt(normFirst * normSecond)
      check abs(float(first.cosine(second)) - expected) < 1e-6

  test "zero and out-of-range sketches are rejected":
    expect ValueError:
      discard packQ4(newSeq[int8](Q4Dimensions))
    var invalid = pattern(2)
    invalid[100] = 8
    expect ValueError:
      discard packQ4(invalid)

  test "serialized index does not depend on searchThreads":
    # Each USearch thread context owns a private RNG that picks a node's HNSW
    # level, so an insertion drawing a pool slot would tie index bytes to the
    # host's core count. Adds are pinned to context 0 to prevent that.
    let base = getTempDir() / "usearch-q4-threads-"
    var paths: seq[string]
    defer:
      for path in paths:
        if fileExists(path): removeFile(path)
    for searchThreads in [1, 2, 8, 64]:
      let path = base & $searchThreads & ".usearch"
      paths.add(path)
      var index = createQ4Index(64, searchThreads = searchThreads)
      check index.searchThreads == searchThreads
      for seed in 1 .. 64:
        discard index.add(packQ4(pattern(seed)))
      index.save(path)
      index.close()
    let expected = $secureHash(readFile(paths[0]))
    for path in paths:
      check $secureHash(readFile(path)) == expected

  test "default index serves concurrent searches":
    var index = createQ4Index(32)
    check index.searchThreads == defaultSearchThreads()
    check index.searchThreads >= 1
    for seed in 1 .. 32:
      discard index.add(packQ4(pattern(seed)))
    # A pool smaller than the caller's thread count fails rather than queuing,
    # and must say so in terms of the knob that fixes it.
    var pinned = createQ4Index(4, searchThreads = 1)
    check pinned.searchThreads == 1
    for seed in 1 .. 4:
      discard pinned.add(packQ4(pattern(seed)))
    check pinned.searchById(0, 2).len == 2

  test "index searches stored records and survives load and view":
    var index = createQ4Index(24)
    check version() == "2.26.2"
    when defined(UsearchScalarQ4):
      check metricImplementation() == "scalar"
    else:
      check metricImplementation() in ["scalar", "avx2"]
    check index.capacity == 24
    check index.connectivity == DefaultConnectivity
    check index.expansionAdd == DefaultExpansionAdd
    check index.expansionSearch == DefaultExpansionSearch

    var records: seq[Q4Record]
    for pair in 0 ..< 10:
      let record = packQ4(pattern(pair + 1))
      records.add(record)
      records.add(record)
    for id, record in records:
      check index.add(record) == uint32(id)
    check index.len == records.len
    index.reserve(28)
    check index.capacity == 28
    check index.memoryStats.totalBytes > 0
    check index.serializedBytes > int64(records.len * Q4RecordBytes)

    for id in 0 ..< records.len:
      let partner = uint32(id xor 1)
      let neighbors = index.searchById(uint32(id), 3)
      check neighbors.len == 3
      check neighbors[0].id == partner
      check abs(neighbors[0].similarity - 1.0'f32) < 1e-6
      check abs(index.cosineById(uint32(id), partner) - 1.0'f32) < 1e-6

    let external = index.search(records[4], 2)
    check external.len == 2
    check external[0].id in [4'u32, 5'u32]
    check abs(external[0].similarity - 1.0'f32) < 1e-6

    let exactExternal = index.exactSearch(records[4], records.len + 5)
    check exactExternal.len == records.len
    var externalSeen = newSeq[bool](records.len)
    for i, neighbor in exactExternal:
      check neighbor.id < uint32(records.len)
      check not externalSeen[neighbor.id.int]
      externalSeen[neighbor.id.int] = true
      check abs(neighbor.similarity - records[4].cosine(records[neighbor.id.int])) < 1e-6
      if i > 0:
        check exactExternal[i - 1].distance <= neighbor.distance
    check externalSeen.allIt(it)

    let exactById = index.exactSearchById(4, records.len + 5)
    check exactById.len == records.len - 1
    var byIdSeen = newSeq[bool](records.len)
    byIdSeen[4] = true
    for i, neighbor in exactById:
      check neighbor.id != 4
      check not byIdSeen[neighbor.id.int]
      byIdSeen[neighbor.id.int] = true
      check abs(neighbor.similarity - index.cosineById(4, neighbor.id)) < 1e-6
      if i > 0:
        check exactById[i - 1].distance <= neighbor.distance
    check byIdSeen.allIt(it)

    let path = getTempDir() / "usearch-q4-test.usearch"
    let repeatedPath = getTempDir() / "usearch-q4-repeat.usearch"
    defer:
      if fileExists(path): removeFile(path)
      if fileExists(repeatedPath): removeFile(repeatedPath)
    index.save(path)

    block:
      var repeated = createQ4Index(24)
      for record in records:
        discard repeated.add(record)
      repeated.reserve(28)
      repeated.save(repeatedPath)
      check $secureHash(readFile(path)) == $secureHash(readFile(repeatedPath))

    block:
      var loaded = openQ4Index(path)
      check loaded.len == records.len
      let neighbors = loaded.searchById(4, 3)
      check neighbors[0].id == 5
      check abs(neighbors[0].similarity - 1.0'f32) < 1e-6
      for neighbor in neighbors:
        let exact = loaded.cosineById(4, neighbor.id)
        check abs(neighbor.similarity - exact) < 1e-6
      check abs(loaded.cosineById(0, 2) - records[0].cosine(records[2])) < 1e-6
      let exact = loaded.exactSearchById(4, 3)
      check exact.len == 3
      check exact[0].id == 5
      for neighbor in exact:
        check abs(neighbor.similarity - loaded.cosineById(4, neighbor.id)) < 1e-6

    block:
      var viewed = openQ4Index(path, memoryMap = true)
      check viewed.len == records.len
      let neighbors = viewed.searchById(8, 3)
      check neighbors[0].id == 9
      check abs(neighbors[0].similarity - 1.0'f32) < 1e-6
      for neighbor in neighbors:
        let exact = viewed.cosineById(8, neighbor.id)
        check abs(neighbor.similarity - exact) < 1e-6
      let exact = viewed.exactSearch(records[8], 2)
      check exact.len == 2
      check exact[0].id in [8'u32, 9'u32]
      expect UsearchError:
        discard viewed.add(records[0])
