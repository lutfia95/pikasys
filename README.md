# pikasys

**Live CPU and GPU monitoring in one C source file.**

pikasys combines system and process monitoring with GPU statistics in a keyboard-driven terminal interface. It targets Linux, Windows through WSL2, and macOS, with selectable themes, history graphs, and JSON export.

The application consists of `pikasys.c` and a `Makefile`. This README documents version **0.1.0**.

> **Development release:** Linux compilation and terminal behavior have been tested. NVIDIA integration has been tested with a simulated driver. Real NVIDIA, AMD, and Intel hardware, WSL, and macOS builds still need validation. Available metrics depend on the operating system, driver, hardware, and permissions; unavailable values appear as `N/A` or JSON `null`.

## Features

- Overall and per-core CPU usage, load averages, and available CPU sensor readings.
- RAM, swap, root filesystem usage, network rates, and disk I/O rates.
- Automatic GPU discovery with NVIDIA NVML, Linux DRM/sysfs, and macOS IOKit backends.
- GPU utilization, memory, temperatures, power, clocks, fans, and encoding/decoding activity where exposed by the backend.
- Process CPU and memory usage, GPU attribution where available, and process details.
- Case-insensitive process search by command, username, or PID.
- Sorting by CPU, RAM, GPU, PID, or name.
- CPU, memory, and GPU history graphs.
- Five built-in themes and custom color overrides.
- Adjustable sampling intervals, pause/resume, and explicit preference saving.
- Newline-delimited JSON output for scripts and logging.

pikasys is a read-only monitor. It does not terminate processes or change hardware settings. Pressing `w` writes the user preference file.

## Requirements

- A C11 compiler, such as GCC or Clang.
- GNU Make or a compatible `make` supporting this Makefile's conditionals.
- Operating-system development headers and libraries.
- An ANSI-compatible terminal for interactive mode.

No Python, ncurses, CUDA toolkit, or GPU SDK is required to build the app. Linux uses system libraries including `libdl` and `libm`; macOS uses IOKit and CoreFoundation frameworks. NVIDIA monitoring loads the installed driver's NVML library at runtime.

On macOS, the Command Line Tools can be installed with:

```bash
xcode-select --install
```

On Linux or inside WSL, install your distribution's C compiler, development headers, and Make if they are not already available. Build and run WSL installations inside the Linux distribution.

The terminal must be at least **48 columns × 18 rows**. A window around **120 × 36** displays more panels and process columns. Narrow windows show a more compact layout.

## Build and install

Extract the source archive and build:

```bash
unzip pikasys-source.zip
cd pikasys
make
```

Run directly from the source directory:

```bash
./pikasys
```

Install system-wide:

```bash
sudo make install
pikasys
```

The default destination is `/usr/local/bin/pikasys`. Once that directory is on your `PATH`, the command works from any directory. Normal monitoring does not require `sudo`.

### Install for your user

```bash
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
pikasys
```

To retain this PATH setting in new terminals, add the following line to your shell configuration, such as `~/.bashrc` for Bash or `~/.zshrc` for Zsh:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Check which executable your shell finds:

```bash
command -v pikasys
pikasys --version
```

### Build options

Choose a compiler or compiler flags:

```bash
make clean
make CC=clang CFLAGS='-O2 -g'
```

