# Blob allocator simulator

The simulator measures the original C++ blob allocator with Python workloads.

The simulator uses the original C++ blob allocator through a small Python adapter.
All simulator code, tests, and documentation belong in this directory.
The simulator reads shared MemCache sources without modifying them.

## CLI help

The `--help` output includes the text in [workload_help.txt](workload_help.txt).
That text defines operation selection and release order for each workload.
The option descriptions must also state defaults and workload restrictions.
Requesting help must not compile code or start a measurement.
Every measurement run must invoke the standalone incremental build.

## Allocation and eviction

The region starts empty. Each step attempts one allocation.
The simulator samples its size from `--block-sizes`.
`--steps` counts allocation attempts, including failures. Its default is 100000.
Evictions do not consume steps. Releases are not randomly selected operations.

Calculate occupancy as reserved bytes divided by capacity, expressed as a percentage.
Reserved bytes include alignment waste and max-value-align padding.
After a successful allocation reaches or exceeds the maximum, the simulator pauses allocations.
It evicts live blocks until occupancy reaches or falls below the minimum.
Then it resumes allocation attempts. There is no background eviction thread.

Set `--min-water-level 80` and `--max-water-level 90` to use the defaults.
Supply integers without a percent sign. Require `0 <= minimum < maximum <= 100`.
The simulator uses exact integer comparisons:

```text
Start: 100 * reserved >= maximum * capacity
Stop:  100 * reserved <= minimum * capacity

Allocation: occupancy rises to 90% or more
Eviction:   occupancy falls to 80% or less
Allocation: resumes
```

An allocation can exceed the maximum. Releasing a whole block can cross below the minimum.
The simulator completes eviction even if the last allocation attempt triggers it.
Each eviction selects uniformly from successfully allocated blocks that remain live.
Selection does not depend on size, age, or address.
The seed controls request sizes and eviction targets through separate random generators.
Eviction does not modify the request-size sequence.

The CLI no longer accepts `--workload`, `--alloc-probability`, or `--compare`.
Use a separate invocation for each alignment mode.

## Block-size probabilities

`--block-sizes` determines request sizes for allocation attempts.
`--block-size` is an alias.

For example, `'4KiB:25%,64KiB:25%,1MiB:50%'` assigns a sampling probability to each size.
Each allocation attempt samples independently.
Percentages describe request counts, not shares of bytes.
They do not guarantee exact counts or successful allocations.

When all percentages are absent, the sizes have equal sampling probabilities.
For example, `'4KiB,64KiB,1MiB'` gives each size a probability of one third.
When any percentage is present, every size must specify a percentage.
The percentages must total 100%.

## Allocation failures and invalid behavior

An ordinary allocation failure means insufficient free capacity or external fragmentation.
External fragmentation means that separate free ranges cannot accommodate the required contiguous allocation.
The simulator records the failure and continues with the next operation.
It does not retry the request automatically.

Failed requests never become eviction candidates.
A failure does not trigger eviction or a retry.
Failures can repeat below the maximum water level.
This simulator does not model MemCache fragmentation recovery or LRU selection.

A double free is a second release of the same request.
Unknown handles, double frees, invalid sizes, and unexpected native failures abort the measurement.
The CLI reports the operation index and error cause.
It exits with a nonzero status.
It does not print a success summary or write a normal report.

## Allocator modes

The `4k-align` mode rounds each request up to a multiple of 4 KiB.
Max-value-align mode reserves at least the largest aligned request size successfully allocated so far.
Releasing the largest block does not reduce this maximum.

## Reports

The terminal report includes operation counters, fragmentation metrics, and a histogram of free ranges.
Each free range contributes once to one histogram bucket.
The byte total uses actual sizes rather than bucket limits.
Free space excludes padding inside active allocations.
The terminal prints exact byte counts with readable binary sizes in parentheses.
For example, `94,208 bytes (92.00 KiB)` describes the same size in two forms.
Tables use spaces to align columns, including the histogram.
JSON and CSV retain their numeric values without display formatting.
The CLI pairs each weight with its block size, for example, `4.00 KiB : 33.33%`.
These percentages describe size selection for allocation attempts.
Rounded percentages can total slightly more or less than 100%.

`--json-report-path` enables a JSON report at the specified path.
`--csv-report-path` enables a CSV report at the specified path.
Without these options, the simulator creates neither report file.
The simulator measures the final state before releasing remaining blocks.

## Run the simulator

Install a C++17 compiler and Python development headers through your operating system.
Install the Python build dependencies from the repository root:

```bash
python3 -m pip install -r script/tools/sim/blob_allocator/requirements.txt
```

The requirements include CMake, pybind11, and nlohmann JSON headers.
Use the same Python interpreter to install dependencies and run the simulator.
Supply nlohmann JSON headers from the repository submodule, the `nlohmann_json` Python package, or a system installation.
The tool does not install dependencies automatically.

If the build reports a missing `nlohmann/json.hpp`, install the headers with the interpreter that runs the simulator:

```bash
python3 -m pip install nlohmann_json
```

Then repeat the simulator command.
For a virtual environment, use its Python interpreter in both commands.
The simulator finds the package headers automatically.

From the repository root, run:

```bash
python3 script/tools/sim/blob_allocator/allocator_sim.py --help
python3 script/tools/sim/blob_allocator/allocator_sim.py \
  --capacity 64MiB --steps 10000 \
  --block-sizes '4KiB:25%,64KiB:25%,1MiB:50%' \
  --mode 4k-align
```

The `--mode` choices are `4k-align` and `max-value-align`.
The default is `4k-align`.
Run each mode separately with the same seed to use the same request-size sequence.
The tool always configures and invokes the incremental native build before measurement.
Unmodified sources do not require recompilation.

