# VQF Optimized

Portable C++17 project. `VQFEngine` is retained as an exact hash-set baseline
and `FingerprintFilter` as an experimental fingerprint-table prototype with
scalar-default and optional AVX2 probe paths. The project now
also includes the upstream SIGMOD 2021 Vector Quotient Filter implementation
as an optional AVX2 backend (`NativeVQF`). The upstream backend is distinct
from both local prototypes.

## Design specification (draft)

The first implementation target is a fixed-capacity, 64-bit-key approximate
membership filter. The API is intended to provide:

- `insert(key)`: add a key, report whether insertion succeeded, and report
  failure when the filter cannot accept the item at its configured limit.
- `query(key)`: report whether a key may be present. False positives are
  possible; false negatives for keys still represented in the filter are not
  acceptable.
- `erase(key)`: remove a previously inserted key, subject to the selected
  filter's deletion contract.

The collection has set semantics: inserting the same key more than once does
not increase its logical item count. The load factor is
`distinct live keys / physical filter slots`; deleted entries do not count as
live. The design target is normal operation through 90% load and a stress
target of 95%. These are targets to validate with repeated insert/erase cycles,
not guaranteed limits until the implementation passes those tests.

The initial false-positive target is at most 1% at both 90% and 95% load,
measured using a fixed, documented workload and at least one million absent
queries. Report the measured rate and memory per live key; do not infer either
from the target.

### Prototype deletion modes

A compact approximate filter stores fingerprints rather than the original
64-bit keys. Two different keys can have the same fingerprint. Consequently,
so deletion semantics are selected when constructing a filter instance and
cannot be mixed on that instance:

- **Fast mode (`erase_fast`)** stores fingerprints only. Its contract requires
  callers to erase only keys that were successfully inserted and have not
  already been erased. Erasing an absent key is unsupported because a
  fingerprint collision could remove another key's membership. When multiple
  live keys share a fingerprint, erasing one must preserve membership for the
  others.
- **Exact mode (`erase_exact`)** keeps enough exact-key metadata to ensure
  erasing an absent key has no effect and to distinguish colliding keys.
  Include this metadata in memory and performance measurements.

Each instance has one immutable deletion mode selected at construction. Calling
the operation for the other mode must be rejected explicitly; do not silently
fall back to the other semantics. Both modes must preserve the no-false-negative
guarantee for keys that remain live. A deleted key may still query positive if
another live key has the same fingerprint.

The current `VQFEngine` hash-set baseline does not yet implement either mode or
deletion. Its existing API and benchmark are a functional scaffold, not the
selected filter design. `FingerprintFilter` implements the two deletion modes
for experimentation, but its open-addressed fingerprint table is a prototype,
not the final VQF data structure. Fast mode counts insertions as occurrences;
inserting the same key twice requires two matching erases. Exact mode uses set
semantics and stores exact keys in auxiliary metadata.

### Upstream Vector Quotient Filter

