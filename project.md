# Project Context

## Purpose
Aker is an experimental MAC address blocking scheduler developed by Comcast (under the xmidt-org organization). It manages time-based network access control by scheduling when specific MAC addresses should be blocked or allowed through a firewall. It communicates via the Parodus/WebPA messaging infrastructure using WRP (Web Routing Protocol) messages and processes schedule data encoded in MessagePack format.

## Tech Stack
- **Language:** C (C99 standard)
- **Build System:** CMake (minimum version 2.8.7)
- **Serialization:** MessagePack (msgpack-c)
- **Messaging:** libparodus / nanomsg (for communication with the Parodus service)
- **Protocol:** WRP-C (Web Routing Protocol)
- **Encoding:** trower-base64
- **Logging:** cimplog
- **Hashing:** Custom MD5 implementation
- **Threading:** POSIX Threads (pthread)
- **HTTP:** libcurl
- **Platform:** Linux (primary), with Mac support

## Project Conventions

### Code Style
- C99 standard (`-std=c99` flag)
- Strict compiler warnings: `-Werror -Wall -Wno-missing-field-initializers`
- GNU source extensions enabled (`-D_GNU_SOURCE`)
- Header files use include guards with double-underscore prefix/suffix pattern (e.g., `__SCHEDULE_H__`) or single-underscore pattern (e.g., `_PROCESS_DATA_H_`)
- Source files organized with clear section dividers using comment blocks:
  - `/*--- Macros ---*/`
  - `/*--- Data Structures ---*/`
  - `/*--- File Scoped Variables ---*/`
  - `/*--- Function Prototypes ---*/`
  - `/*--- External Functions ---*/`
- Apache 2.0 license header required at the top of every source file
- Function documentation uses Doxygen-style `@brief`, `@param`, `@return` comments in headers

### Architecture Patterns
- **Modular design:** Each functional area has a dedicated `.c`/`.h` pair (schedule, scheduler, time, decode, wrp_interface, process_data, etc.)
- **Scheduler thread model:** A background thread (`scheduler_start`) manages the time-based schedule execution and firewall command invocation via `system()`
- **Message processing pipeline:** Incoming WRP messages → decode → process_data → schedule update → scheduler re-evaluation
- **Data encoding:** Schedules are transmitted as MessagePack-encoded binary data with MD5 checksums for integrity validation
- **Memory management:** Custom memory wrappers (`aker_mem.c/h`) for allocation tracking
- **External dependencies built from source:** CMake `ExternalProject_Add` pulls and builds dependencies at specific Git commits/tags
- **Yocto build support:** Conditional compilation paths (`BUILD_YOCTO`) for embedded Linux builds vs. development builds

### Testing Strategy
- **Framework:** CUnit (`CUnit/Basic.h`)
- **Memory testing:** Valgrind (`--leak-check=full --show-reachable=yes`) for all tests unless `DISABLE_VALGRIND` is set
- **Code coverage:** gcov via `-fprofile-arcs -ftest-coverage` flags
- **CI/CD:** GitHub Actions workflows
- **Coverage reporting:** Codecov integration
- **Static analysis:** Coverity Scan, SonarCloud, LGTM
- **Test structure:** One test executable per module (e.g., `test_schedule`, `test_wrp_interface`, `test_process_data`)
- **Test data:** JSON schedule data with corresponding C header files for test fixtures (`scheduler_data0-4.json/.h`)
- **Mocking:** Custom mock files for libparodus (`libparodus_mock.c`) and memory wrapper (`mem_wrapper.c`)
- **Test execution:** Via CTest (`make test`)

### Git Workflow
- **Main branch:** `main`
- **Versioning:** Semantic Versioning (SemVer)
- **Changelog:** Follows [Keep a Changelog](http://keepachangelog.com/en/1.0.0/) format
- **Pull Requests:** Narrowly focused, 3-4 logical commits max, linked to issues
- **CLA required:** Comcast Contributor License Agreement must be signed before merge
- **Current version:** 1.2.0

## Domain Context
- **Network access control:** The system blocks/unblocks MAC addresses on a schedule to control device network access (parental controls / device management)
- **Schedule types:** Supports both "absolute" (one-time UTC-based) and "weekly" (recurring) schedules
- **Timezone-aware:** Schedules include timezone information for correct local time interpretation
- **Xmidt ecosystem:** Part of the Comcast xmidt platform for CPE (Customer Premises Equipment) device management
- **Parodus integration:** Communicates with the Parodus client daemon which provides the WebPA protocol bridge to cloud services
- **Firewall integration:** Executes firewall commands to actually enforce MAC blocking/unblocking
- **Report rate:** Configurable reporting intervals for operational metrics (hourly to weekly)

## Important Constraints
- **License:** Apache License 2.0
- **Platform:** Primarily targets Linux-based embedded systems (Yocto builds)
- **Real-time scheduling:** Must handle timezone changes and time-based event triggering accurately
- **Memory safety:** Valgrind-clean requirement for all tests
- **Thread safety:** Scheduler runs in a separate thread; shared state must be protected
- **MAC address limit:** Configurable maximum (`max_macs`, defaults to `INT_MAX`)
- **MAC address format:** `"11:22:33:44:55:66"` (18 characters including null terminator)

## External Dependencies
- **libparodus** — Client library for Parodus/WebPA communication (Comcast, GitHub)
- **wrp-c** — WRP message encoding/decoding library (Comcast, GitHub)
- **msgpack-c** — MessagePack serialization library (GitHub)
- **nanomsg** — Lightweight messaging library for IPC (v1.1.2)
- **trower-base64** — Base64 encoding/decoding (Comcast, GitHub)
- **cimplog** — Logging library (Comcast, GitHub)
- **libcurl** — HTTP client library (system package)
- **CUnit** — Unit testing framework (system package, test-only)
- **Valgrind** — Memory analysis tool (system package, test-only)

## Additional Notes
- The project includes a CLI utility (`aker-cli`) for printing/inspecting schedules, built only in non-Yocto environments
- All external dependencies are pinned to specific Git commits for reproducible builds
- The `tests/` directory contains pre-generated test data headers derived from JSON schedule definitions
- Metric reporting was added in v1.1.0 and enhanced with assurance metrics and MAC validation in v1.2.0
- **[Unreleased] T2 Downtime Notifications:** A new feature (`aker_notify.c/h`) emits JSON payloads via `t2_event_s` for four downtime lifecycle events (`DOWNTIME_STARTING_SOON`, `DOWNTIME_STARTED`, `DOWNTIME_ENDING_SOON`, `DOWNTIME_ENDED`) with 15-minute pre-notifications. Guarded by `ENABLE_FEATURE_TELEMETRY2_0` build flag. Designed as a pre-computed notification list diffed from consecutive schedule events.
- The `docs/changes/` directory contains design documents, proposals, and specs for in-progress features
- Please pay special attention to: understand the code for all the repos under the current workspace
