# Project Context

## Purpose
Aker is an experimental MAC address blocking scheduler for the XMiDT / RDK device
ecosystem. It runs as a long-lived service (daemon) on customer-premises equipment
(CPE, e.g. cable gateways/routers) and applies time-based parental-control style
rules that block specific network devices (identified by MAC address) from
accessing the network during configured time windows.

Schedules are delivered to the device as MsgPack-encoded payloads over the XMiDT
cloud bus (via the Parodus message broker, using WRP — the WebPA Request Protocol).
Aker decodes these schedules, persists them locally, evaluates which MAC addresses
should be blocked at the current time, and invokes an external firewall command to
enforce the blocking. It also reports operational/assurance metrics back to the
cloud and to the system log.

Key capabilities:
- Accept and validate scheduling data via WRP CRUD messages (Create/Read/Update/Delete).
- Support both **weekly** recurring rules and **absolute** (one-time, UTC-anchored) rules.
- Persist the active schedule and an MD5 checksum to disk for restart resilience.
- Evaluate the current time window and call a configurable firewall command to block MACs.
- Emit operational metrics (devices blocked, window transitions, schedule set count,
  MD5 error count, process start time, timezone, schedule-enabled state).
- Provide a standalone command-line utility (`aker-cli`) for printing/inspecting schedules.

## Tech Stack
- **Language:** C (C99 — `-std=c99`, `-D_GNU_SOURCE`)
- **Build system:** CMake (`cmake_minimum_required(VERSION 2.8.7)`), with CTest for tests
- **Serialization:** MsgPack (`msgpack-c`) for schedule encoding/decoding
- **Messaging / transport:**
  - `libparodus` — client library for the Parodus message broker
  - `wrp-c` — WebPA Request Protocol (WRP) message encoding/decoding
  - `nanomsg` — underlying transport used by libparodus
- **Supporting libraries:**
  - `cimplog` — logging abstraction (wrapped by `aker_log.h`)
  - `trower-base64` — base64 encoding
  - `libcurl` — HTTP fetching (used by the `aker-cli` utility for retrieving schedules from a URL)
  - `pthreads` — the scheduler runs on its own thread
- **Optional integrations (compile-time feature flags):**
  - `telemetry2_0` (`ENABLE_FEATURE_TELEMETRY2_0`) — T2 telemetry bus messages
  - Google Breakpad (`INCLUDE_BREAKPAD`) — crash reporting
  - Yocto build mode (`BUILD_YOCTO`) — embedded/cross-compilation target that skips
    `ExternalProject` dependency fetching and coverage flags
- **Testing:** CUnit (`-lcunit`) test framework, with Valgrind (`--leak-check=full`)
  for memory checking and gcov/lcov for coverage
- **CI / quality tooling (from README badges):** GitHub Actions (CI), Codecov,
  Coverity Scan, SonarCloud, LGTM

## Project Conventions

### Code Style
- **Language standard:** C99. Compiled with strict warnings treated as errors:
  `-Werror -Wall -W -Wno-missing-field-initializers`.
- **Indentation:** 4 spaces, no tabs.
- **File headers:** Every source/header file begins with the Apache License 2.0
  header block (Comcast copyright). New files should preserve this header.
- **Section banners:** Source and header files use consistent comment-banner sections
  to organize content, e.g.:
  ```c
  /*----------------------------------------------------------------------------*/
  /*                                   Macros                                   */
  /*----------------------------------------------------------------------------*/
  ```
  Common sections: Macros, Data Structures, File Scoped Variables, Global Variables,
  Function Prototypes, External Functions, Internal functions. Sections with no
  content are explicitly marked `/* none */`.
