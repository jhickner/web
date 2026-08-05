#!/bin/sh
# Build Web.app: an http/https handler that hands the link to `web`.
#
# macOS will only offer an app bundle as the default browser, and it delivers
# the url as an Apple Event rather than on a command line, so the bundle is an
# AppleScript applet with an `open location` handler. All it does is run the
# shell script in its Resources, which hands the url to a running window and
# falls back to opening a terminal.
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

# $1 as one shell word, for the values baked into the generated script.
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

# The url goes to a window that is already up when there is one; `web --open`
# fails when there is not, which is the signal to start a terminal instead.
#
# A window in the terminal already running, not another copy of the
# application: `open -n` starts a second instance, with its own icon in the
# Dock and its own everything, and a link is not a reason for one. Ghostty is
# scriptable, so it is asked; anything else, and anything that refuses, gets
# `open -n` after all.
TMUX_BIN=$(command -v tmux 2>/dev/null || true)

OPEN_URL=$APP/Contents/Resources/open-url
{
    echo "#!/bin/sh"
    echo "WEB=$(quote "$WEB")"
    echo "TERMINAL=$(quote "$TERMINAL")"
    echo "ACTIVATE=$(quote "$ACTIVATE")"
    echo "TMUX_BIN=$(quote "$TMUX_BIN")"
    cat <<'BODY'

# $1 as one shell word, so an address carrying & or ? or a quote survives
# being written into a command line.
sq() { printf "'"; printf %s "$1" | sed "s/'/'\\\\''/g"; printf "'"; }

# A pane running one of these is a pane running nothing.
is_shell() {
    case $1 in
    bash|zsh|fish|sh|dash|ksh|mksh|tcsh|csh|nu|elvish|xonsh) return 0 ;;
    *) return 1 ;;
    esac
}

# The tmux window on screen, as session:index, or nothing. Taken from the client
# that was active last: two clients showing different windows cannot both be the
# one being looked at, and from out here nothing else distinguishes them.
tmux_window() {
    [ -n "$TMUX_BIN" ] && [ -x "$TMUX_BIN" ] || return 1
    tty=$("$TMUX_BIN" list-clients -F '#{client_activity} #{client_tty}' 2>/dev/null |
          sort -rn | head -1 | cut -d' ' -f2)
    [ -n "$tty" ] || return 1
    "$TMUX_BIN" display-message -p -t "$tty" '#{session_name}:#{window_index}' 2>/dev/null
}

# A pane of window $1 with nothing running in it. The active one for preference:
# it is the one the cursor is already in, so the page arrives where the eye is.
idle_pane() {
    "$TMUX_BIN" list-panes -t "$1" -F '#{pane_active} #{pane_id} #{pane_current_command}' \
        2>/dev/null | sort -rn | while read -r act id cmd; do
        if is_shell "$cmd"; then printf %s "$id"; break; fi
    done
}

# The command line $1, run in the terminal already on screen. A free pane of the
# tmux window being looked at is the best of these: no new window anywhere, and
# the page lands in the session it was asked for from. Failing that a tmux window
# of its own, which is still that terminal rather than another one.
run_on_screen() {
    win=$(tmux_window) || return 1
    [ -n "$win" ] || return 1
    pane=$(idle_pane "$win")
    if [ -n "$pane" ]; then
        "$TMUX_BIN" select-window -t "$win" 2>/dev/null || return 1
        "$TMUX_BIN" select-pane -t "$pane" 2>/dev/null
        # C-u first: the pane is at a prompt, and a line half typed there would
        # otherwise have the address appended to it and run as one command.
        "$TMUX_BIN" send-keys -t "$pane" C-u "$1" Enter 2>/dev/null || return 1
    else
        "$TMUX_BIN" new-window -t "${win%%:*}" "$1" 2>/dev/null || return 1
    fi
    return 0
}

# A tab of the window already open, for a terminal that is not showing tmux.
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

# The last resort: a window of its own. Ghostty is scriptable and makes it in
# the instance already running; anything else, and anything that refuses, gets a
# second instance of the application because that is the only way left to ask.
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

# A window already running takes it, and has selected its own tmux pane; what
# only out here can be done is bringing the application forward.
if [ -n "$url" ] && "$WEB" --open "$url" 2>/dev/null; then
    [ "$ACTIVATE" = yes ] && /usr/bin/open -a "$TERMINAL"
    exit 0
fi

cmd=$(sq "$WEB")
[ -n "$url" ] && cmd="$cmd $(sq "$url")"

# Outwards from what is already on screen, a step at a time.
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
# No LSUIElement here, deliberately. It keeps the applet out of the Dock, which
# is what you want of something that exits the moment it has passed the url on -
# but System Settings leaves an agent out of the default browser list, and being
# choosable there is the whole point. The list is the only way in: LaunchServices
# refuses to have the browser changed by anything but the user, so duti and every
# other third party gets -54 for asking. So the Dock bounce stays.
$PB -c "Delete :LSUIElement" "$PLIST" >/dev/null 2>&1 || true

# osacompile builds a droplet, because the script has an `open` handler, and a
# droplet claims every file there is - which puts it in the Open With menu of
# everything on the disk. The claim is narrowed to the pages it can actually
# draw.
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
