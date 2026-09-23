version = "0.1.1"
author = "Brent Pedersen"
description = "Minimal packed-Q4 USearch wrapper with an exact integer cosine metric"
license = "MIT"
srcDir = "src"

# Compiler selection lives in config.nims so that it applies to plain `nim c`
# runs inside the repository as well as to these tasks.

task test, "compile and run wrapper tests":
  mkDir "build"
  exec "nim c -r --nimcache:build/nimcache --out:build/test_usearch_q4" &
    " tests/test_usearch_q4.nim"

task bench, "run the synthetic sizing and throughput benchmark":
  mkDir "build"
  exec "nim c -d:release --nimcache:build/nimcache-release --out:build/bench_q4" &
    " -r bench/bench_q4.nim"
