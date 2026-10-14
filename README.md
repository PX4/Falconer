# PX4 Falconer

Mission planning and execution app for [PX4](https://px4.io), built as a focused, lean command and control app for pilots.

Falconer is PX4 only. It covers mission planning, manual flight with telemetry and video, and single vehicle, multi-vehicle and swarm operation. Vehicle setup, calibration, tuning, parameter editing, firmware flashing and log analysis are out of scope.

## Status

Early development. No usable release yet.

## Architecture

- **UI**: Svelte 5 in a Tauri 2 shell, on Windows, macOS, Linux, iOS and Android.
- **Backend**: C++ around [MAVSDK](https://github.com/mavlink/MAVSDK), linked as a library.
- **Interface**: an internal protobuf contract over an authenticated loopback WebSocket. It is not a public API.

## License

[MIT](LICENSE)
