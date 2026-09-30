# PhotoViewer

Personal image inspection tool for image-processing work: raylib (OpenGL) + Dear ImGui (docking) + OpenCV.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/PhotoViewer [files or folders...]
```

Requires OpenCV (`brew install opencv`). raylib, ImGui and rlImGui are fetched by CMake.

## Install as default image viewer (macOS)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
sudo cmake --install build          # -> /Applications/PhotoViewer.app
scripts/macos-set-default.sh        # as your user, not sudo; add --dry-run to preview
```

Double-clicking an image (or Open With) then opens it in PhotoViewer; opening more files while it runs adds them to the list.
The app links Homebrew's OpenCV, so rebuild + reinstall after an OpenCV major upgrade.
HEIC and camera RAW stay with Preview (OpenCV usually can't decode them).

## Features

- Drag & drop multiple files or folders; dropping an already open file reloads it.
- OpenCV `IMREAD_UNCHANGED` decoding: 8/16-bit, signed, 32F/64F (TIFF, EXR, PFM...), alpha. Raw values are shown, EXIF orientation is **not** applied.
- Loupe tooltip (hold Shift) with pixel grid, values, hex, HSV.
- Pixel values printed inside cells at high zoom (like `cv::imshow`), pixel grid.
- Histogram (per channel, log scale, hovered-pixel markers), stats; ROI stats and histogram.
- Metadata: file info, image type/memory, EXIF (camera, exposure, GPS) from JPEG/TIFF/DNG/PNG/WebP.
- Auto-reload when files change on disk, so it can stay open next to a pipeline that writes outputs.
- Shared view when switching images, for flicker comparison of processing stages.

## Layout

Top menu bar (File / View / Image / Help) holds all settings and actions; current image, size, type and zoom are shown on its right.
The right sidebar (~10% width) has three tabs: **Info** (pixel readout, histogram, stats — right-click histogram for log scale), **Meta**, **Files** (right-click an entry for reload / reveal / close).

## Controls

| Input | Action |
|---|---|
| Two-finger scroll | pan |
| Pinch, ⌘/Ctrl + scroll | zoom at cursor (untick "Scroll pans" for plain mouse-wheel zoom) |
| Hold Shift | loupe |
| Left / middle drag | pan |
| Double-click, `F`, `0` | fit |
| `1` `2` `3` `4` | 100 / 200 / 400 / 800 % (physical pixels) |
| `+` / `-` | zoom |
| Click | pin pixel (stays across images) |
| Right-drag or Option-drag | ROI; right-click or `Esc` clears |
| `←` `→` / Space / Backspace | previous / next image |
| `N` | normalize min..max |
| `C` | cycle channel All/R/G/B/A |
| `G` `V` | toggle grid / cell values |
| `R` | reload, `Delete` closes |
| `F1` | ImGui demo |

Layout is saved in `~/.photoviewer.ini` (or View → Reset layout).
