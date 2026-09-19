# Sourced by each test. Sets up a local folder and a fake remote on this
# machine: $L is the local folder, $R the remote one ($ISF_TEST_REMOTE is the
# remote home). isf reaches the remote through test/fake-ssh.

set -u

TOP=$(cd "$(dirname "$0")/.." && pwd)
ISF=${ISF:-$TOP/isf}
T=$(mktemp -d "${TMPDIR:-/tmp}/isf-test.XXXXXX")
L=$T/local/proj
R=$T/home/proj
PID=

mkdir -p "$T/bin" "$T/local" "$T/home" "$T/state" "$L"
ln -s "$TOP/test/fake-ssh" "$T/bin/ssh"
export PATH="$T/bin:$PATH"
export XDG_STATE_HOME=$T/state
export ISF_TEST_REMOTE=$T/home

if [ -z "${SFTP_SERVER:-}" ]; then
        for p in /usr/lib/ssh/sftp-server /usr/lib/openssh/sftp-server \
                /usr/libexec/openssh/sftp-server /usr/libexec/sftp-server; do
                [ -x "$p" ] && SFTP_SERVER=$p && break
        done
fi
[ -n "${SFTP_SERVER:-}" ] || { echo "no sftp-server found: set SFTP_SERVER" >&2; exit 2; }
export SFTP_SERVER

cleanup() {
        [ -n "$PID" ] && stop
        rm -rf "$T"
}
trap cleanup EXIT

fail() {
        echo "FAIL: $*" >&2
        if [ -s "$T/out" ] || [ -s "$T/err" ]; then
                echo "--- isf output:" >&2
                cat "$T/out" "$T/err" >&2 2>/dev/null
        fi
        exit 1
}

# Start isf in the background, from $T/local, with ARGS (default: sync proj
# with host:proj) and wait until it's watching. Its output goes to $T/out and
# $T/err.
start() {
        [ $# -gt 0 ] || set -- ./proj host:proj
        : >"$T/out"
        : >"$T/err"
        # Its own process group, so stop() gets the ssh children too
        (cd "$T/local" && exec setsid "$ISF" "$@" -I "$ISF" >"$T/out" 2>"$T/err") &
        PID=$!
        for _ in $(seq 200); do
                grep -q "watching for changes" "$T/out" && return 0
                kill -0 "$PID" 2>/dev/null || { PID=; fail "isf exited: $*"; }
                sleep 0.05
        done
        fail "isf didn't start: $*"
}

stop() {
        kill -TERM -- "-$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
        PID=
        check_sanitizers
}

# With a sanitizer build (ISF=...), any report fails the test
check_sanitizers() {
        if grep -qE 'Sanitizer|runtime error:' "$T/err" 2>/dev/null; then
                fail "sanitizer report"
        fi
}

# Run isf in the foreground (from $T/local) and expect it to exit on its own.
# Sets $STATUS.
run() {
        (cd "$T/local" && exec timeout 20 "$ISF" "$@" -I "$ISF") >"$T/out" 2>"$T/err"
        STATUS=$?
        check_sanitizers
}

# What DIR has: paths, types, modes, sizes, whole-second mtimes of files,
# contents and link targets. Directory and symlink mtimes aren't synced.
tree() {
        (cd "$1" && {
                find . -mindepth 1 -type d -printf '%p d %m\n'
                find . -mindepth 1 -type l -printf '%p l %l\n'
                find . -mindepth 1 -type f -printf '%m %s %T@ %p\n' |
                        awk '{ sub(/\.[0-9]*$/, "", $3); print }'
                find . -mindepth 1 -type f -exec md5sum {} + | awk '{ print $2 " md5 " $1 }'
        } | LC_ALL=C sort)
}

# Wait until $L and $R have the same (up to 10 s)
wait_same() {
        for _ in $(seq 200); do
                [ "$(tree "$L")" = "$(tree "$R")" ] && return 0
                sleep 0.05
        done
        diff <(tree "$L") <(tree "$R") >&2
        fail "local and remote differ${1:+: $1}"
}

# Wait until COMMAND succeeds (up to 10 s)
wait_for() {
        for _ in $(seq 200); do
                eval "$1" && return 0
                sleep 0.05
        done
        fail "timed out waiting for: $1"
}

# Changes are sent after 100 ms without events: give the side that is
# expected to stay the same time to (wrongly) change
settle() { sleep 0.5; }

expect_out() { grep -qF -- "$1" "$T/out" || fail "output lacks '$1'"; }
expect_err() { grep -qF -- "$1" "$T/err" || fail "errors lack '$1'"; }
expect_file() { [ "$(cat "$1" 2>/dev/null)" = "$2" ] || fail "'$1' should hold '$2', has '$(cat "$1" 2>&1)'"; }
expect_missing() { [ ! -e "$1" ] && [ ! -L "$1" ] || fail "'$1' should not exist"; }