The Makefile accepts `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, `LDLIBS`, `PREFIX`, and `DESTDIR`. Clean before rebuilding with changed flags, since changing a Make variable alone does not force recompilation.

Stage an installation for packaging:

```bash
make install DESTDIR="$PWD/staging" PREFIX=/usr
```

This places the executable at `staging/usr/bin/pikasys`.

## Quick start

```bash
pikasys
pikasys --theme dracula
pikasys --interval 0.5 --sort ram
pikasys --filter python
pikasys --gpu-only
pikasys --ascii --no-color
```

Press `?` for help inside the app.

## Views and controls

| Key | Action |
| --- | --- |
| `0` | Overview: CPU, memory summary, GPU, and processes as space allows |
| `1` | CPU view and paginated logical CPU readings |
| `2` | GPU view and GPU-attributed processes |
| `3` | Process table |
| Up / Down or `k` / `j` | Select a process |
| Page Up / Page Down | Move through the process list |
| Home / End | Select the first or last process |
| `s` | Cycle the sort field |
| `r` | Reverse the sort order |
| `/` | Enter a process search |
| Enter | Apply a search, or toggle selected process details |
| `G` | Toggle GPU-only process filtering |
| `[` / `]` or Left / Right | Select the previous or next GPU |
| `<` / `>` | Select the previous or next CPU page |
| `t` | Cycle the theme and clear custom color overrides |
| `a` | Toggle ASCII rendering |
| `C` | Toggle colors |
| `w` | Save preferences |
| `+` or `=` | Sample faster |
| `-` | Sample more slowly |
| Space | Pause or resume sampling |
| `?` | Toggle help |
| Esc | Close an overlay and clear the search filter |
| `q` | Quit |

Keys are case-sensitive. While editing a search, ordinary characters become part of the query; use Enter or Esc to leave search editing.

The GPU view's process table includes processes attributed to **any detected GPU**. Switching the selected GPU changes the GPU panel, not that table's device scope.

## Command-line options

| Option | Description |
| --- | --- |
| `--theme NAME` | Select `pikachu`, `dracula`, `nord`, `ocean`, or `light` |
| `--interval SEC` | Sampling interval from `0.1` to `60` seconds; default `1` |
| `--sort FIELD` | Sort by `cpu`, `ram`, `gpu`, `pid`, or `name`; `memory` aliases `ram` |
| `--filter TEXT` | Filter process commands, usernames, or PIDs |
| `--gpu-only` | Show only GPU-attributed processes |
| `--no-gpu` | Skip GPU probing |
| `--ascii` | Use ASCII graphs and borders |
| `--no-color` | Disable colors |
| `--json` | Output one JSON object per sample |
| `--count N` | Exit after a positive number of samples, in either mode |
| `--once` | Output one JSON sample after a short baseline warmup |
| `--config PATH` | Use an alternate preference file |
| `--no-config` | Disable preference loading and saving |
| `--self-test` | Run built-in regression checks |
| `--version` | Print the version |
| `--help`, `-h` | Print usage information |

Use separate arguments, for example `--interval 0.5`, rather than `--interval=0.5`. The presence of the `NO_COLOR` environment variable disables colors at startup.

## Themes and configuration

The default theme is `pikachu`. Other built-in themes are `dracula`, `nord`, `ocean`, and `light`.

pikasys reads preferences from:

- `$XDG_CONFIG_HOME/pikasys/config` when `XDG_CONFIG_HOME` is an absolute path.
- Otherwise, `$HOME/.config/pikasys/config`.

Example preference file:

```ini
# pikasys preferences
theme=nord
interval=1
sort=CPU
ascii=false
color=true

# Optional six-digit RGB overrides
color.background=#12151c
color.foreground=#e8ecf3
color.muted=#8993a5
color.accent=#ffd449
color.good=#80d4a0
color.warning=#ffb35c
color.bad=#ff6b81
color.selection=#333a4d
```

Values are unquoted. Put comments on their own lines. Color overrides accept six hexadecimal digits, with or without the leading `#`.

Command-line settings override the file. Selecting `--theme NAME` or pressing `t` clears custom color overrides for the current session.

Press `w` to save the current theme, interval, sort field, ASCII setting, color setting, and custom colors. Search filters, the selected view, reverse sorting, and GPU-only filtering are not saved. Preferences are not saved automatically on exit.

Use a separate configuration or ignore saved settings:

```bash
pikasys --config ./my-pikasys.conf
pikasys --no-config
```

## JSON export

Export a single snapshot:

```bash
pikasys --once > snapshot.json
```

Record 60 samples:

```bash
pikasys --json --count 60 --interval 1 > metrics.jsonl
```

Record continuously until interrupted:

```bash
pikasys --json --interval 2 > metrics.jsonl
```

Filter exported processes:

```bash
pikasys --json --count 10 --filter python > python-metrics.jsonl
```

JSON mode does not require an interactive terminal. Output uses **JSON Lines**, with one complete object per line, rather than one array containing all samples.

Each object includes:

| Field | Contents |
| --- | --- |
| `schema`, `version` | Output schema and application version |
| `timestamp` | Unix time in seconds |
| `hostname`, `platform`, `wsl` | System identity and WSL detection |
| `sample_seconds`, `uptime_seconds` | Observed interval and system uptime |
| `cpu` | Overall usage, logical CPU usage, load, and available sensors |
| `memory` | RAM and swap usage in bytes |
| `io` | Network and disk rates in bytes/second, plus root filesystem usage |
| `gpus` | Detected GPU identities and available metrics |
| `processes` | Process records after filtering and sorting |
| `visible_processes`, `total_processes` | Filtered and collected process counts |
| `permission_denials`, `gpu_status` | Collection diagnostics |

Unavailable numeric readings are `null`. A process's `gpu_mask` is a hexadecimal string: bit `n` corresponds to GPU index `n` in the GPU records. This identifies devices attributed to the process; it is not a utilization value.

The first output follows a short counter warmup. Later samples include collection overhead, so use `sample_seconds` when interpreting timing rather than assuming perfectly spaced timestamps.

## Platform and GPU support

| Target | Implemented backend | Current limitations and validation |
| --- | --- | --- |
| Linux CPU/system | `/proc`, `/sys`, system calls | Build and live system monitoring tested; sensors depend on exposed files and permissions |
| Windows through WSL2 | Linux guest interfaces and available GPU driver interfaces | Reports guest-exposed information; WSL has not been tested for this release |
| NVIDIA on Linux/WSL | Dynamically loaded NVML | Simulated two-GPU tests passed; real hardware remains untested |
| AMD on Linux | DRM fdinfo and sysfs | Implemented; real hardware remains untested |
| Intel on Linux | DRM fdinfo, including Xe cycle counters, and sysfs | Implemented; device-wide GPU utilization may be unavailable even when process counters exist; real hardware remains untested |
| Other Linux DRM devices | Compatible DRM/sysfs fields | Best effort; not a guarantee of support for every GPU |
| macOS CPU/system | Mach, libproc, network interfaces, IOKit disk statistics | Implemented; macOS compilation and runtime remain untested |
| macOS GPU | IOKit registry statistics | Best effort, undocumented counter keys; no per-process GPU attribution; hardware remains untested |

