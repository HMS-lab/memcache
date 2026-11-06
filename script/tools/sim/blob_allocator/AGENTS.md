# Blob allocator simulator

## Purpose and location

Implement a Python simulator for the existing `MmcBlobAllocator` class.
Use the actual C++ allocator to measure internal and external fragmentation.

Keep simulator code, tests, examples, and documentation in `script/tools/sim/blob_allocator/`.
Place simulator tests in the local `tests/` directory.
Place simulator instructions in the local `README.md`.
These locations replace the locations in the earlier implementation plan.

Treat the simulator as an independent internal program.
The user does not intend to merge it into the main MemCache project.
Do not modify repository files outside this simulator directory.
Read shared allocator sources and necessary headers without modifying them.
Do not add simulator integration to the main project.

Keep generated binaries in a local `build/` directory inside this simulator directory.
Exclude generated files through a local `.gitignore` file.
Keep generated reports in a user-selected location.
Do not commit generated files.

## Allocator boundary

Do not modify these files:

- `src/memcache/csrc/meta_service/mmc_blob_allocator.cpp`
- `src/memcache/csrc/meta_service/mmc_blob_allocator.h`

Compile the original allocator source directly.
Do not copy or reimplement its allocation algorithm.
Keep C++ code limited to a thin pybind11 adapter and necessary standalone support.
Implement workloads, metrics, histograms, and reports in Python.
Run operations sequentially in the first version.

## Invalid behavior

Reject zero and negative allocation sizes.
Reject sizes whose alignment would overflow `uint64_t`.
Reject unknown handles and handles that the simulator already freed.
Do not ignore a double free.
Translate unsuccessful releases and unexpected native failures into Python exceptions.
Abort the entire measurement when any of these errors occurs.
Report the operation index and the error cause.
Exit the CLI with a nonzero status.
Do not print a success summary or write a normal report for an aborted measurement.

Ordinary allocation failures from insufficient capacity or external fragmentation remain valid measurement results.
A skipped release for an allocation that failed is also valid.
Track these skipped releases separately from invalid or repeated releases.

## Build

Provide a standalone build that excludes the full service and wheel.
Do not require NPU hardware, CANN libraries, or a running MetaService.

Use the Python driver as the compilation entry point.
Let the driver invoke CMake for the standalone project in this directory.
Let the driver invoke the standalone build through `cmake --build`.
Reference the shared allocator source through its existing repository path.
Do not invoke or modify `script/build_and_pack_run.sh` or other main project build scripts.
Do not modify main project CMake files, package files, tests, documentation, or ignore rules.

The user explicitly requires an independent build for this internal tool.
This requirement replaces the repository build-entry rule for the simulator.
It also replaces the build integration described in the earlier implementation plan.

Use the interpreter that starts the Python driver to compile the extension.
Invoke the standalone build before every measurement run.
Use incremental compilation to rebuild modified sources.
Do not provide `--no-build` or another option that skips the build.
Abort the measurement if the build fails.
Do not compile code when Python imports the driver module.
Report missing prerequisites without installing them automatically.

## Workloads and CLI

Provide an importable Python API for controlled Alloc/Free experiments.
The CLI uses allocation attempts and synchronous water-level eviction.
Start eviction at or above maximum occupancy. Stop at or below minimum occupancy.
Use reserved bytes divided by capacity. Include padding.
Defaults: minimum 80%, maximum 90%. Require integer levels: 0 <= minimum < maximum <= 100.
Support random and LRU eviction through --eviction-policy. Default: random.
Select uniformly from successful live allocations for random eviction.
Select the least recently used block for LRU eviction. Allocation and reads count as use.
Do not select failed requests.
Count steps as allocation attempts. Eviction does not consume steps.
Treat --steps as a minimum. Stop after the first eviction at or after that count.
Collect the final histograms before and after that eviction.
At each --sample-every multiple, schedule a histogram pair at the next eviction.
Combine pending intervals into one pair. Label columns with the actual allocation count.
Print separate history tables for the upper and lower occupancy watermarks.
Retain actual occupancy and the eviction phase in JSON histogram records.
Abort if no configured request can fit before the final eviction.
Keep interval sampling in the explicit Alloc/Free API.
Show CLI progress automatically. Do not add progress options or explanations to user documentation.
Print 100% only after the final sample succeeds.
Do not retry failures or trigger eviction because of a failure.
Keep size, eviction, classification, and read random generators separate and seeded.
Preserve the existing size and random eviction streams.
Derive new streams from stable seed strings. Do not use Python hash().
Remove workload selection, allocation probability, and comparison options.
Keep help and README consistent. Help must not compile code.