`NativeVQF` wraps the BSD-3-Clause implementation from
[saltsystemslab/vqf](https://github.com/saltsystemslab/vqf), pinned to revision
`62accaf20c0f6014f98fc8d9b995e244b6318ce3`. The retained license and upstream
notes are in [third_party/vqf](./third_party/vqf).

This upstream revision uses an 8-bit tag configuration and x86-64 AVX2, BMI,
BMI2, and POPCNT instructions. `NativeVQF::is_supported()` checks these
features at runtime, including OS-enabled AVX state. The constructor performs
that check before calling upstream code and throws `std::runtime_error` on an
unsupported CPU, rather than allowing an illegal-instruction crash. There is
no scalar fallback for this upstream revision. `erase` should only be used
for keys that were inserted and not already removed; this is a probabilistic
filter, not an exact-key set.

The wrapper tracks item count because the upstream metadata does not update
that field. The benchmark reports physical capacity, FPR, insert/query/churn
throughput, and bytes used by the filter. It also reports if a reinsertion
fails at high load; such failure is a measured capacity limitation, not a
successful churn result.

`TieredNativeVQF` is an optional wrapper for high-load insertion and churn. It
starts with one NativeVQF and creates additional filters when a tier reaches
its configured ceiling (default 88%, configurable from 50% to 90%). Queries
check each tier. An exact key-to-tier index ensures erase targets the layer
that accepted a key instead of risking removal after a false positive in
another layer. Tiered inserts have set semantics (duplicate keys do not
increase size), unlike the occurrence-counting `NativeVQF` wrapper. The exact
index adds memory overhead; `memory_bytes()` includes key payload but not
unordered-map allocator/bucket overhead. Total capacity is the sum of all
allocated tiers, so overall utilization is not comparable to a single-tier
filter's load.

### Separate key-width and fingerprint-width comparisons

`WideFingerprintFilter` and `vqf_bench_wide_fingerprint` are a separate
prototype for comparing input keys of 16, 31, 64, 128, and 256 bits with packed
fingerprints of 16, 32, 64, and 128 bits. The wider key is hashed to a
128-bit value before truncation to the selected fingerprint width. On x86
builds whose compiler supports AVX2, probing uses a dedicated AVX2 comparison
kernel when `FingerprintScanMode::automatic` is selected and the runtime CPU
and OS support it; other machines use the scalar path. The default is scalar:
initial benchmarks did not show a consistent AVX2 win, so dispatch remains
opt-in rather than risking a default regression. Fingerprint and slot-state
masks are processed by lane in the vectorized path. `FingerprintFilter` uses
the same runtime-dispatched probing approach for its 64-bit slots. Set
`-DVQF_ENABLE_AVX2_SCAN=OFF` to omit the AVX2 kernel entirely. This is not the
upstream VQF data structure.

The current fingerprint widths are byte-aligned (16/32/64/128 bits), so
PDEP/PEXT do not provide useful sparse-bit extraction here; ordinary aligned
loads and shifts are the more direct representation. No BMI2 requirement is
added to the portable prototype. Use the `compare` benchmark mode and repeat
measurements on the target CPU before opting into AVX2.

The benchmark reports false-positive rate before and after churn, operation
throughput, packed filter-array bytes, and exact-key payload bytes. Exact-mode
hash-table bucket/node allocator overhead is not included. The 16-bit input
domain cannot support the requested capacity and one million distinct absent
keys, so its capacity is capped and absent keys are sampled with replacement;
interpret its FPR as a smaller-domain measurement. The VQF benchmark remains
separate because this upstream revision uses a fixed 8-bit tag.

### Cuckoo and Blocked Bloom competitors

The project also provides two comparison implementations in
`competitor_filters.hpp`:

- `CuckooFilter` stores 16-bit fingerprints in four-entry buckets, supports
  insert/query/erase, and uses bounded random-walk relocation. Erase is
  fingerprint-based; only erase keys previously inserted and not already
  removed. Its allocated bucket capacity is rounded up to a power of two and
  can exceed the requested slot count.
- `BlockedBloomFilter` chooses one 512-bit block per key and sets multiple
  bits within that block. It is sized for an expected item count and target
  false-positive rate; this implementation is insert/query only and does not
  support deletion.

`vqf_bench_competitors` sends the same sequential keys and absent-query stream
to both filters at 90% and 95% of the nominal item target. It reports achieved
load relative to expected item capacity for Bloom and physical slots for
Cuckoo/VQF, false positives, throughput, and filter-array bytes. These load
percentages are not equivalent. The Bloom filter is sized to a 1% target
FPR at the nominal capacity; its FPR changes with actual fill. When
`VQF_ENABLE_UPSTREAM_VQF=ON`, the same benchmark also includes the upstream
VQF backend, after checking CPU support. Bloom has no deletion, so churn is
marked unsupported for that row.

This benchmark uses equal item counts, not equal memory budgets. Bloom and
Cuckoo load percentages describe different things and must not be interpreted
as equivalent design points. Use the reported memory and FPR when comparing
them; repeat with a memory-budget-matched setup before making efficiency
claims.

### Performance claims to validate

- The AVX2 probe compares multiple fingerprint lanes and combines match/state
  masks; scalar prefix handling and other hot-path branches remain. Do not
  describe the full insert/query/erase path as branchless without checking
  optimized machine code.
- Comparison set: **Blocked Bloom Filter**, **Cuckoo Filter**, and
  **Vector Quotient Filter (VQF)**. Local Bloom and Cuckoo implementations are
  comparison baselines; the upstream VQF remains optional.
- Compare using identical key/query traces and a common memory budget. Report
  false-positive rate, bytes per live key, insert/query throughput, and
  deletion throughput where supported. Record CPU, compiler, build flags,
  dataset seed, and whether AVX2 was used.
- A conventional Blocked Bloom Filter does not support deletion. Include it
  for insert/query/FPR/memory comparisons, and mark deletion as unsupported.
  If deletion performance must be compared across all structures, add a
  Counting Blocked Bloom variant as a separately labeled implementation;
  include its counter memory overhead.
- A 90-95% slot load is meaningful for slot-based structures such as Cuckoo
  Filters and the VQF implementation under test, but is not directly
  comparable to a Bloom Filter. For cross-structure comparisons, fix the
  memory budget and report each structure's achieved load or bits per key,
  rather than equating their load-factor definitions.
- Do not compare the local `FingerprintFilter` prototype as though it were a
  Blocked Bloom Filter or Cuckoo Filter. Do not publish cross-filter claims
  until the other implementations use the same benchmark protocol.
- The upstream VQF target is an AVX2-only backend, not a runtime-dispatched
  portable library. A scalar VQF implementation and safe runtime dispatch for
  old CPUs remain future work.

## Requirements

- CMake 3.18 or newer
- A C++17-capable compiler
- Optional: OpenMP and libnuma

The default build keeps the main library portable and may compile a separate
AVX2 scan kernel. That kernel is opt-in at runtime and CPU/OS-checked before
use; set `-DVQF_ENABLE_AVX2_SCAN=OFF` to omit it. In contrast,
`-DVQF_ENABLE_AVX2=ON` compiles the entire library for AVX2 without runtime
dispatch and must remain off for binaries that need to run on older CPUs.

## Configure and build

Run these commands from this directory:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Use VQF from another CMake project

Install the library to a prefix that your other project can access. On
Windows/MSYS2, for example:

```powershell
cmake -S . -B build `
  -DVQF_BUILD_TESTS=OFF `
  -DVQF_BUILD_BENCHMARKS=OFF `
  -DCMAKE_INSTALL_PREFIX="D:/libs/vqf"
cmake --build build --config Release
cmake --install build --config Release
```

In the consuming project's `CMakeLists.txt`, locate the package and link the
exported target. Linking the target also supplies the installed include
directory and C++17 requirement:

```cmake
find_package(vqf CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE vqf::vqf)
```

Example `main.cpp`:

```cpp
#include <vqf/vqf_core.hpp>

int main() {
    vqf::VQFEngine filter(1024);
    filter.insert(42);
    return filter.query(42) ? 0 : 1;
}
```

Configure the consumer with the install prefix:

```powershell
cmake -S path/to/my_app -B path/to/my_app/build `
  -DCMAKE_PREFIX_PATH="D:/libs/vqf"
cmake --build path/to/my_app/build --config Release
```

The package exports `vqf::vqf` in every build. If the library was built with
`-DVQF_ENABLE_UPSTREAM_VQF=ON`, it also exports `vqf::native` and
`vqf::upstream`. Require the optional native component and link `vqf::native`
to use `NativeVQF` or `TieredNativeVQF`:

```cmake
find_package(vqf CONFIG REQUIRED COMPONENTS native)
target_link_libraries(my_app PRIVATE vqf::native)
```

```cpp
#include <vqf/vqf_native.hpp>
#include <vqf/vqf_tiered_native.hpp>

vqf::NativeVQF::is_supported();  // Check before choosing the native backend.
vqf::TieredNativeVQF filter({100000, 88});
```

`NativeVQF` requires an x86-64 GCC/Clang build with AVX2, BMI, BMI2, and
POPCNT. Its constructor throws if the running CPU/OS lacks those features;
there is no scalar fallback in this upstream revision. The base `vqf::vqf`
target and its portable filters do not require these native-VQF extensions.
When installed, the native target includes the upstream static library and its
BSD-3-Clause license.

An in-tree consumer example is available in
[`tests/cmake_consumer`](./tests/cmake_consumer). For example, after
installing the native-enabled package:

```powershell
cmake -S tests/cmake_consumer -B build/consumer `
  -DCMAKE_PREFIX_PATH="D:/libs/vqf" `
  -DVQF_CONSUMER_WITH_NATIVE=ON
cmake --build build/consumer --config Release
```

Build the upstream AVX2 VQF backend and its benchmark:

```powershell
cmake -S . -B build `
  -DVQF_ENABLE_UPSTREAM_VQF=ON `
  -DVQF_BUILD_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\benchmarks\vqf_bench_native.exe
```

The upstream backend is optional and is built only with an x86-64 GCC/Clang
target supporting `-mavx2`, `-mbmi`, `-mbmi2`, and `-mpopcnt`. At runtime,
`NativeVQF` checks CPU/OS support before initializing the upstream filter and
throws a clear exception if unavailable. It does not provide a scalar
fallback.

The default value of `VQF_BUILD_BENCHMARKS` is `OFF`. To build the benchmark
executables, configure with `-DVQF_BUILD_BENCHMARKS=ON`. With the MinGW
Makefiles generator, CMake puts them under `build/benchmarks/`, not directly
under `build/`. For a multi-configuration generator such as Visual Studio,
they are normally under `build/benchmarks/Release/` when built as Release.

The baseline throughput benchmark has no third-party dependency. Results
should be measured in Release mode and are not directly comparable across
different CPUs or build settings.

### Parallel batch queries and NUMA affinity

`ParallelQueryExecutor` in `vqf_parallel.hpp` parallelizes read-only batch
queries over filters that expose a thread-safe, non-throwing
`const query(value_type)` method. It is tested with both `VQFEngine` and
`NativeVQF`. The filter must remain alive and unchanged for the duration of a
batch; this executor does not make concurrent insert/erase operations
thread-safe.

OpenMP is enabled when found. `thread_count = 0` uses the OpenMP runtime's
default team size; a positive value requests that number of workers. If built
without OpenMP, the default executes serially and explicitly requesting more
than one worker throws an error. Empty batches accept null pointers; non-empty
batches require valid input and output buffers.

On Linux builds with libnuma, `numa_node >= 0` temporarily pins each query
worker to CPUs on that NUMA node and restores its previous CPU affinity after
the batch. `numa_node = -1` leaves affinity unchanged. A build without libnuma
rejects an explicit NUMA-node request rather than silently ignoring it. This
feature controls worker affinity only: it does not migrate or allocate the
engine's vectors on a NUMA node, so initialize/touch the engine data using the
memory policy appropriate for the application.

Example:

```cpp
#include "vqf/vqf_core.hpp"
#include "vqf/vqf_parallel.hpp"

vqf::VQFEngine filter(1000000);
// Insert keys before starting parallel queries.
vqf::ParallelQueryExecutor queries({8, -1});
queries.batch_process(filter, keys.data(), keys.size(), results.data());
```

Configure with OpenMP (if installed) and optional Linux NUMA detection using
the normal CMake build. To deliberately disable either facility, set
`-DVQF_ENABLE_OPENMP=OFF` or `-DVQF_ENABLE_NUMA=OFF`.

The optional parallel throughput benchmark compares the serial and parallel
batch-query paths and verifies that their results match:

```powershell
.\build\benchmarks\vqf_bench_parallel.exe 5000000 0 -1
```

Its arguments are query count, worker count (`0` means OpenMP default), and
NUMA node (`-1` means do not pin). NUMA affinity is Linux-only and requires
libnuma at build time.

The filter-quality benchmark reports false-positive rate before and after
three insert/erase churn cycles, positive/negative query throughput, insert
throughput, and churn throughput at 90% and 95% load, for 16-, 24-, and 32-bit
fingerprints in both deletion modes. Its optional third argument selects
`scalar` (the default), `auto` AVX2 dispatch, or `compare` to run both paths.
Insert timing stops immediately after the insert phase; query timing covers
only the corresponding query loop.

```powershell
.\build\benchmarks\vqf_bench_filter_quality.exe
```

By default it runs one million absent-key queries per configuration. Optional
arguments set capacity and absent-query count, for example:

```powershell
.\build\benchmarks\vqf_bench_filter_quality.exe 10000 20000
.\build\benchmarks\vqf_bench_filter_quality.exe 10000 20000 compare
```

The benchmark uses a deterministic sequential-key workload and is intended for
local comparisons, not as evidence against other filters. Run it multiple times
and compare medians before making performance claims; test additional key
distributions before drawing general conclusions. Exact mode uses an
`unordered_set` of keys, whose memory use is not included in the throughput
table.

Build all benchmark programs, including the optional native VQF benchmark.
Run these commands from the project root. For the MSYS2 UCRT64 compiler on
Windows, add both MSYS2 binary directories to `PATH` and use a fresh build
directory so an old CMake generator/compiler cache cannot interfere:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"
cmake -S . -B build-bench `
  -G "MinGW Makefiles" `
  "-DCMAKE_CXX_COMPILER=C:\msys64\ucrt64\bin\g++.exe" `
  "-DCMAKE_MAKE_PROGRAM=C:\msys64\ucrt64\bin\mingw32-make.exe" `
  -DVQF_ENABLE_UPSTREAM_VQF=ON `
  -DVQF_BUILD_BENCHMARKS=ON `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
ctest --test-dir build-bench --output-on-failure
Get-ChildItem .\build-bench\benchmarks\*.exe
```

To build every benchmark in one step, use `cmake --build` with no target name.
Do not mix the old `build` directory with `build-bench-ucrt64`: each directory
has its own generator, options, and executable files. For example, if you used
the `build-bench-ucrt64` directory, build and run all targets from that same
directory:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"
cmake --build build-bench-ucrt64
Get-ChildItem .\build-bench-ucrt64\benchmarks\*.exe

.\build-bench-ucrt64\benchmarks\vqf_bench_filter_quality.exe 100000 1000000 compare
.\build-bench-ucrt64\benchmarks\vqf_bench_competitors.exe 100000 1000000
.\build-bench-ucrt64\benchmarks\vqf_bench_wide_fingerprint.exe 100000 1000000 compare
.\build-bench-ucrt64\benchmarks\vqf_bench_parallel.exe 5000000 0 -1
.\build-bench-ucrt64\benchmarks\vqf_bench_native.exe 100000 1000000
```

Run the Cuckoo/Blocked Bloom comparison (and upstream VQF when enabled):

```powershell
.\build-bench\benchmarks\vqf_bench_competitors.exe 100000 1000000
```

The arguments are nominal item capacity and absent-query count. The benchmark
uses the same sequential workload for each competitor. It reports physical
load and memory because rounded Cuckoo capacity can differ from nominal
capacity. The Bloom row marks deletion churn as unsupported.

Run the separated wide-key/fingerprint comparison (the optional third
argument selects `auto` AVX2 dispatch, `scalar`, or both paths for comparison):

```powershell
.\build-bench\benchmarks\vqf_bench_wide_fingerprint.exe 100000 1000000
.\build-bench\benchmarks\vqf_bench_wide_fingerprint.exe 100000 1000000 compare
```

Its arguments are requested capacity and absent-query count. `Key` and `FP`
are varied independently. `FP bytes` includes packed fingerprint, reference
count, and slot-state arrays; `Exact bytes` is only the exact-key payload.
Compare the VQF's separate 8-bit-tag benchmark as a distinct implementation,
not as another row in the fingerprint-width sweep.

The upstream VQF benchmark reports single-tier and tiered results:

```powershell
.\build-bench\benchmarks\vqf_bench_native.exe
```

Its optional arguments are requested slot count and absent-query count:

```powershell
.\build-bench\benchmarks\vqf_bench_native.exe 100000 1000000
```

Along with the single-tier 90%/95% measurements, this runs a tiered case with
an 88% per-tier ceiling, high-load churn, and query FPR. Tiered query cost
includes checking the active layers; memory includes exact key payload but
excludes unordered-map allocator overhead. Interpret its load using the
reported total allocated slots and tier count, not the single-tier load rows.

The filter test additionally checks colliding fingerprints with a shared
probe-chain start, tombstone reuse behavior, duplicate insert/erase semantics
in both deletion modes, and 20 cycles of erase/reinsert at 95% load.

If CMake is not installed but MinGW is available, compile and run it directly:

```powershell
New-Item -ItemType Directory -Force .\build | Out-Null
g++ -std=gnu++1z -O2 -Wall -Wextra -Wpedantic -I.\include `
  .\src\vqf_filter.cpp .\benchmarks\bench_filter_quality.cpp `
  -o .\build\vqf_bench_filter_quality.exe
if ($LASTEXITCODE -eq 0) {
  .\build\vqf_bench_filter_quality.exe 100000 1000000
}
```
