#!/bin/sh
# Make PhotoViewer the default app for common image types (per-user setting).
# Run as yourself, NOT with sudo:   scripts/macos-set-default.sh [/Applications/PhotoViewer.app] [--dry-run]
set -eu

APP="/Applications/PhotoViewer.app"
DRY=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY=1 ;;
        *) APP="$arg" ;;
    esac
done

if [ "$(id -u)" = "0" ]; then
    echo "Run this as your normal user (default apps are per-user), not with sudo." >&2
    exit 1
fi
if [ ! -d "$APP" ]; then
    echo "App not found: $APP (install it first: sudo cmake --install build)" >&2
    exit 1
fi

# Register the bundle so LaunchServices knows its document types.
[ "$DRY" = 1 ] || /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"

SWIFT_SRC="$(mktemp -t pv_default).swift"
trap 'rm -f "$SWIFT_SRC"' EXIT
cat > "$SWIFT_SRC" <<'EOF'
import AppKit
import UniformTypeIdentifiers

let args = CommandLine.arguments
let app = URL(fileURLWithPath: args[1])
let dryRun = args.count > 2 && args[2] == "1"
// HEIC/RAW are left alone: OpenCV usually can't decode them, so Preview stays their default.
let types = ["public.jpeg", "public.png", "public.tiff", "com.microsoft.bmp", "org.webmproject.webp",
             "com.compuserve.gif", "com.ilm.openexr-image", "public.radiance", "public.pbm",
             "public.jpeg-2000", "public.avif"]

var pending = 0
var failed = false
for id in types {
    guard let t = UTType(id) else { print("skip  \(id) (unknown on this macOS)"); continue }
    if dryRun {
        let cur = NSWorkspace.shared.urlForApplication(toOpen: t)?.lastPathComponent ?? "-"
        print("would set \(id)  (currently \(cur))")
        continue
    }
    pending += 1
    NSWorkspace.shared.setDefaultApplication(at: app, toOpen: t) { err in
        DispatchQueue.main.async {
            if let err { print("FAIL  \(id): \(err.localizedDescription)"); failed = true }
            else { print("ok    \(id)") }
            pending -= 1
        }
    }
}
while pending > 0 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
exit(failed ? 1 : 0)
EOF
swift "$SWIFT_SRC" "$APP" "$DRY"
