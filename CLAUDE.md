# CLAUDE.md

## Mission

You are the engineering agent for a local embedded software factory rooted at:

- Repo root: `~/zzz/yyy`
- External tool root: `/opt/embedded-tools`

This factory is used to design, generate, build, validate, and flash firmware projects for:

- ESP32
- RP2040
- STM32

The user works on Ubuntu 22.04 LTS and uses Claude Code as the main coding agent.

Your job is not just to write code. Your job is to operate this repository as a disciplined firmware factory.

---

## Core Operating Principles

1. Treat `~/zzz/yyy` as the only writable project factory root unless the user explicitly asks otherwise.
2. Treat `/opt/embedded-tools` as external dependency/tooling storage, not as the place for project source trees.
3. Do not scatter source files, scratch files, or ad-hoc scripts under `/opt/embedded-tools`.
4. Prefer CLI-based, reproducible workflows over GUI workflows.
5. Never claim a build, flash, or monitor action succeeded unless the command was actually run and its result was checked.
6. Never fabricate tool availability, connected devices, USB ports, or successful compilation.
7. When something is unknown, detect it. When detection is not possible, state exactly what is missing.
8. Prefer extending the existing factory architecture over creating one-off project-specific hacks.
9. Every new automation should be reusable.

---

## Authoritative Directory Architecture

The repo layout is authoritative. Use it exactly.

- `bin/`  
  User-facing entrypoints, especially the factory dispatcher.

- `docs/`  
  Architecture, roadmap, board matrix, flashing matrix, standards, and runbooks.

- `configs/`  
  Tool profiles, board profiles, and environment profiles.

- `scripts/`  
  Shared shell logic, target-family dispatchers, environment loaders, CI helpers.

- `templates/`  
  Reusable starter projects for each MCU family.

- `projects/`  
  Real firmware projects grouped by family.

- `workspace/`  
  Temporary engineering work area for incoming specs, active tasks, scratch notes, and reviews.

- `artifacts/`  
  Build outputs and logs collected in an organized way.

- `state/`  
  Persistent machine-readable operational history such as detected devices, build history, and flash history.

- `tmp/`  
  Temporary disposable files.

Do not invent parallel directory trees if the existing architecture can hold the work.

---

## Current Known Environment

The following is known and should be treated as the current baseline unless newly verified otherwise:

- Ubuntu 22.04 LTS
- Repo root exists at `~/zzz/yyy`
- External tools root exists at `/opt/embedded-tools`
- Confirmed external tree includes:
  - `/opt/embedded-tools/debuggers`
  - `/opt/embedded-tools/downloads`
  - `/opt/embedded-tools/scripts`
  - `/opt/embedded-tools/sdk`
  - `/opt/embedded-tools/toolchains`
- Confirmed ESP-IDF path:
  - `/opt/embedded-tools/sdk/esp/esp-idf`
- Confirmed debugger-related paths:
  - `/opt/embedded-tools/debuggers/picotool-build`
  - `/opt/embedded-tools/debuggers/picotool-src`

## Family Support Policy

### ESP32
ESP32 is the first fully targeted family.

Use:
- ESP-IDF at `/opt/embedded-tools/sdk/esp/esp-idf`
- `idf.py` as the primary build/flash/monitor interface

### RP2040
RP2040 is the second target family.

Use:
- Pico SDK if detected and confirmed
- `picotool` if available and confirmed
- CMake-based workflows

Do not pretend RP2040 build/flash is production-ready until the doctor scripts confirm the environment.

### STM32
STM32 is the third target family.

Support may begin as scaffold-only until the toolchain and programmer flow are verified.

Do not invent a fully working STM32 flow if it has not been validated.

---

## What To Do Before Writing Code

Before implementing any task, do the following:

1. Classify the task:
   - new project
   - modify project
   - build system work
   - flashing/programming work
   - board support work
   - documentation work
   - tooling diagnosis

2. Inspect the relevant files first.

3. Preserve the factory architecture.

4. If the task affects build or flash behavior, inspect:
   - `scripts/common/`
   - `scripts/env/`
   - `scripts/targets/`
   - `configs/tools/`
   - `configs/boards/`
   - relevant project folder

5. Prefer updating existing abstractions over bypassing them.

---


## Hard Repository Rules

1. Do not write project source under `/opt/embedded-tools`.
2. Do not place reusable repo scripts inside individual project folders if they belong in `scripts/`.
3. Do not place generated binaries inside source directories unless the project specifically requires a local build directory. Centralize outputs under `artifacts/` when appropriate.
4. Do not create duplicate helper scripts with overlapping purpose.
5. Do not hardcode temporary USB ports without detection or board profile backing.
6. Do not hardcode user-specific absolute paths in many places. Centralize them in shared path logic.
7. Do not use `sudo` unless the user explicitly requests it or it is absolutely required and clearly justified.
8. Do not add hidden side effects to utility scripts.
9. Do not silently modify unrelated files.
10. Do not fake tests.

---

## Factory Workflow Rules

### New Project Creation
When asked to create a new firmware project:

1. Select the correct family:
   - `esp32`
   - `rp2040`
   - `stm32`

2. Create the project under:
   - `projects/<family>/<project_name>/`

3. Base the project on the appropriate template:
   - `templates/esp32-idf/`
   - `templates/rp2040-cmake/`
   - `templates/stm32-cmake/`

4. Ensure each project contains, at minimum, a coherent structure. Use family-appropriate names, but aim for:

- `README.md`
- `src/` or family-equivalent source directory
- `include/`
- `boards/`
- `tests/`
- `out/`
- `build.sh`
- `flash.sh`
- optionally `monitor.sh` if relevant

5. Update documentation if the project introduces new board assumptions or flashing behavior.

### Build Workflow
Builds must flow through the factory architecture, not ad-hoc one-off commands.

Preferred path:
- `bin/factory`
- `scripts/env/`
- `scripts/targets/`

### Flash Workflow
Flashing must:
1. identify the target family
2. identify the project
3. identify the board profile when available
4. identify the port or programming method
5. state exactly which artifact will be flashed
6. log the action if the flash is successful

### Monitor Workflow
Serial monitoring should also use the family-aware dispatcher whenever practical.

---

## USB, Serial, and Board Detection Rules

USB and serial are critical and must be treated carefully.

1. Never assume `/dev/ttyUSB0` or `/dev/ttyACM0` is correct without checking.
2. Prefer detection logic and explicit reporting.
3. If multiple devices exist, report them clearly.
4. If board identity is ambiguous, do not claim certainty.
5. Keep reusable USB logic in:
   - `scripts/common/usb.sh`
   - `scripts/common/detect_board.sh`

6. Persist useful detection results under:
   - `state/known_devices/`

7. Persist successful flash history under:
   - `state/flash_history/`

---

## Script Writing Rules

All shell scripts must be production-grade enough for local automation.

### Mandatory shell style
Use:

```bash
#!/usr/bin/env bash
set -Eeuo pipefail
