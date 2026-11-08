# Upstream VQF dependency

- Project: [saltsystemslab/vqf](https://github.com/saltsystemslab/vqf)
- Revision: `62accaf20c0f6014f98fc8d9b995e244b6318ce3`
- Paper: Pandey et al., “Vector Quotient Filters,” SIGMOD 2021,
  https://doi.org/10.1145/3448016.3452841
- License: BSD 3-Clause; see [LICENSE](./LICENSE).

Only the library sources needed for the AVX2 implementation are included.
The benchmark and demo programs from upstream are not part of this dependency.
Unconditional construction and full-filter diagnostic prints in
`src/vqf_filter.c` were removed; callers receive status through the API and
the benchmark reports capacity failures in its result table.
