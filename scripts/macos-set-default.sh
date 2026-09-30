#!/bin/sh
# Make PhotoViewer the default app for common image types, or undo it (per-user setting).
# Run as yourself, NOT with sudo:
#   scripts/macos-set-default.sh             # set PhotoViewer as default (saves previous defaults)
#   scripts/macos-set-default.sh --dry-run   # show what would change
#   scripts/macos-set-default.sh --restore   # put the previous defaults back (Preview if unknown)
# Optional first/any non-flag argument: path to the app (default /Applications/PhotoViewer.app).
set -eu

APP="/Applications/PhotoViewer.app"
MODE=set
for arg in "$@"; do
    case "$arg" in
        --dry-run) MODE=dry ;;
        --restore) MODE=restore ;;
        -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
        *) APP="$arg" ;;
    esac
done

if [ "$(id -u)" = "0" ]; then
    echo "Run this as your normal user (default apps are per-user), not with sudo." >&2
    exit 1
fi
if [ "$MODE" != restore ] && [ ! -d "$APP" ]; then
    echo "App not found: $APP (install it first: sudo cmake --install build)" >&2
    exit 1
fi

SAVED="$HOME/Library/Application Support/PhotoViewer/previous-defaults.tsv"
mkdir -p "$(dirname "$SAVED")"

# Register the bundle so LaunchServices knows its document types.
if [ "$MODE" = set ]; then
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"
fi

SWIFT_SRC="$(mktemp -t pv_default).swift"
trap 'rm -f "$SWIFT_SRC"' EXIT
cat > "$SWIFT_SRC" <<'EOF'
import AppKit
import UniformTypeIdentifiers

let args = CommandLine.arguments
let mode = args[1], app = URL(fileURLWithPath: args[2]), savedPath = args[3]
let preview = URL(fileURLWithPath: "/System/Applications/Preview.app")
// HEIC/RAW are left alone: OpenCV usually can't decode them, so Preview stays their default.
let types = ["public.jpeg", "public.png", "public.tiff", "com.microsoft.bmp", "org.webmproject.webp",
             "com.compuserve.gif", "com.ilm.openexr-image", "public.radiance", "public.pbm",
             "public.jpeg-2000", "public.avif"]

// Previous defaults: "uti<TAB>/path/to/App.app" per line.
var saved: [String: String] = [:]
if let text = try? String(contentsOfFile: savedPath, encoding: .utf8) {
    for line in text.split(separator: "\n") {
        let parts = line.split(separator: "\t", maxSplits: 1).map(String.init)
        if parts.count == 2 { saved[parts[0]] = parts[1] }
    }
}

var pending = 0, failed = false
func setDefault(_ target: URL, _ t: UTType, _ id: String) {
    pending += 1
    NSWorkspace.shared.setDefaultApplication(at: target, toOpen: t) { err in
        DispatchQueue.main.async {
            if let err { print("FAIL  \(id): \(err.localizedDescription)"); failed = true }
            else { print("ok    \(id) -> \(target.lastPathComponent)") }
            pending -= 1
        }
    }
}

var toSave = saved
for id in types {
    guard let t = UTType(id) else { print("skip  \(id) (unknown on this macOS)"); continue }
    let current = NSWorkspace.shared.urlForApplication(toOpen: t)
    switch mode {
    case "dry":
        print("would set \(id)  (currently \(current?.lastPathComponent ?? "-"))")
    case "restore":
        var target = preview
        if let p = saved[id], FileManager.default.fileExists(atPath: p) { target = URL(fileURLWithPath: p) }
        setDefault(target, t, id)
    default:
        // Remember what was there before, unless it's already us (re-runs keep the original).
        if let c = current, c.lastPathComponent != app.lastPathComponent, saved[id] == nil { toSave[id] = c.path }
        setDefault(app, t, id)
    }
}
while pending > 0 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }

if mode == "set" {
    let text = toSave.sorted { $0.key < $1.key }.map { "\($0.key)\t\($0.value)" }.joined(separator: "\n") + "\n"
    try? text.write(toFile: savedPath, atomically: true, encoding: .utf8)
    print("previous defaults saved to \(savedPath)")
}
if mode == "restore" && !failed { try? FileManager.default.removeItem(atPath: savedPath) }
exit(failed ? 1 : 0)
EOF
swift "$SWIFT_SRC" "$MODE" "$APP" "$SAVED"
