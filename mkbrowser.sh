#!/bin/sh
set -eu

APP_DIR=${APP_DIR:-$HOME/Applications}
TERMINAL=Ghostty
ACTIVATE=yes

usage() {
    cat >&2 <<EOF
usage: mkbrowser.sh [--terminal NAME] [--no-activate] [--to DIR]
  --terminal NAME  the terminal app a link opens a new window in when no
                   window is running, and the one brought forward when one
                   is (default Ghostty)
  --no-activate    leave the terminal where it is when a running window
                   takes the link
  --to DIR         where the bundle goes (default ~/Applications)
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
        --terminal) [ $# -ge 2 ] || usage; TERMINAL=$2; shift 2 ;;
        --no-activate) ACTIVATE=no; shift ;;
        --to) [ $# -ge 2 ] || usage; APP_DIR=$2; shift 2 ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

# $1 as one shell word
quote() { printf "'"; printf %s "$1" | sed "s/'/'\\\\''/g"; printf "'"; }

WEB=$(command -v web 2>/dev/null || true)
[ -n "$WEB" ] || WEB=$HOME/.local/bin/web
if [ ! -x "$WEB" ]; then
    echo "mkbrowser: no web on PATH and none at $WEB; run make install first" >&2
    exit 1
fi

[ -d "/Applications/$TERMINAL.app" ] || [ -d "$HOME/Applications/$TERMINAL.app" ] ||
    echo "mkbrowser: warning: no $TERMINAL.app in /Applications" >&2

APP=$APP_DIR/Web.app
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat >"$WORK/handler.applescript" <<'EOF'
on open location this_url
	hand_over(this_url)
end open location

on open these_items
	repeat with f in these_items
		hand_over("file://" & (POSIX path of (f as alias)))
	end repeat
end open

on run
	hand_over("")
end run

on hand_over(u)
	set s to quoted form of (POSIX path of (path to resource "open-url"))
	do shell script s & " " & quoted form of u
end hand_over
EOF

mkdir -p "$APP_DIR"
rm -rf "$APP"
osacompile -o "$APP" "$WORK/handler.applescript"

TMUX_BIN=$(command -v tmux 2>/dev/null || true)

OPEN_URL=$APP/Contents/Resources/open-url
{
    echo "#!/bin/sh"
    echo "WEB=$(quote "$WEB")"
    echo "TERMINAL=$(quote "$TERMINAL")"
    echo "ACTIVATE=$(quote "$ACTIVATE")"
    echo "TMUX_BIN=$(quote "$TMUX_BIN")"
    cat <<'BODY'

# $1 as one shell word
sq() { printf "'"; printf %s "$1" | sed "s/'/'\\\\''/g"; printf "'"; }

is_shell() {
    case $1 in
    bash|zsh|fish|sh|dash|ksh|mksh|tcsh|csh|nu|elvish|xonsh) return 0 ;;
    *) return 1 ;;
    esac
}

# the tmux window on screen, as session:index, or nothing
tmux_window() {
    [ -n "$TMUX_BIN" ] && [ -x "$TMUX_BIN" ] || return 1
    tty=$("$TMUX_BIN" list-clients -F '#{client_activity} #{client_tty}' 2>/dev/null |
          sort -rn | head -1 | cut -d' ' -f2)
    [ -n "$tty" ] || return 1
    "$TMUX_BIN" display-message -p -t "$tty" '#{session_name}:#{window_index}' 2>/dev/null
}

# a pane of window $1 with nothing running in it, active one first
idle_pane() {
    "$TMUX_BIN" list-panes -t "$1" -F '#{pane_active} #{pane_id} #{pane_current_command}' \
        2>/dev/null | sort -rn | while read -r act id cmd; do
        if is_shell "$cmd"; then printf %s "$id"; break; fi
    done
}

# run the command line $1 in the terminal already on screen
run_on_screen() {
    win=$(tmux_window) || return 1
    [ -n "$win" ] || return 1
    "$TMUX_BIN" select-window -t "$win" 2>/dev/null || return 1
    pane=$(idle_pane "$win")
    if [ -n "$pane" ]; then
        "$TMUX_BIN" select-pane -t "$pane" 2>/dev/null
        "$TMUX_BIN" send-keys -t "$pane" C-u "$1" Enter 2>/dev/null || return 1
    else
        "$TMUX_BIN" split-window -t "$win" "$1" 2>/dev/null ||
            "$TMUX_BIN" new-window -t "${win%%:*}" "$1" 2>/dev/null || return 1
    fi
    return 0
}

new_tab() {
    [ "$TERMINAL" = Ghostty ] || return 1
    /usr/bin/osascript - "$1" >/dev/null 2>&1 <<'AS'
on run argv
	tell application "Ghostty"
		activate
		new tab with configuration {command:(item 1 of argv)}
	end tell
end run
AS
}

new_window() {
    if [ "$TERMINAL" = Ghostty ]; then
        /usr/bin/osascript - "$1" >/dev/null 2>&1 <<'AS' && return 0
on run argv
	tell application "Ghostty"
		activate
		new window with configuration {command:(item 1 of argv)}
	end tell
end run
AS
    fi
    /usr/bin/open -na "$TERMINAL" --args -e /bin/sh -c "$1"
}

url=$1

if [ -n "$url" ] && "$WEB" --open "$url" 2>/dev/null; then
    [ "$ACTIVATE" = yes ] && /usr/bin/open -a "$TERMINAL"
    exit 0
fi

cmd=$(sq "$WEB")
[ -n "$url" ] && cmd="$cmd $(sq "$url")"

if run_on_screen "$cmd"; then
    /usr/bin/open -a "$TERMINAL"
    exit 0
fi
new_tab "$cmd" && exit 0
new_window "$cmd"
BODY
} >"$OPEN_URL"
chmod 755 "$OPEN_URL"

PLIST=$APP/Contents/Info.plist
PB=/usr/libexec/PlistBuddy
set_key() { $PB -c "Delete :$1" "$PLIST" >/dev/null 2>&1 || true; $PB -c "Add :$1 $2" "$PLIST" >/dev/null; }

set_key CFBundleName "string web"
set_key CFBundleDisplayName "string web"
set_key CFBundleIdentifier "string com.jhickner.web.browser"
set_key CFBundleShortVersionString "string 1.0"
$PB -c "Delete :LSUIElement" "$PLIST" >/dev/null 2>&1 || true

$PB -c "Delete :CFBundleDocumentTypes" "$PLIST" >/dev/null 2>&1 || true
$PB -c "Add :CFBundleDocumentTypes array" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0 dict" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeName string HTML document" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Viewer" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0:LSHandlerRank string Alternate" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes array" "$PLIST" >/dev/null
$PB -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:0 string public.html" "$PLIST" >/dev/null

$PB -c "Delete :CFBundleURLTypes" "$PLIST" >/dev/null 2>&1 || true
$PB -c "Add :CFBundleURLTypes array" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0 dict" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0:CFBundleURLName string Web page" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0:CFBundleTypeRole string Viewer" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes array" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes:0 string http" "$PLIST" >/dev/null
$PB -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes:1 string https" "$PLIST" >/dev/null

codesign --force --sign - "$APP" >/dev/null 2>&1 || true

LSREG=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "$LSREG" ]; then "$LSREG" -f "$APP" || true; fi

echo "built $APP"
echo "web binary: $WEB"
echo "terminal:   $TERMINAL"
echo
echo "To make it the default:"
echo "  System Settings > Desktop & Dock > Default web browser > web"
echo "  then confirm the prompt. Only the user can change the browser;"
echo "  duti and anything else asking for it gets -54."