Accept distributions through `--block-sizes` with `--block-size` as an alias.
For example, accept `'4KiB:25%,64KiB:25%,1MiB:50%'`.
Treat percentages as sampling probabilities for allocation attempts.
Use equal probabilities when all percentages are absent.
Require percentages for every entry when any percentage is present.
Require explicit percentages to total 100%.
Do not promise exact request counts for sampled distributions.
Use a local random generator with a configurable seed.

Use `4k-align` mode by default.
Support max-value-align through --mode. Measure one mode per invocation.
Keep run_workload for explicit Alloc/Free experiments without automatic eviction.

Generate synthetic reads before every allocation attempt, including failed attempts.
Default: four reads. Allow a nonnegative integer through --reads-per-allocation.
Skip reads when no blocks are live. Permit repeated reads of the same block.
Use Python bookkeeping only. Do not read payloads or call the native allocator for reads.
With zero reads, LRU behaves as FIFO.
Use --hot-block-percent for the hot assignment probability. Default: 20.
Use --hot-read-percent for the hot group selection probability per read. Default: 80.
Require integer percentages from zero through 100.
Keep each successful allocation's classification until release.
Draw a classification for each allocation attempt. Discard it after failure.
Select uniformly within the chosen group. Use the other group when the chosen group is empty.
Run the same read model for both eviction policies.
Do not promise exact group sizes, read counts, or improved fragmentation with LRU.
Keep native operation indices separate from synthetic read counters.

## Metrics and reports

Use `GetFreeSegments()` to retrieve exact free ranges.
Derive each histogram, free-byte total, and largest range from one snapshot.
Count each free range exactly once.
Do not divide a range into hypothetical smaller ranges.
Sum actual range sizes within each histogram bucket.

Report free space separately from internal fragmentation.
Internal fragmentation means reserved bytes minus active requested bytes.
Free space excludes padding inside allocated blocks.
Include alignment and max-value-align padding in the effective allocation size.

Classify an ordinary failure as external fragmentation when the effective size fits total free space but exceeds every free range.
Classify it as insufficient capacity when the effective size exceeds total free space.
Treat unexpected native failures as errors.

Measure the final state before releasing remaining allocations.
Print a terminal report by default.
Write JSON only when `--json-report-path` specifies a destination.
Write CSV only when `--csv-report-path` specifies a destination.
Use `--csv-report-path`, not `csv--report-path`.

## Documentation

Use the `asd-ste100:asd-ste100` skill for all simulator documentation.
Apply its strict rules to assistant instructions, procedures, CLI help, and error messages.
Apply its rules for explanatory prose to README descriptions.
Preserve technical conditions and uncertainty when simplifying text.
Define necessary technical terms.
Do not claim certified compliance with the official ASD dictionary.

Read the skill from its available catalog location.
Run its `scripts/ste-lint.py` on new or modified documentation when available.
Review advisory findings manually.
Keep all simulator documentation in this directory.

## Verification

Use local Python tests that do not require a running service.
Test sampling, reproducibility, report options, and eviction thresholds.
Test LRU promotion, FIFO with zero reads, hot/cold selection, empty groups, and access counters.
Test that access settings do not modify size sampling or random eviction behavior.
Test that CLI help describes operation selection, release order, failures, and workload restrictions.
Test that requesting help does not compile code or start a measurement.
Test internal waste, external fragmentation, coalescing, and max-value-align padding against the real allocator.
Test histogram accounting with sizes that fall between bucket boundaries.
Check that histogram counts and bytes match the exact free ranges.
Check that reserved bytes plus free bytes equal capacity.
Check that requested bytes plus internal fragmentation equal reserved bytes.
Do not report native tests as passed unless the native extension compiled and the tests ran.

Follow repository naming, formatting, named-constant, and license requirements.
Leave unrelated user files unchanged.

## User-facing mode name

Accept exactly two `--mode` values: `4k-align` and `max-value-align`.
Use those names in reports, help, and documentation.
Do not provide `--compare`.
Keep the existing C++ implementation unchanged.
Explain each fragmentation percentage with its formula and denominator.
Use actual current allocations and exact free ranges for the final report.
Do not present cumulative padding as current internal fragmentation.
