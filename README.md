# PX4 Falconer

Mission planning and execution app for [PX4](https://px4.io), built as a focused, lean command and control app for pilots.

Falconer is PX4 only. It covers mission planning, manual flight with telemetry and video, and single vehicle, multi-vehicle and swarm operation. Vehicle setup, calibration, tuning, parameter editing, firmware flashing and log analysis are out of scope.

## Status

Early development. No usable release yet.

## Architecture

- **UI**: Svelte 5 in a Tauri 2 shell, on Windows, macOS, Linux, iOS and Android.
- **Backend**: C++ around [MAVSDK](https://github.com/mavlink/MAVSDK), linked as a library.
- **Interface**: an internal protobuf contract over an authenticated loopback WebSocket. It is not a public API.

## Development

The backend is developed and tested without the UI. `fctl` stands in for it: it starts the backend the way the Tauri shell will, authenticates over the contract, and sends commands.

Requirements (macOS, Homebrew): `cmake ninja mavsdk protobuf boost openssl@3`, plus Docker for PX4 SITL.

```sh
cmake -S . -B build -G Ninja
cmake --build build

./build/cli/fctl sitl up 2       # two PX4 SITL (SIH) vehicles in Docker
./build/cli/fctl top             # live dashboard: fleet table, vehicle detail, events, command line
./build/cli/fctl                 # plain interactive shell; type "help"
./build/cli/fctl run -c "wait 2; wait-until all armable; arm all; takeoff all 10; wait-alt all 9.5; land all"
./build/cli/fctl sitl down
```

`fctl run` stops at the first failed command and exits non-zero. Backend logs and SITL ULogs go to `.falconer/`. The SITL image is pinned in `sitl/image`.

For a sanitizer build, add `-DFALCONER_SANITIZER=address` (or `thread`) in a separate build directory.

## License

[MIT](LICENSE)
