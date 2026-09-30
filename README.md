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
scripts/macos-set-default.sh        # as your user, not sudo; --dry-run previews, --restore undoes
```

Double-clicking an image (or Open With) then opens it in PhotoViewer; opening more files while it runs adds them to the list.

### Going back to Preview

```sh
scripts/macos-set-default.sh --restore
```

This restores whatever each type opened with before (saved in `~/Library/Application Support/PhotoViewer/previous-defaults.tsv`), or Preview if nothing was saved.

Manually, per file type: in Finder select an image → **Get Info** (⌘I) → **Open with:** choose **Preview** → **Change All…**. Repeat for each extension (JPEG, PNG, TIFF, …).

To remove the app completely: restore defaults first, then `sudo rm -rf /Applications/PhotoViewer.app`.
The app links Homebrew's OpenCV, so rebuild + reinstall after an OpenCV major upgrade.
HEIC and camera RAW stay with Preview (OpenCV usually can't decode them).

## Install as default image viewer (Ubuntu / Linux)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
sudo cmake --install build              # -> /usr/local/bin/PhotoViewer + .desktop entry + icon
scripts/linux-set-default.sh            # as your user, not sudo; --dry-run previews, --restore undoes
```

This registers `photoviewer.desktop` via `xdg-mime` for common image MIME types (JPEG, PNG, TIFF, BMP,
WebP, GIF, OpenEXR, PBM, JPEG 2000, AVIF), so double-click / "Open With" in your file manager opens
PhotoViewer; opening more files while it runs adds them to the list. Requires `xdg-utils` (usually
already installed).

### Going back to the previous default

```sh
scripts/linux-set-default.sh --restore
```

This restores whatever each MIME type opened with before (saved in
`~/.local/share/photoviewer/previous-defaults.tsv`); types with no saved default are left alone.

Manually, per file type: right-click an image in your file manager → **Open With** / **Properties** →
pick the app → **Set as default**.

To remove the app completely: restore defaults first, then delete `/usr/local/bin/PhotoViewer`,
`/usr/local/share/applications/photoviewer.desktop` and `/usr/local/share/pixmaps/photoviewer.png`
(or wherever `CMAKE_INSTALL_PREFIX` pointed).

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
| Scroll (Linux/Windows) | zoom at cursor; Ctrl+scroll pans |
| Two-finger scroll (macOS) | pan; Pinch / ⌘+scroll zooms |
| View → Scroll pans (trackpad) | flips which of the two above scroll does |
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