Set `BLOB_SIM_JSON_HEADERS` to a directory that contains `nlohmann/json.hpp` when the repository headers are absent.
The driver passes this directory to its standalone CMake project.
An explicit path takes priority over automatic discovery.
Automatic discovery checks the repository headers, the Python package, and system headers in that order.
For example, if the file is `/opt/json/include/nlohmann/json.hpp`, use `/opt/json/include` as the header directory.

```bash
BLOB_SIM_JSON_HEADERS=/opt/json/include python3 script/tools/sim/blob_allocator/allocator_sim.py
```

The directory must contain the headers, not just an empty `nlohmann` directory.

## Fragmentation formulas and percentages

The final report describes the active allocations at the end of the workload.
It does not sum fragmentation across past operations.
All percentage fields use the formulas below.
JSON and CSV store percentages on a scale from zero to 100.
The terminal rounds these values to two decimal places.

### Internal fragmentation

Internal fragmentation is padding inside active allocations.
The allocator reserves this padding, so other requests cannot use it.

```text
R = bytes reserved for active allocations
Q = bytes requested by active allocations
I = R - Q
Internal fragmentation percentage = 100 * I / R

Example: two active requests
Requested:  [ 5 KiB ][ 5 KiB ]                  Q = 10 KiB
Reserved:   [ 5 KiB | 3 KiB padding ] x 2      R = 16 KiB
Padding:                                      I =  6 KiB
Percentage: 100 * 6 / 16 = 37.5%
```

Reserved bytes form the denominator.
Capacity and requested bytes do not form this denominator.
When reserved bytes equal zero, the percentage has no defined value.
The terminal prints `N/A`, JSON stores `null`, and CSV leaves the field empty.

The tool also separates two sources of padding:

```text
Alignment waste per block = align_up(requested, 4096) - requested
Extra padding per block = reserved - align_up(requested, 4096)
I = sum(alignment waste) + sum(extra padding)
```

Extra padding comes from max-value-align in this simulator.
The maximum grows after successful large allocations.
It does not decrease after releases.

### External fragmentation

External fragmentation describes separate free ranges.
A request needs one range large enough for its effective size.

```text
F = sum of exact free-range sizes
L = size of the largest free range
External fragmentation percentage = 100 * (F - L) / F

[16 KiB free][16 KiB used][16 KiB free][16 KiB used]
F = 32 KiB
L = 16 KiB
Percentage: 100 * (32 - 16) / 32 = 50%
```

The denominator is total free bytes.
Internal padding does not contribute to free bytes.
When free bytes equal zero, the percentage has no defined value.
The report uses the same undefined-value conventions as internal fragmentation.

This percentage measures free space outside the largest range.
It does not mean that this space is permanently unusable.
Smaller requests can still use smaller ranges.

For each failed allocation, the tool calculates its effective size before the attempt:

```text
4k-align: effective = align_up(requested, 4096)
max-value-align: effective = max(align_up(requested, 4096), historical maximum)

effective > F             -> insufficient free capacity
L < effective <= F         -> external fragmentation
```

In the example, a 24 KiB request fails from external fragmentation.
There are 32 KiB free, but each free range contains only 16 KiB.
A 16 KiB request can still succeed.

### Workload percentages

Block-size percentages specify probabilities for allocation attempts.
For example, `4KiB:25%` gives that size a sampling probability of 0.25.
The observed counts can differ because each selection is random.
These percentages do not describe fragmentation or successful allocation counts.

### Histogram accounting

Histogram intervals do not overlap.
Each contiguous free range appears in one interval.

```text
Range sizes:                    12 KiB, 16 KiB, 16 KiB
Bucket:                         (8 KiB, 16 KiB]
Range count:                    3
Actual bytes:                   44 KiB
```

The byte total is 44 KiB, not three times the 16 KiB bucket limit.
A 16 KiB range never also appears as two 8 KiB ranges.
The report prints integer bytes to preserve exact sizes.
Divide bytes by 1073741824 to express a total in GiB.


## Tests

Run the Python tests without compiling native code:

```bash
python3 -m unittest discover -s script/tools/sim/blob_allocator/tests -v
```

Enable the native integration tests after installing the build prerequisites:

```bash
BLOB_SIM_NATIVE_TESTS=1 python3 -m unittest discover \
  -s script/tools/sim/blob_allocator/tests -v
```

Without that environment variable, the test runner marks native tests as skipped.
A skip does not establish that native code works.

Set `BLOB_SIM_PYTHON_HEADERS` to the Python header directory when CMake cannot locate an existing installation.
The directory must contain `Python.h` for the current interpreter.

## Sampling and eviction counters

`--sample-every` counts allocation attempts. Its default is 1000.
Periodic samples follow any eviction that the allocation triggers.
The simulator also records initial, eviction-start, eviction-end, and final samples.
`allocation_index` counts allocation attempts.
`operation_index` counts allocation attempts plus individual releases.
Eviction samples can share an allocation index.

`eviction_cycles` counts completed cycles. `evictions` counts released blocks.
`evicted_reserved_bytes` sums the reserved bytes returned by eviction over the entire run.
This cumulative counter is not current free space.
For water-level runs, releases equal evictions and skipped releases equal zero.
Occupancy percentage uses capacity as its denominator.
Fragmentation percentages retain the denominators defined above.

## Custom experiments

`run_simulation(config)` runs the water-level policy and always invokes the build first.
The CLI configuration contains sizes, weights, capacity, steps, seed, mode, sample interval, and both water levels.
The configuration uses numeric percentages such as `80`, not strings such as `80%`.

`run_workload(sequence, capacity, mode='4k-align', sample_every=1000)` runs explicit `Alloc` and `Free` operations.
This custom runner does not add eviction. Use it for controlled experiments and allocator tests.
It measures one mode per invocation.
