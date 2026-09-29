# Magnetometer Visualized

A visualizer for magnetometer data and Earth's magnetic reference field, built with raylib.

⚠️ **Work in Progress** - This project is under active development. Expect incomplete features and breaking changes.

## About

This project aims to visualize magnetometer readings alongside Earth's reference magnetic field for a given area. It's built on top of a raylib quickstart template, with raylib vendored directly into the repository.

Live data arrives over a serial COM port from an STM32 running the magnetometer firmware. The COM port reading is handled by a vendored copy of [com-port-api](https://github.com/jamesxbunker-cpu/com-port-api), a small C library that wraps the Windows serial API with overlapped I/O and event-driven waiting.

## Status

- [x] raylib cloned and running (engine foundation)
- [x] Earth's reference field added for area
- [x] File-based playback of recorded magnetometer data
- [x] Live COM port streaming via vendored `com-port-api`
- [x] Line-buffered reader (handles partial lines, CRLF, bursts)
- [ ] Slow-client / disconnect handling
- [ ] Configurable line format
- [ ] Documentation for building and running

## Project Structure

```text
magnetometer-visualized
├── raylib-quickstart/
│   ├── src/
│   │   ├── main.c                    # Live visualizer (COM port input)
│   │   └── third_party/
│   │       └── comport/              # Vendored com-port-api
│   │           ├── README.md         # Source commit hash
│   │           ├── comport.h
│   │           ├── comport.c
│   │           ├── serial.h
│   │           └── serial.c
│   ├── resources/
│   │   └── magnetometer_data.txt     # Recorded data for offline mode
│   ├── Makefile
│   ├── raylib-quickstart.make        # Premake-generated (edited to add comport)
│   └── raylib.make                   # Premake-generated
├── .gitignore
└── README.md
```

## Dependencies

- **raylib** - vendored in `raylib-quickstart/raylib/`
- **com-port-api** - vendored in `raylib-quickstart/src/third_party/comport/`

The vendored `com-port-api` copy is pinned to a specific commit. See `third_party/comport/README.md` for the exact hash and update instructions.

**Do not edit files under `third_party/comport/` directly.** Changes go into the `com-port-api` repo first, then get copied over.

## Getting Started

Setup instructions will be added as the project stabilizes.
At a high level, you'll need:

- [x] A C/C++ toolchain (MinGW-w64)
- [x] raylib (included in the repo)
- [x] com-port-api (vendored in `src/third_party/comport/`)

### Build

```bash
cd raylib-quickstart
make
```

This produces `bin/Debug/raylib-quickstart.exe`.

### Run - Live COM mode

Connect the STM32 and confirm the COM port in Device Manager. Then:

```bash
./bin/Debug/raylib-quickstart.exe COM5 115200
```

Replace `COM5` with the actual port number and `115200` with the baud rate the STM32 is using. The defaults (if no arguments are given) are `COM3` and `115200`.

The window opens and the trail fills in as samples arrive. The HUD shows the current vector, |B|, running mean |B|, temperature, and stream statistics (bytes received, samples parsed, parse failures).

### Run - Offline mode

The file-based version still works if you want to demo without hardware:

```bash
./bin/Debug/raylib-quickstart.exe
```

No, this runs live mode with defaults. For offline, build and run `main_offline.c` explicitly.

*(Note: the offline target needs to be wired into the Makefile as a separate executable. Until then, offline data must be fed through the live path or the old binary.)*

## Data Format

The visualizer expects one sample per line, whitespace-separated:

```
rawX rawY rawZ gaussX gaussY gaussZ tempC
```

Only the last four columns are used for rendering. Column 1-3 are kept for lossless round-tripping. Both CRLF and LF line endings are accepted.

Any line that does not parse as 7 numbers is silently skipped and counted in the HUD's parse-failure counter.

## Roadmap

- [ ] Flesh out the magnetometer data pipeline (configurable format, units)
- [ ] Add offline mode as a proper separate target in the Makefile
- [ ] Persist the vendored library change in `premake5.lua` so it survives regeneration
- [ ] Improve visualization of the reference field
- [ ] Add documentation for building and running
- [ ] Handle STM32 disconnects gracefully (reconnect)
- [ ] Add screenshots / demo

## Snapshot

<img width="1099" height="767" alt="image" src="https://github.com/user-attachments/assets/48900f2e-066f-4adc-bfc8-26e94138d2f3" />

## Contributing

This is an early-stage personal project. If you'd like to contribute or have ideas, feel free to open an issue.

## License

TBD