version = "0.1.0"
author = "Brent Pedersen"
description = "Minimal packed-Q4 USearch wrapper with an exact integer cosine metric"
license = "MIT"
srcDir = "src"

task test, "compile and run wrapper tests":
  exec "nim c --cc:clang -r --nimcache:/tmp/usearch-q4-nimcache --out:/tmp/test_usearch_q4 tests/test_usearch_q4.nim"