- **Naming conventions:**
  - Functions: `lower_snake_case` (e.g. `create_schedule`, `process_schedule_data`,
    `aker_metric_inc_device_block_count`).
  - Types/structs: `lower_snake_case` typedef'd, frequently with a `_t` suffix
    (e.g. `schedule_t`, `schedule_event_t`, `mac_address`).
  - Macros/constants: `UPPER_SNAKE_CASE` (e.g. `MAC_ADDRESS_SIZE`, `SERVICE_AKER`).
  - Module-prefixed public APIs use the module name as a prefix (e.g. `aker_metric_*`,
    `aker_mem_*`).
  - Internal/static helpers are sometimes prefixed with a double underscore
    (e.g. `__validate_mac`).
- **Header guards:** Classic `#ifndef __NAME_H__ / #define __NAME_H__ / #endif`
  include guards. C++ interop guarded with `extern "C"` where relevant.
- **Documentation:** Public functions are documented with Doxygen-style `/** ... */`
  comment blocks describing `@param`, `@return`, and `@note`.
- **Memory management:** Allocation is funneled through a custom wrapper layer
  (`aker_mem.c` / `aker_mem.h`) to support instrumentation and leak detection in tests.

### Architecture Patterns
- **Single-binary service with a worker thread.** `main.c` parses command-line
  options, initializes telemetry/metrics, imports any previously-persisted schedule,
  starts the scheduler thread, and runs the main message loop receiving WRP messages
  from Parodus.
- **Modular, single-responsibility translation units.** Each concern lives in its own
  `.c`/`.h` pair:
  - `main.c` — entry point, option parsing, main receive loop.
  - `wrp_interface.c` — WRP message routing/dispatch (CRUD handling).
  - `process_data.c` — applies WRP CRUD operations, persists data + MD5 to disk.
  - `decode.c` — decodes MsgPack schedule payloads into `schedule_t`.
  - `schedule.c` / `schedule_print.c` — the schedule data model, time-window
    evaluation, and human-readable printing.
  - `scheduler.c` — the background thread that periodically evaluates the active
    schedule and invokes the firewall command via `system()`.
  - `time.c` — timezone-aware time handling and "seconds since last Sunday" math
    for weekly rules.
  - `aker_md5.c` / `md5.c` — checksum computation for persisted schedule integrity.
  - `aker_msgpack.c` — MsgPack helpers.
  - `aker_metrics.c` — operational/assurance metrics collection and reporting.
  - `aker_mem.c` — memory allocation wrappers.
  - `aker_help.c` — CLI help/usage text.
  - `cli.c` — standalone `aker-cli` tool (built only for non-Yocto builds).
- **Schedule data model.** A `schedule_t` holds a timezone string, a shared MAC
  address table, a `weekly` linked list of recurring `schedule_event_t` rules, an
  `absolute` linked list of one-time rules, and a configurable report rate. Each
  `schedule_event_t` is a singly-linked-list node using a C99 flexible array member
  (`uint32_t block[]`) to store indices of MACs to block at that time.
- **External dependencies built via CMake `ExternalProject_Add`.** For standard
  (non-Yocto) builds, third-party libs are fetched from pinned Git tags/commits and
  built into a local `_install` prefix. Yocto builds expect these to be provided by
  the build environment.
- **Configuration via command-line options** (see `main.c` `getopt_long`):
  `--parodus-url`, `--client-url`, `--firewall-cmd`, `--data-file`, `--md5-file`,
  `--max-macs`, `--device-id`, `--help`.

### Testing Strategy
- **Framework:** CUnit. Tests live under `tests/` and are registered as CTest targets
  via `add_test(...)` in [tests/CMakeLists.txt](tests/CMakeLists.txt).
- **Per-module test executables.** Each test binary links the specific production
  source files it exercises plus shared stubs/mocks. Examples:
  - `test_schedule`, `test_decode`, `test_time`, `test_time_changes`,
    `test_wrp_interface`, `test_aker_msgpack`, `test_aker_metrics`, `test_md5`,
    `test_process_data`, `test_process_ret_now`, `test_process_is_create_ok`,
    `test_scheduler`, `test_reporter`.