This release does not build as a native Windows executable. Windows usage is through WSL2.

On macOS, CPU temperature and package power are not collected by the current backend. GPU statistics may be missing or change between operating-system versions. On all platforms, unavailable process attribution can leave the GPU-only table empty even if a GPU is detected.

## Interpreting the readings

**CPU usage:** Overall CPU and per-core usage are percentages from counter differences. Process CPU usage uses **100% per logical CPU**, so a multithreaded process can exceed 100%. Short sampling intervals can show counter quantization effects.

**RAM:** Linux reports total memory minus available memory. macOS reports active, wired, and compressed physical pages. These definitions differ, so values need not match another monitor that uses a different accounting method.

**GPU process usage:** NVIDIA readings use available NVML process samples. For DRM, the app sums the busiest normalized engine from each client assigned to a process. Process totals can exceed 100%, including when activity spans multiple devices. They are not a substitute for device-wide GPU utilization.

**GPU memory:** Shared GPU memory belongs to system memory and should not be interpreted as dedicated VRAM. Shared DRM clients are deduplicated and attributed to the first accessible PID. Resident buffers may still be shared between separate clients, so summing process memory is not necessarily equal to device memory usage.

**Network:** Totals exclude loopback but include virtual interfaces. The same traffic may appear at more than one virtual network layer.

**Disk I/O:** Linux sums whole backing devices while excluding loop, RAM, zram, device-mapper, and MD devices to reduce duplicate accounting across storage layers. Process disk I/O depends on access to the corresponding counters.

**History:** Graphs retain up to 240 samples in memory. At a one-second interval this is approximately four minutes; the displayed portion also depends on terminal width. History is not written to disk unless you explicitly export samples.

## Tests

Run the included checks:

```bash
make check
```

This runs built-in parser/calculation tests and a short JSON collection smoke test. On the tested Linux build, the built-in suite contains 40 checks. Platform-specific checks mean counts may differ on macOS.

Build and run AddressSanitizer and UndefinedBehaviorSanitizer checks with a compiler that supports them:

```bash
make sanitize
```

If LeakSanitizer specifically fails because the execution environment does not support it, run the address and undefined-behavior checks with leak detection disabled:

```bash
ASAN_OPTIONS=detect_leaks=0 make sanitize
```

Version 0.1.0 was also exercised with a simulated NVML driver, terminal keyboard/resize tests, configuration saving, install/uninstall checks, signal cleanup, and an active CPU workload. Those additional development harnesses are not included in the two-file source distribution. These tests do not establish real GPU or macOS compatibility.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| `make` or the compiler is missing | Install your operating system's development tools |
| `pikasys: command not found` | Check the install prefix and `PATH`, or run `./pikasys` from the source directory |
| Interactive mode requires a terminal | Run in a terminal, or use `--json` / `--once` for redirected output |
| `TERM=dumb` | Use a terminal with ANSI support, or export JSON |
| Broken borders or graph characters | Use `--ascii`; ensure the terminal supports UTF-8 for Unicode rendering |
| Colors are missing or incorrect | Check `NO_COLOR`; try `--no-color`, or press `C` to toggle colors |
| Panels or columns are missing | Enlarge the terminal; use `0`–`3` to switch views |
| No GPU is detected | Check the installed driver and device visibility; under WSL, check guest GPU exposure |
| NVIDIA monitoring is unavailable | The installed driver's NVML library must be accessible; the app also checks `/usr/lib/wsl/lib/libnvidia-ml.so.1` |
| A value is `N/A` or `null` | The counter may be unsupported, inaccessible, or awaiting a usable sample |
| GPU-only process list is empty | Process attribution may be unavailable or restricted; macOS GPU attribution is not implemented |
| Saved settings seem unexpected | Try `--no-config`, or inspect the preference file |

To isolate a GPU backend issue, run:

```bash
pikasys --no-config --no-gpu
```

For a bug report, include the pikasys version, OS, compiler, build output, GPU model and driver where relevant, and steps to reproduce. JSON snapshots contain hostnames, usernames, and process commands; inspect them before sharing.

## Uninstall and clean

Remove a default system-wide installation:

```bash
sudo make uninstall
```

Remove a user installation:

```bash
make uninstall PREFIX="$HOME/.local"
```

Use the same prefix that you used when installing. Uninstall removes the executable and leaves preferences in place.

Remove local build outputs:

```bash
make clean
```

## License

pikasys is distributed under the **MIT License**. The complete license text is included at the end of `pikasys.c`.
