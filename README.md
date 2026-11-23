# Vivitar Capture

A native Windows app for USB capture cards (built for a Vivitar card), styled after Elgato Studio.

- Live low-latency preview (Media Foundation capture, Direct3D 11 rendering)
- Picks the best capture format the card offers (uncompressed when available at your frame rate)
- GPU picture enhancement: noise/block reduction, Lanczos/bicubic upscaling, contrast-adaptive sharpening, colour controls, before/after compare
- Recording to MP4 (hardware H.264 + AAC), optionally enhanced and upscaled to 1440p
- PNG screenshots, audio monitoring with level meters, library of captures

## Download

Grab `VivitarCapture.exe` from the [latest release](../../releases/tag/latest). It's a single file with no installer.

## Shortcuts

| Key | Action |
| --- | --- |
| R | Start / stop recording |
| S | Screenshot |
| F / F11 | Fullscreen preview (Esc to exit) |
| M | Mute audio monitor |
| E | Enhancement on / off |
| C | Compare before / after |

Settings and a log file live in `%APPDATA%\VivitarCapture`.

## Building

Every push to `main` is built by GitHub Actions. To build locally: Visual Studio 2022 (Desktop C++), then
`cmake -S . -B build -A x64 && cmake --build build --config Release`.