- **Mocks and stubs:**
  - `libparodus_mock.c` — mock of the Parodus client so tests run without a broker.
  - `common_test_stubs.c` — shared stub implementations.
  - `mem_wrapper.c` / `mem_wrapper.h` — instrumented memory wrapper enabling
    allocation-failure injection and leak detection.
- **Test fixtures / data:** JSON schedule definitions (`scheduler_dataN.json`) with
  generated C header counterparts (`scheduler_dataN.h`), plus timezone fixtures
  (`tz1.h`, `tz2.h`) and assorted `testN.h` payload headers.
- **Memory checking:** Tests are run under Valgrind
  (`valgrind --leak-check=full --show-reachable=yes -v`) unless `DISABLE_VALGRIND`
  is set.
- **Coverage:** Built with gcov instrumentation (`-fprofile-arcs -ftest-coverage -O0`);
  coverage is uploaded to Codecov in CI. Note: `terminate_scheduler_thread()` exists
  specifically so the scheduler thread can be stopped cleanly in tests without
  `SIGTERM` killing the process before gcov data is written.
- **How to run:**
  ```
  mkdir build
  cd build
  cmake ..
  make
  make test
  ```

### Git Workflow
- **Hosting:** GitHub (`xmidt-org/aker`; historically `Comcast/aker`).
- **Default branch:** `main`.
- **Contributions:** Fork-and-pull-request model. A Comcast Contributor License
  Agreement (CLA) must be signed before code is merged (enforced automatically on PR).
- **Pull request guidelines** (from [CONTRIBUTING.md](CONTRIBUTING.md)):
  - Narrowly focused, with no more than 3–4 logical commits.
  - Address no more than one issue when possible.
  - Must be reviewable in the GitHub code review tool.
  - Link related issues using `#<issue-number>` in commit/PR messages.
  - Behavior changes must be documented in the README or wiki.
  - Note: CONTRIBUTING.md references idiomatic Go formatting/testing language, but the
    codebase is C — treat the Go-specific wording as stale boilerplate and follow the
    C conventions described above.
- **Versioning:** Semantic Versioning. Project version is set in the top-level
  [CMakeLists.txt](CMakeLists.txt) (`set(VERSION "1.2.0")`).
- **Changelog:** Maintained in [CHANGELOG.md](CHANGELOG.md) following the
  "Keep a Changelog" format (Added / Changed / Fixed / Security sections per release).

## Domain Context
- **XMiDT / WebPA ecosystem:** Aker is a downstream service in the XMiDT platform used
  to manage broadband CPE at scale. Cloud-originated commands reach the device through
  Parodus (the on-device message broker) using WRP messages.
- **WRP (WebPA Request Protocol):** The message format/envelope used to communicate
  between the cloud and device services. Aker handles WRP CRUD operations to set,
  retrieve, and clear schedules. Relevant identifiers: service name `aker`, parameter
  paths `schedule` and `now`.
- **MsgPack payloads:** Schedule definitions are transmitted as MsgPack binary blobs
  and decoded into the internal `schedule_t` model.
- **MAC address blocking:** The core domain action. MACs are stored as
  `"11:22:33:44:55:66"` strings (18-byte buffer including null terminator), validated
  for format, and enforced via an external firewall command supplied at startup
  (`--firewall-cmd`).
- **Schedule semantics:**
  - **Weekly rules** are anchored to "seconds since the most recent Sunday" and recur
    until a new schedule arrives.
  - **Absolute rules** are anchored to UTC Unix time and apply once within a matching
    window.
  - A `report_rate_s` controls how often metrics are reported
    (`0` = none, `3600` = hourly minimum, `86400` = daily, `604800` = weekly).
- **Timezone handling:** Schedules carry a timezone string; time evaluation and
  assurance-metric timestamps are timezone-aware (a past bug around the timezone offset
  for assurance metrics was fixed in v1.2.0).
