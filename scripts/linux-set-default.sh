#!/bin/sh
# Make PhotoViewer the default app for common image types (XDG), or undo it (per-user setting).
# Run as yourself, NOT with sudo (associations live in your user's mimeapps.list):
#   scripts/linux-set-default.sh             # set PhotoViewer as default (saves previous defaults)
#   scripts/linux-set-default.sh --dry-run   # show what would change
#   scripts/linux-set-default.sh --restore   # put the previous defaults back (skips types with none saved)
# Optional first/any non-flag argument: desktop file id (default photoviewer.desktop).
set -eu

DESKTOP=photoviewer.desktop
MODE=set
for arg in "$@"; do
    case "$arg" in
        --dry-run) MODE=dry ;;
        --restore) MODE=restore ;;
        -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
        *) DESKTOP="$arg" ;;
    esac
done

if [ "$(id -u)" = "0" ]; then
    echo "Run this as your normal user (default apps are per-user), not with sudo." >&2
    exit 1
fi
command -v xdg-mime >/dev/null 2>&1 || { echo "xdg-mime not found (install package xdg-utils)." >&2; exit 1; }
if [ "$MODE" != restore ] \
   && [ ! -f "/usr/share/applications/$DESKTOP" ] \
   && [ ! -f "/usr/local/share/applications/$DESKTOP" ] \
   && [ ! -f "$HOME/.local/share/applications/$DESKTOP" ]; then
    echo "Desktop file not found: $DESKTOP (install it first: sudo cmake --install build)" >&2
    exit 1
fi

SAVED="$HOME/.local/share/photoviewer/previous-defaults.tsv"
mkdir -p "$(dirname "$SAVED")"
TAB="$(printf '\t')"

# HEIC/RAW have no widely-registered MIME type here and OpenCV usually can't decode them anyway.
TYPES="image/jpeg image/png image/tiff image/bmp image/webp image/gif image/x-exr image/x-portable-bitmap image/jp2 image/avif"

# Previous defaults: "mime<TAB>desktop-file-id" per line.
saved_for() {
    [ -f "$SAVED" ] && awk -F"$TAB" -v k="$1" '$1==k{print $2; f=1} END{exit !f}' "$SAVED"
}

case "$MODE" in
    dry)
        for t in $TYPES; do
            cur=$(xdg-mime query default "$t" 2>/dev/null || true)
            echo "would set $t  (currently ${cur:-none})"
        done
        ;;
    restore)
        for t in $TYPES; do
            if prev=$(saved_for "$t"); then
                xdg-mime default "$prev" "$t"
                echo "ok    $t -> $prev"
            else
                echo "skip  $t (no previous default known)"
            fi
        done
        rm -f "$SAVED"
        ;;
    set)
        NEW="$SAVED.new"
        : > "$NEW"
        [ -f "$SAVED" ] && cat "$SAVED" >> "$NEW"
        for t in $TYPES; do
            cur=$(xdg-mime query default "$t" 2>/dev/null || true)
            # Remember what was there before, unless it's already us (re-runs keep the original).
            if [ -n "$cur" ] && [ "$cur" != "$DESKTOP" ] && ! saved_for "$t" >/dev/null; then
                printf '%s\t%s\n' "$t" "$cur" >> "$NEW"
            fi
            xdg-mime default "$DESKTOP" "$t"
            echo "ok    $t -> $DESKTOP"
        done
        sort -u -o "$NEW" "$NEW"
        mv "$NEW" "$SAVED"
        echo "previous defaults saved to $SAVED"
        ;;
esac
