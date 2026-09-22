# Vendored USearch source

The three headers in `usearch/` and `USearch-LICENSE` come from the official
USearch `v2.26.2` source archive:

<https://github.com/unum-cloud/USearch/archive/refs/tags/v2.26.2.tar.gz>

The downloaded archive had SHA-256:

```text
11a7eb49b34be0ce2c7a60af3a4f0a435b6c8f70dbcbd35495bbd3913bc8f665
```

Vendored-file SHA-256 values:

```text
42141603ddd0a48d286849aa2d763acefa9c315f9ae983beebe0f8df73a8c3ee  usearch/index.hpp
d57d57eb192656d85e8e2b5857ce4710414e6affa77968c1822edf604a2f6904  usearch/index_dense.hpp
a480e64ca5216b46127b34f26c5d9c985f5651a05c2ec7bba680e2536963c456  usearch/index_plugins.hpp
c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4  USearch-LICENSE
```

Only the header-only dense-index implementation is vendored. Optional NumKong,
language bindings, benchmarks, and packaging files are excluded.