- **Persistence & integrity:** The active schedule is written to a data file alongside
  an MD5 checksum file so the device can restore and verify its schedule across
  restarts; MD5 mismatches are tracked as an error metric.

## Important Constraints
- **C99 only**, and all builds must compile cleanly with `-Werror -Wall` — warnings
  break the build.
- **Embedded target support:** Must build in a Yocto/cross-compilation environment
  (`BUILD_YOCTO`), where external dependencies are provided by the build system rather
  than fetched via `ExternalProject_Add`, and coverage/CLI targets are excluded.
- **Pinned dependency versions:** External libraries are fetched at specific Git
  commits/tags (see [CMakeLists.txt](CMakeLists.txt)). Changing these pins can affect
  reproducibility and compatibility.
- **MAC limit:** A maximum number of MAC addresses can be enforced via `--max-macs`
  (defaults to `INT_MAX` in the service; the `aker-cli` utility caps at 128). MAC
  address format is validated.
- **Security / input handling:** Schedule payloads arrive from the network and are
  parsed in C; decoding (`decode.c`), MAC validation, and bounded buffers are
  security-sensitive. The scheduler invokes an external command via `system()` with the
  operator-provided `--firewall-cmd`, so that command and its inputs must be trusted/controlled.
- **Licensing:** Apache License 2.0. All source files must retain the license header,
  and contributions require a signed Comcast CLA.
- **Experimental status:** The README explicitly describes Aker as "an experimental
  MAC address blocking scheduler."

## External Dependencies
- **Parodus / libparodus** — on-device message broker client; primary channel for
  receiving schedules and sending responses/metrics. Configured via `--parodus-url`
  and `--client-url`.
- **wrp-c** — WRP message encode/decode library.
- **msgpack-c** — MsgPack serialization for schedule payloads.
- **nanomsg** — transport layer beneath libparodus.
- **cimplog** — logging backend.
- **trower-base64** — base64 utility.
- **libcurl** — HTTP client used by `aker-cli` to fetch schedules from a URL.
- **Telemetry 2.0 (T2)** — optional telemetry bus (`telemetry_busmessage_sender`) when
  `ENABLE_FEATURE_TELEMETRY2_0` is enabled.
- **Google Breakpad** — optional crash reporting when `INCLUDE_BREAKPAD` is defined.
- **External firewall command** — an operator-provided executable (passed via
  `--firewall-cmd`) that performs the actual MAC blocking on the device.
- **Project wiki** — additional documentation lives at
  https://github.com/Comcast/aker/wiki (referenced from the README).

## Additional Notes
- **Single repository in this workspace.** Despite the request to review "all the repos
  under the current space," the workspace contains exactly one repository — `aker` —
  rooted at the workspace folder. There are no nested or sibling repositories present;
  the projects referenced in [CMakeLists.txt](CMakeLists.txt) (msgpack-c, nanomsg,
  wrp-c, libparodus, cimplog, trower-base64) are **external dependencies fetched at
  build time**, not checked-in sub-repositories. If additional repositories are expected,
  they are not part of the current workspace and would need to be added.
- **Repository layout:**
  - `src/` — all production C sources and headers, plus the service `CMakeLists.txt`.
  - `tests/` — CUnit tests, mocks/stubs, and JSON/header test fixtures.
  - Top-level: `CMakeLists.txt` (build + external deps), `README.md`,
    `CONTRIBUTING.md`, `CHANGELOG.md`, `LICENSE`, `NOTICE`.
- **`aker-cli`** is a developer/diagnostic tool (not installed as part of the service)
  for inspecting and printing schedules; it is only built for non-Yocto builds and
  reuses the production source files plus the Parodus mock.
- **Stale documentation caveat:** `CONTRIBUTING.md` contains Go-oriented language
  ("golang's standard testing tools", "idiomatic golang code formatting") that does not
  match this C codebase — follow the C/CMake/CUnit conventions documented here instead.
