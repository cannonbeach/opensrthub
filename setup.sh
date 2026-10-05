#!/usr/bin/env bash
#
# opensrthub installer
#
# Consolidates the old setupopensrthub.sh / setupsystem.sh / setuprepo.sh into a
# single idempotent installer. Safe to re-run: every phase checks for the work it
# would do and skips it if it is already done.
#
# Usage:  ./setup.sh [options]
# Run    ./setup.sh --help    for the option list.
#
set -Eeuo pipefail

#-----------------------------------------------------------------------------
# Configuration
#-----------------------------------------------------------------------------
NODE_MAJOR=22
SRT_TAG="v1.5.4"
FFMPEG_BRANCH="release/6.1"
CURL_REPO="https://github.com/cannonbeach/curl.git"
SRT_REPO="https://github.com/Haivision/srt.git"
FFMPEG_REPO="https://git.ffmpeg.org/ffmpeg.git"

# Kernel command-line parameters this appliance requires. Add to this list to
# have setup.sh manage another one; each is added to /etc/default/grub only if
# it is not already on the command line. A parameter whose key is already present
# with a different value is corrected rather than duplicated.
GRUB_PARAMS=(
    "apparmor=0"                            # AppArmor off at the kernel level
    "cpufreq.default_governor=performance"  # no on-demand scaling under stream load
    "mitigations=off"                       # see the README before changing this
)

APP_DIR="/var/app"
DATA_DIR="/opt/srthub"
DOCKER_IMAGE="dockersrthub"
SERVICE_NAME="opensrthub"
HTTP_PORT=8080

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="${REPO_DIR}/opensrthub-install.log"
JOBS="$(nproc)"

#-----------------------------------------------------------------------------
# Options
#-----------------------------------------------------------------------------
SKIP_DEPS=0
SKIP_BUILD=0
SKIP_DOCKER=0
SKIP_TUNING=0
PURGE_TELEMETRY=0
VERIFY_ONLY=0
REBOOT_REQUIRED=0
SERVICE_MANAGER="systemd"
ADMIN_PASSWORD=""

usage() {
    cat <<EOF
opensrthub installer

Usage: ./setup.sh [options]

Options:
  --skip-deps           Skip the apt / Node.js / npm system package phase.
  --skip-build          Skip building libsrt, libcurl, FFmpeg and srthub.
                        (Use when only the web app or Docker image changed.)
  --skip-docker         Skip building the '${DOCKER_IMAGE}' container image.
  --purge-telemetry     Also apt-purge the crash reporters and phone-home
                        packages instead of only disabling them (apport, whoopsie,
                        kerneloops, ubuntu-report, popularity-contest). Intended
                        for server installs; on a desktop install purging apport
                        can drag the desktop metapackage with it.
                        --purge-apport is accepted as an older name for this.
  --skip-tuning         Skip the host tuning phase (AppArmor, unattended
                        upgrades, MOTD, apt-daily timers, sysctl settings).
                        Use this if you manage host configuration elsewhere.
  --service=MANAGER     How to run the web app: systemd (default), pm2, or none.
  --admin-password=PW   Set the initial admin password non-interactively.
                        Only used when ${DATA_DIR}/users.json does not yet exist.
  --verify              Run the post-install checks only, then exit.
  -h, --help            Show this help.

Examples:
  ./setup.sh                          Full install (first time).
  ./setup.sh --skip-deps              Rebuild + reinstall after a git pull.
  ./setup.sh --skip-deps --skip-build Reinstall the web app only.
  ./setup.sh --verify                 Check an existing installation.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --skip-deps)          SKIP_DEPS=1 ;;
        --skip-build)         SKIP_BUILD=1 ;;
        --skip-docker)        SKIP_DOCKER=1 ;;
        --skip-tuning)        SKIP_TUNING=1 ;;
        --purge-telemetry)    PURGE_TELEMETRY=1 ;;
        --purge-apport)       PURGE_TELEMETRY=1 ;;   # earlier name, still accepted
        --verify)             VERIFY_ONLY=1 ;;
        --service=*)          SERVICE_MANAGER="${arg#*=}" ;;
        --admin-password=*)   ADMIN_PASSWORD="${arg#*=}" ;;
        -h|--help)            usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

case "$SERVICE_MANAGER" in
    systemd|pm2|none) ;;
    *) echo "Invalid --service value: $SERVICE_MANAGER (expected systemd, pm2 or none)" >&2; exit 2 ;;
esac

#-----------------------------------------------------------------------------
# Output helpers
#-----------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[34m'
else
    C_RESET=""; C_BOLD=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""
fi

# hostname -I exits non-zero on some systems and prints nothing when there is no
# address. With pipefail that would abort the script, so swallow it and let the
# caller handle an empty result.
primary_ip() {
    hostname -I 2>/dev/null | awk '{print $1}' || true
}

PHASE_NUM=0
log()   { echo "${C_BLUE}srthub:${C_RESET} $*"; }
warn()  { echo "${C_YELLOW}srthub: WARNING:${C_RESET} $*" >&2; }
ok()    { echo "${C_GREEN}srthub: OK:${C_RESET} $*"; }
skip()  { echo "${C_BLUE}srthub:${C_RESET} $* ${C_BOLD}(already done, skipping)${C_RESET}"; }
# For work skipped because the user asked, not because it was already done.
bypass() { echo "${C_BLUE}srthub:${C_RESET} skipping $* ${C_BOLD}(requested)${C_RESET}"; }
phase() {
    PHASE_NUM=$((PHASE_NUM + 1))
    echo
    echo "${C_BOLD}${C_BLUE}==> [${PHASE_NUM}] $*${C_RESET}"
}
# PID of the top-level shell, so die() can tell whether it is running inside a
# subshell (a command substitution, a pipeline stage) where a plain 'exit' would
# only leave the subshell and let the script carry on past a fatal error.
TOP_PID=$$

die() {
    echo >&2
    if [ "${VERIFY_ONLY:-0}" -eq 1 ]; then
        echo "${C_RED}${C_BOLD}srthub: CHECK FAILED:${C_RESET} $*" >&2
    else
        echo "${C_RED}${C_BOLD}srthub: INSTALL FAILED:${C_RESET} $*" >&2
        echo "${C_RED}srthub:${C_RESET} This script is safe to re-run once the problem is fixed;" >&2
        echo "${C_RED}srthub:${C_RESET} completed steps will be skipped." >&2
    fi
    echo "${C_RED}srthub:${C_RESET} Full log: ${LOG_FILE}" >&2
    if [ "${BASHPID:-$$}" != "$TOP_PID" ]; then
        # Inside a subshell, so 'exit' alone would not stop the script: signal
        # the main shell to abort as well.
        kill -s TERM "$TOP_PID" 2>/dev/null || true
    fi
    exit 1
}

# Mirror everything to the install log.
#
# A FIFO rather than "exec > >(tee ...)": bash does not set $! for a process
# substitution, so there is no way to wait for tee to drain, and the script can
# exit before the final (most important) error message is ever written.
LOG_FIFO="$(mktemp -u "${TMPDIR:-/tmp}/opensrthub-log.XXXXXX")"
mkfifo "$LOG_FIFO"
tee -a "$LOG_FILE" < "$LOG_FIFO" &
TEE_PID=$!
exec > "$LOG_FIFO" 2>&1
rm -f "$LOG_FIFO"

cleanup() {
    # Stop refreshing the sudo timestamp.
    if [ -n "${SUDO_KEEPALIVE_PID:-}" ]; then
        kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
    fi
    # Close the log pipe so tee sees EOF, then wait for it to finish writing.
    exec 1>&- 2>&- || true
    if [ -n "${TEE_PID:-}" ]; then
        wait "$TEE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'die "unexpected error on line $LINENO (command: $BASH_COMMAND)"' ERR
trap 'exit 1' TERM

echo "=== opensrthub install started $(date -Is) ==="

#-----------------------------------------------------------------------------
# Privilege handling
#-----------------------------------------------------------------------------
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
    # Separate handle for apt calls: "$SUDO -E" would expand to a bare "-E"
    # when SUDO is empty. Already root, so the environment carries through.
    SUDO_E=""
    # Remember who invoked us so build artifacts don't end up root-owned in a
    # user's checkout (the old scripts did exactly that).
    INVOKING_USER="${SUDO_USER:-}"
else
    SUDO="sudo"
    SUDO_E="sudo -E"
    INVOKING_USER="$(id -un)"
    command -v sudo >/dev/null 2>&1 || die "sudo is not installed and this script is not running as root."
    # --verify is read-only, so don't make the user authenticate just to look.
    if [ "$VERIFY_ONLY" -eq 0 ]; then
        log "Requesting sudo privileges up front (the build takes a while and"
        log "should not stop halfway through to ask for a password)..."
        sudo -v || die "Could not obtain sudo privileges."
        # Keep the sudo timestamp alive for the duration of the build.
        ( while true; do sudo -n true 2>/dev/null || exit; sleep 50; done ) &
        SUDO_KEEPALIVE_PID=$!
    elif ! sudo -n true 2>/dev/null; then
        # No cached credentials: fall back to unprivileged checks rather than
        # prompting. Docker checks will simply report as failures.
        warn "no cached sudo credentials; container checks may report as failed."
        warn "run 'sudo -v' first for a complete report."
    fi
fi

#-----------------------------------------------------------------------------
# Verification (also runnable standalone via --verify)
#-----------------------------------------------------------------------------
verify_install() {
    local failures=0
    local host_ip
    host_ip="$(primary_ip)"
    [ -n "$host_ip" ] || host_ip="localhost"

    check() {
        local label="$1"; shift
        if "$@" >/dev/null 2>&1; then
            printf '  %s[ ok ]%s %s\n' "$C_GREEN" "$C_RESET" "$label"
        else
            printf '  %s[fail]%s %s\n' "$C_RED" "$C_RESET" "$label"
            failures=$((failures + 1))
        fi
    }

    echo
    echo "${C_BOLD}Post-install checks${C_RESET}"
    check "srthub binary built            (${REPO_DIR}/srthub)" test -x "${REPO_DIR}/srthub"
    check "data directories present       (${DATA_DIR})"        test -d "${DATA_DIR}/configs"
    check "web app installed              (${APP_DIR}/server.js)" test -f "${APP_DIR}/server.js"
    check "node modules installed         (${APP_DIR}/node_modules)" test -d "${APP_DIR}/node_modules/express"
    check "TLS certificate present        (${APP_DIR}/cert)"    test -s "${APP_DIR}/cert/server.crt"
    check "user database present          (${DATA_DIR}/users.json)" test -s "${DATA_DIR}/users.json"
    check "docker daemon running"                               $SUDO docker info
    check "container image built          (${DOCKER_IMAGE})"    $SUDO docker image inspect "$DOCKER_IMAGE"

    case "$SERVICE_MANAGER" in
        systemd) check "service enabled at boot        (${SERVICE_NAME}.service)" systemctl is-enabled "${SERVICE_NAME}.service"
                 check "service running                (${SERVICE_NAME}.service)" systemctl is-active "${SERVICE_NAME}.service" ;;
        pm2)     check "pm2 process running            (${SERVICE_NAME})" bash -c "$SUDO pm2 describe ${SERVICE_NAME} >/dev/null" ;;
    esac

    if [ "$SKIP_TUNING" -eq 0 ]; then
        # Read the live values rather than the config file: a setting that is in
        # sysctl.conf but was rejected by the kernel is not actually applied.
        sysctl_is() {
            [ "$(sysctl -n "$1" 2>/dev/null)" = "$2" ]
        }
        check "sysctl tcp_syncookies=1"        sysctl_is net.ipv4.tcp_syncookies 1
        check "sysctl accept_redirects=0"      sysctl_is net.ipv4.conf.all.accept_redirects 0
        check "sysctl ipv6 disabled"           sysctl_is net.ipv6.conf.all.disable_ipv6 1
        check "sysctl randomize_va_space=2"    sysctl_is kernel.randomize_va_space 2
        check "sysctl core_uses_pid=1"         sysctl_is kernel.core_uses_pid 1
        check "sysctl suid_dumpable=1"         sysctl_is fs.suid_dumpable 1
        check "apt-daily.timer disabled"       bash -c '! systemctl is-enabled apt-daily.timer 2>/dev/null | grep -q "^enabled$"'
        check "apport disabled"                bash -c '! systemctl is-enabled apport.service 2>/dev/null | grep -q "^enabled$"'
        check "whoopsie (crash upload) off"    bash -c '! systemctl is-enabled whoopsie.service 2>/dev/null | grep -q "^enabled$"'
        check "popularity-contest not sending" bash -c '! grep -q "PARTICIPATE=\"yes\"" /etc/popularity-contest.conf 2>/dev/null'
        check "popcon cron job inert"          bash -c '! test -x /etc/cron.daily/popularity-contest'
        check "ubuntu-report cannot run"       bash -c '! test -x /usr/bin/ubuntu-report'

        # sshd -T prints the configuration sshd has actually resolved, so this
        # catches a drop-in that was written but never read.
        # Read the governor the kernel is actually using, not what was configured:
        # the whole point is that the configured value used to be overridden.
        if [ -e /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
            check "cpu governor is performance"  bash -c "! grep -L performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | grep -q ."
            check "ondemand.service disabled"    bash -c '! systemctl is-enabled ondemand.service 2>/dev/null | grep -q "^enabled$"'
        fi

        check "sshd config valid"              bash -c "$SUDO sshd -t"
        check "sshd: no CBC ciphers"           bash -c "! $SUDO sshd -T 2>/dev/null | grep -i '^ciphers ' | grep -q cbc"
        check "sshd: no SHA1/MD5 MACs"         bash -c "! $SUDO sshd -T 2>/dev/null | grep -i '^macs ' | grep -qE 'md5|sha1'"
        check "sshd: no weak kex"              bash -c "! $SUDO sshd -T 2>/dev/null | grep -i '^kexalgorithms ' | grep -qE 'group1-|group14-sha1|gss-'"
        check "sshd: PermitRootLogin no"       bash -c "$SUDO sshd -T 2>/dev/null | grep -qi '^permitrootlogin no'"
        check "sshd: MaxAuthTries 4"           bash -c "$SUDO sshd -T 2>/dev/null | grep -qi '^maxauthtries 4'"
        check "sshd: LoginGraceTime 60"        bash -c "$SUDO sshd -T 2>/dev/null | grep -qi '^logingracetime 60'"

        check "core_pattern is not apport"     bash -c '! grep -q apport /proc/sys/kernel/core_pattern'
        check "core_pattern -> ${DATA_DIR}/cores" bash -c "grep -q '^${DATA_DIR}/cores/' /proc/sys/kernel/core_pattern"
        check "core dump dir writable"         test -w "${DATA_DIR}/cores"
        check "unattended-upgrades disabled"   bash -c '! systemctl is-enabled unattended-upgrades.service 2>/dev/null | grep -q "^enabled$"'
        check "apparmor.service disabled"      bash -c '! systemctl is-enabled apparmor.service 2>/dev/null | grep -q "^enabled$"'
        # Check the generated boot config, not /etc/default/grub: a drop-in in
        # /etc/default/grub.d can override that file, so only the generated
        # config shows what the kernel will actually be handed. Whether a
        # parameter is in force is a separate question from whether it is
        # configured, so both are reported.
        for gp in "${GRUB_PARAMS[@]}"; do
            if grep -qE "(^| )${gp//./\\.}( |\$)" /proc/cmdline 2>/dev/null; then
                printf '  %s[ ok ]%s %s\n' "$C_GREEN" "$C_RESET" "${gp} active on the running kernel"
            elif $SUDO grep -q -- "$gp" /boot/grub/grub.cfg 2>/dev/null; then
                printf '  %s[pend]%s %s\n' "$C_YELLOW" "$C_RESET" "${gp} in the boot config, NOT yet active - reboot required"
            else
                printf '  %s[fail]%s %s\n' "$C_RED" "$C_RESET" "${gp} missing from /boot/grub/grub.cfg"
                failures=$((failures + 1))
            fi
        done
    fi

    # Give the service a moment on a cold start before probing the port.
    local attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        curl -sk --max-time 2 "https://127.0.0.1:${HTTP_PORT}/" >/dev/null 2>&1 && break
        sleep 1
    done
    check "web app answering on :${HTTP_PORT}" curl -sk --max-time 5 "https://127.0.0.1:${HTTP_PORT}/"

    echo
    if [ "$failures" -eq 0 ]; then
        echo "${C_GREEN}${C_BOLD}All checks passed.${C_RESET}"
        echo
        echo "  Open:     ${C_BOLD}https://${host_ip}:${HTTP_PORT}${C_RESET}"
        echo "  Login:    admin / (the password set during install)"
        echo "  Users:    ${DATA_DIR}/users.json"
        case "$SERVICE_MANAGER" in
            systemd) echo "  Logs:     sudo journalctl -u ${SERVICE_NAME} -f" ;;
            pm2)     echo "  Logs:     sudo pm2 logs ${SERVICE_NAME}" ;;
        esac
        echo
        echo "  Your browser will warn about the certificate: it is self-signed."
        echo "  Replace ${APP_DIR}/cert/server.{key,crt} with a real certificate to remove the warning."
        return 0
    fi
    echo "${C_RED}${C_BOLD}${failures} check(s) failed.${C_RESET} See ${LOG_FILE} for details."
    return 1
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
    if verify_install; then exit 0; else exit 1; fi
fi

#-----------------------------------------------------------------------------
# preflight
#-----------------------------------------------------------------------------
phase "Preflight checks"

[ -f "${REPO_DIR}/Makefile" ] && [ -d "${REPO_DIR}/source" ] \
    || die "setup.sh must be run from inside the opensrthub repository."

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    log "Detected OS: ${PRETTY_NAME:-unknown}"
    if [ "${ID:-}" != "ubuntu" ]; then
        warn "This installer targets Ubuntu. '${ID:-unknown}' is untested; package names may differ."
    fi
    UBUNTU_VERSION="${VERSION_ID:-24.04}"
else
    warn "Cannot read /etc/os-release; assuming Ubuntu 24.04 for the container base image."
    UBUNTU_VERSION="24.04"
fi

# The srthub binary is built on the host and then copied into the container, so
# the container's glibc must be at least as new as the host's. Matching the
# version is the simplest way to guarantee that.
log "Container base image will be ubuntu:${UBUNTU_VERSION} (matched to this host)"

AVAIL_KB="$(df -Pk "$REPO_DIR" 2>/dev/null | awk 'NR==2 {print $4}' || true)"
case "$AVAIL_KB" in
    ''|*[!0-9]*) AVAIL_KB=0 ;;
esac
if [ "$AVAIL_KB" -gt 0 ] && [ "$AVAIL_KB" -lt 10485760 ]; then
    warn "Less than 10 GB free on $(df -Ph "$REPO_DIR" | awk 'NR==2 {print $6}'). The FFmpeg and SRT builds may run out of space."
fi
log "Build parallelism: -j${JOBS}"

#-----------------------------------------------------------------------------
# directory layout
#-----------------------------------------------------------------------------
phase "Creating directory layout"

for d in "${DATA_DIR}" "${DATA_DIR}/configs" "${DATA_DIR}/status" \
         "${DATA_DIR}/thumbnail" "${DATA_DIR}/scan" "${DATA_DIR}/cores" \
         "${APP_DIR}" "${APP_DIR}/public" "${APP_DIR}/cert"; do
    if [ -d "$d" ]; then
        skip "directory $d"
    else
        log "creating $d"
        $SUDO mkdir -p "$d"
    fi
done

# Core dumps are written by the kernel as the crashing process's own uid, so this
# directory must be writable by anything that might crash. The cores themselves
# land as 0600 and the sticky bit stops one user clearing another's - the same
# arrangement Ubuntu uses for /var/crash.
$SUDO chmod 1777 "${DATA_DIR}/cores"

#-----------------------------------------------------------------------------
# system packages
#-----------------------------------------------------------------------------
phase "Installing system packages"

if [ "$SKIP_DEPS" -eq 1 ]; then
    bypass "the system package phase (--skip-deps)"
else
    export DEBIAN_FRONTEND=noninteractive

    # Keep unattended upgrades from opening the 'which services to restart'
    # dialog, which would otherwise block a hands-off install on Ubuntu 24.04.
    if [ -d /etc/needrestart/conf.d ]; then
        echo '$nrconf{restart} = "a";' | $SUDO tee /etc/needrestart/conf.d/99-opensrthub.conf >/dev/null
    fi

    log "apt-get update"
    $SUDO_E apt-get update -y

    log "apt-get upgrade"
    $SUDO_E apt-get upgrade -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold"

    # One transaction instead of ~30: faster, and a single point of failure.
    APT_PACKAGES=(
        # toolchain
        build-essential cmake autoconf automake libtool pkg-config nasm tcl git
        # libraries the build links against
        libssl-dev zlib1g-dev liblzma-dev
        # runtime / operational tooling
        curl ca-certificates gnupg jq net-tools ethtool tcpdump ifstat
        zip unzip cpufrequtils
        # container runtime
        docker.io
    )
    log "installing: ${APT_PACKAGES[*]}"
    $SUDO_E apt-get install -y "${APT_PACKAGES[@]}"

    # docker-buildx exists on 24.04 but not on older releases, and the build
    # works fine with plain 'docker build'. Treat it as optional.
    if ! $SUDO_E apt-get install -y docker-buildx 2>/dev/null; then
        log "docker-buildx not available on this release (not required)"
    fi

    # Node.js via NodeSource.
    #
    # Check for the binary before running it: on a fresh system 'node' does not
    # exist, and with pipefail a 127 from the left of the pipe would abort here.
    if command -v node >/dev/null 2>&1; then
        CURRENT_NODE_MAJOR="$(node -v | sed 's/^v\([0-9]*\).*/\1/')"
    else
        CURRENT_NODE_MAJOR=0
    fi
    # Anything non-numeric would make the comparison below an error, not a false.
    case "$CURRENT_NODE_MAJOR" in
        ''|*[!0-9]*) CURRENT_NODE_MAJOR=0 ;;
    esac

    if [ "$CURRENT_NODE_MAJOR" -ge "$NODE_MAJOR" ]; then
        skip "Node.js v${CURRENT_NODE_MAJOR} (>= ${NODE_MAJOR}) already installed"
    else
        log "installing Node.js ${NODE_MAJOR}.x from NodeSource"
        $SUDO mkdir -p /etc/apt/keyrings
        curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
            | $SUDO gpg --dearmor --yes -o /etc/apt/keyrings/nodesource.gpg
        echo "deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_${NODE_MAJOR}.x nodistro main" \
            | $SUDO tee /etc/apt/sources.list.d/nodesource.list >/dev/null
        $SUDO_E apt-get update -y
        $SUDO_E apt-get install -y nodejs
        if command -v node >/dev/null 2>&1; then
            ok "installed Node.js $(node -v)"
        else
            die "the nodejs package installed but produced no 'node' binary."
        fi
    fi

    log "ensuring the docker daemon is enabled and running"
    $SUDO systemctl enable --now docker

    if [ "$SERVICE_MANAGER" = "pm2" ]; then
        log "installing pm2"
        $SUDO npm install -g pm2@latest
        $SUDO pm2 install pm2-logrotate || warn "pm2-logrotate install failed (non-fatal)"
    fi
fi

#-----------------------------------------------------------------------------
# third-party libraries
#-----------------------------------------------------------------------------
phase "Building third-party libraries"

# clone_at <repo-url> <target-dir> <git-ref> <branch-name>
clone_at() {
    local repo="$1" dir="$2" ref="$3" branch="$4"
    if [ -d "${dir}/.git" ]; then
        log "${dir} already cloned"
    else
        log "cloning ${repo} into ${dir}"
        rm -rf "$dir"
        git clone "$repo" "$dir"
    fi
    pushd "$dir" >/dev/null || die "cannot enter ${dir}"
    # Re-running 'checkout -b' on an existing branch is an error, so only create
    # the branch if it isn't there yet.
    if git rev-parse --verify --quiet "$branch" >/dev/null; then
        git checkout "$branch"
    else
        git checkout -b "$branch" "$ref"
    fi
    popd >/dev/null
}

if [ "$SKIP_BUILD" -eq 1 ]; then
    bypass "the third-party library build (--skip-build)"
else
    # --- libsrt -------------------------------------------------------------
    if [ -f "${REPO_DIR}/cbsrt/libsrt.a" ]; then
        skip "libsrt (cbsrt/libsrt.a exists)"
    else
        log "building libsrt ${SRT_TAG}"
        clone_at "$SRT_REPO" "${REPO_DIR}/cbsrt" "tags/${SRT_TAG}" "${SRT_TAG}"
        pushd "${REPO_DIR}/cbsrt" >/dev/null || die "cannot enter cbsrt"
        ./configure --prefix=/usr
        make -j"${JOBS}"
        popd >/dev/null
        [ -f "${REPO_DIR}/cbsrt/libsrt.a" ] || die "libsrt.a was not produced. See ${LOG_FILE}."
        ok "libsrt built"
    fi

    # --- libcurl ------------------------------------------------------------
    if [ -f "${REPO_DIR}/cblibcurl/lib/.libs/libcurl.a" ]; then
        skip "libcurl (cblibcurl/lib/.libs/libcurl.a exists)"
    else
        log "building libcurl"
        if [ -d "${REPO_DIR}/cblibcurl/.git" ]; then
            log "cblibcurl already cloned"
        else
            rm -rf "${REPO_DIR}/cblibcurl"
            git clone "$CURL_REPO" "${REPO_DIR}/cblibcurl"
        fi
        pushd "${REPO_DIR}/cblibcurl" >/dev/null || die "cannot enter cblibcurl"

        # curl's configure.ac declares no AC_CONFIG_AUX_DIR, so autoconf and
        # libtoolize fall back to searching for install-sh / install.sh / shtool
        # in '.', then '..', then '../..', using the first directory that has
        # one. Any install.sh above the checkout therefore captures curl's config
        # aux directory: libtoolize writes ltmain.sh outside the curl tree and
        # automake then dies with "required file './ltmain.sh' not found".
        #
        # Seeding install-sh into the tree does NOT fix this - install-sh is on
        # buildconf's own cleanup list, so it is deleted before libtoolize runs.
        # Declaring the aux directory explicitly does; libtoolize then reports it
        # is honouring AC_CONFIG_AUX_DIR and the build is immune to whatever sits
        # above the checkout.
        if ! grep -q '^AC_CONFIG_AUX_DIR' configure.ac; then
            log "declaring AC_CONFIG_AUX_DIR in curl's configure.ac"
            awk '{print} /^AC_INIT/ && !d {print "AC_CONFIG_AUX_DIR([.])"; d=1}' \
                configure.ac > configure.ac.opensrthub
            mv configure.ac.opensrthub configure.ac
            grep -q '^AC_CONFIG_AUX_DIR' configure.ac \
                || die "could not declare AC_CONFIG_AUX_DIR in curl's configure.ac."
        fi

        if [ ! -f configure ]; then
            log "generating curl's configure script (buildconf)"
            # autoconf 2.70+ enables -Wobsolete by default (2.69 did not), and
            # this curl fork predates the AC_HELP_STRING -> AS_HELP_STRING rename
            # and still uses AC_TRY_*, AC_HEADER_TIME and AC_TYPE_SIGNAL. On
            # Ubuntu 24.04's autoconf 2.71 that means several hundred lines of
            # "macro is obsolete" warnings out of buildconf. The macros still
            # work, so suppress just that category - genuine syntax and
            # portability warnings still come through. autoconf, aclocal and
            # automake all honour $WARNINGS.
            WARNINGS=no-obsolete ./buildconf
        fi
        [ -f configure ] || die "curl's configure script was not generated. See ${LOG_FILE}."
        ./configure --prefix=/usr --enable-static --enable-pthreads \
                    --without-ssl --without-librtmp --without-libidn2 \
                    --without-nghttp2 --without-brotli
        make -j"${JOBS}"
        popd >/dev/null
        [ -f "${REPO_DIR}/cblibcurl/lib/.libs/libcurl.a" ] || die "libcurl.a was not produced. See ${LOG_FILE}."
        ok "libcurl built"
    fi

    # --- FFmpeg -------------------------------------------------------------
    if [ -f "${REPO_DIR}/cbffmpeg/libavcodec/libavcodec.a" ]; then
        skip "FFmpeg (cbffmpeg/libavcodec/libavcodec.a exists)"
    else
        log "building FFmpeg ${FFMPEG_BRANCH} (this is the slowest step, typically 5-15 minutes)"
        clone_at "$FFMPEG_REPO" "${REPO_DIR}/cbffmpeg" "remotes/origin/${FFMPEG_BRANCH}" "release6.1"
        pushd "${REPO_DIR}/cbffmpeg" >/dev/null || die "cannot enter cbffmpeg"
        ./configure --prefix=/usr --disable-encoders --disable-iconv \
                    --disable-v4l2-m2m --disable-muxers --disable-vaapi \
                    --disable-vdpau --disable-videotoolbox --disable-avdevice \
                    --enable-encoder=mjpeg
        make -j"${JOBS}"
        popd >/dev/null
        [ -f "${REPO_DIR}/cbffmpeg/libavcodec/libavcodec.a" ] || die "libavcodec.a was not produced. See ${LOG_FILE}."
        ok "FFmpeg built"
    fi
fi

#-----------------------------------------------------------------------------
# build srthub
#-----------------------------------------------------------------------------
phase "Building srthub"

if [ "$SKIP_BUILD" -eq 1 ] && [ -x "${REPO_DIR}/srthub" ]; then
    bypass "the srthub build (--skip-build)"
else
    pushd "$REPO_DIR" >/dev/null
    make -j"${JOBS}"
    popd >/dev/null
    [ -x "${REPO_DIR}/srthub" ] || die "the srthub binary was not produced. See ${LOG_FILE}."
    ok "srthub built"
fi

#-----------------------------------------------------------------------------
# container image
#-----------------------------------------------------------------------------
# The web app launches every stream as a container:
#   docker run ... dockersrthub /usr/bin/srthub <id>
# so this image is mandatory, not optional. The old setup script never built it,
# which meant a fresh install could start the UI but not start a stream.
phase "Building the ${DOCKER_IMAGE} container image"

if [ "$SKIP_DOCKER" -eq 1 ]; then
    bypass "the container image build (--skip-docker)"
else
    $SUDO docker info >/dev/null 2>&1 || die "the docker daemon is not running. Try: sudo systemctl start docker"
    log "copying srthub binary into the build context"
    $SUDO cp "${REPO_DIR}/srthub" "${REPO_DIR}/docker/srthub"
    pushd "${REPO_DIR}/docker" >/dev/null
    log "docker build (base image ubuntu:${UBUNTU_VERSION})"
    $SUDO docker build --build-arg "UBUNTU_VERSION=${UBUNTU_VERSION}" -t "$DOCKER_IMAGE" .
    popd >/dev/null
    $SUDO docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1 \
        || die "the ${DOCKER_IMAGE} image was not created. See ${LOG_FILE}."

    # Confirm the binary's shared libraries actually resolve under the
    # container's glibc. A silent mismatch here is the difference between
    # "the UI works" and "streams refuse to start". Checking the loader rather
    # than running srthub avoids blocking on a process that expects a config.
    log "checking that srthub's libraries resolve inside the container"
    if $SUDO docker run --rm --entrypoint /bin/sh "$DOCKER_IMAGE" \
           -c 'ldd /usr/bin/srthub 2>&1' | grep -q 'not found'; then
        die "srthub cannot load inside the ${DOCKER_IMAGE} image (missing shared libraries).
       The container base image (ubuntu:${UBUNTU_VERSION}) is older than this host's glibc."
    fi
    ok "container image built and srthub loads correctly inside it"
fi

#-----------------------------------------------------------------------------
# install the web application
#-----------------------------------------------------------------------------
phase "Installing the web application"

install_file() {
    local src="$1" dest="$2"
    [ -f "$src" ] || die "expected file not found: ${src}"
    log "installing $(basename "$src") -> ${dest}"
    $SUDO cp "$src" "$dest"
}

install_file "${REPO_DIR}/webapp/server.js"          "${APP_DIR}/"
install_file "${REPO_DIR}/webapp/authenticate.html"  "${APP_DIR}/"
install_file "${REPO_DIR}/webapp/package.json"       "${APP_DIR}/"
install_file "${REPO_DIR}/webapp/public/index.html"  "${APP_DIR}/public/"
if [ -f "${REPO_DIR}/webapp/package-lock.json" ]; then
    install_file "${REPO_DIR}/webapp/package-lock.json" "${APP_DIR}/"
fi
# client.js was the predecessor of the inline script in index.html. Nothing
# loads it any more, so an install that still has it is serving a dead 47KB
# file to every logged-in browser that asks for /srthub/client.js.
if [ -f "${APP_DIR}/public/client.js" ]; then
    log "removing the obsolete public/client.js"
    $SUDO rm -f "${APP_DIR}/public/client.js"
fi

# Node modules are installed locally from a lockfile rather than globally with a
# symlink into /usr/lib/node_modules, so the versions are reproducible.
if [ -L "${APP_DIR}/node_modules" ]; then
    log "removing the legacy /usr/lib/node_modules symlink"
    $SUDO rm -f "${APP_DIR}/node_modules"
fi

log "installing web app dependencies"
pushd "$APP_DIR" >/dev/null
if [ -f package-lock.json ]; then
    $SUDO npm ci --omit=dev
else
    $SUDO npm install --omit=dev
fi
popd >/dev/null
[ -d "${APP_DIR}/node_modules/express" ] || die "npm did not install the web app dependencies. See ${LOG_FILE}."
ok "web app dependencies installed"

#-----------------------------------------------------------------------------
# event log rotation
#-----------------------------------------------------------------------------
phase "Configuring event log rotation"

# /var/log/srthub.log is the event log the web GUI reads, one JSON object per
# line. Nothing rotated it before this, so it grew without limit: a busy
# appliance can put tens of megabytes a day into it, and installs have been
# found with it well past a hundred megabytes.
#
# Daily with "rotate 30" is what the GUI's day picker expects to find: thirty
# days of history and a file boundary near midnight.
#
# dateext names the rotations for a day instead of numbering them, and
# dateyesterday names each one for the day its events came from rather than the
# day logrotate happened to run. The GUI does not trust those names - it picks
# files by when they were written and filters entries on their own timestamps -
# but a name that matches the contents matters to whoever reads the directory.
#
# delaycompress leaves the most recent rotation uncompressed, which is the one
# the GUI reaches for most; the server reads .gz through zlib either way.
#
# The one cost of dateext: if the box was off across a rotation and logrotate
# catches up twice in one day, the second run finds its target name taken and
# skips, so that day keeps appending to the live log. It corrects itself at the
# next rotation and the GUI is unaffected either way, since it reads files by
# when they were written rather than by what they are called.
#
# No copytruncate: server.js appends with appendFileSync, which opens and closes
# the file for every event, so a rename strands no descriptor and loses no
# writes. If that ever changes to a held-open stream, this needs copytruncate or
# events will vanish into the unlinked inode after the first rotation.
log "installing /etc/logrotate.d/srthub (daily, 30 days)"
$SUDO tee /etc/logrotate.d/srthub >/dev/null <<'LOGROTATE'
# Managed by opensrthub setup.sh
/var/log/srthub.log {
    daily
    rotate 30
    dateext
    dateyesterday
    missingok
    notifempty
    compress
    delaycompress
    create 0640 root adm
}
LOGROTATE

# The file has to exist for the GUI to append to it, and logrotate's "create"
# only applies from the first rotation onwards.
if [ ! -f /var/log/srthub.log ]; then
    log "creating /var/log/srthub.log"
    $SUDO touch /var/log/srthub.log
    $SUDO chown root:adm /var/log/srthub.log
    $SUDO chmod 0640 /var/log/srthub.log
fi

if $SUDO logrotate --debug /etc/logrotate.d/srthub >/dev/null 2>&1; then
    ok "event log rotates daily and keeps 30 days"
else
    warn "logrotate did not accept /etc/logrotate.d/srthub; the event log will not rotate"
fi

#-----------------------------------------------------------------------------
# credentials and TLS certificate
#-----------------------------------------------------------------------------
phase "Configuring credentials and TLS"

if [ -s "${DATA_DIR}/users.json" ]; then
    skip "users.json (existing credentials preserved)"
else
    if [ -z "$ADMIN_PASSWORD" ] && [ -t 0 ] && [ -e /dev/tty ]; then
        # Prompt on /dev/tty rather than stdout: stdout is piped through tee for
        # the install log, which can hold a newline-less prompt in a buffer.
        {
            printf '\n%sSet the initial web login password for user '"'"'admin'"'"'.%s\n' "$C_BOLD" "$C_RESET"
            printf 'Leave blank to have one generated for you.\n'
        } > /dev/tty
        printf 'Password: ' > /dev/tty
        IFS= read -rs ADMIN_PASSWORD < /dev/tty || true
        printf '\n' > /dev/tty
        if [ -n "$ADMIN_PASSWORD" ]; then
            printf 'Confirm:  ' > /dev/tty
            IFS= read -rs ADMIN_PASSWORD_CONFIRM < /dev/tty || true
            printf '\n' > /dev/tty
            [ "$ADMIN_PASSWORD" = "$ADMIN_PASSWORD_CONFIRM" ] || die "passwords did not match."
        fi
    fi
    if [ -z "$ADMIN_PASSWORD" ]; then
        # No trailing 'head' in the pipeline: it closes the pipe early, tr takes
        # SIGPIPE, and pipefail turns that into a fatal error. Trim with a shell
        # expansion instead.
        ADMIN_PASSWORD="$(head -c 32 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' || true)"
        ADMIN_PASSWORD="${ADMIN_PASSWORD:0:20}"
        if [ -z "$ADMIN_PASSWORD" ]; then
            die "could not generate a random password from /dev/urandom."
        fi
        GENERATED_PASSWORD=1
    fi
    log "creating ${DATA_DIR}/users.json"
    printf '[{"username":"admin","password":"%s"}]\n' "$ADMIN_PASSWORD" \
        | $SUDO tee "${DATA_DIR}/users.json" >/dev/null
    $SUDO chmod 600 "${DATA_DIR}/users.json"
fi

if [ -s "${APP_DIR}/cert/server.crt" ] && [ -s "${APP_DIR}/cert/server.key" ]; then
    skip "TLS certificate (existing certificate preserved)"
else
    CERT_CN="$(hostname -f 2>/dev/null || hostname)"
    CERT_IP="$(primary_ip)"
    SAN="DNS:${CERT_CN},DNS:localhost,IP:127.0.0.1"
    [ -n "$CERT_IP" ] && SAN="${SAN},IP:${CERT_IP}"
    log "generating a self-signed certificate for ${CERT_CN}"
    # -subj and -addext keep this non-interactive; the old script stopped here
    # and waited for a country code.
    $SUDO openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -subj "/CN=${CERT_CN}" -addext "subjectAltName=${SAN}" \
        -keyout "${APP_DIR}/cert/server.key" \
        -out "${APP_DIR}/cert/server.crt"
    $SUDO chmod 600 "${APP_DIR}/cert/server.key"
    ok "certificate generated (self-signed, valid 10 years)"
fi

#-----------------------------------------------------------------------------
# host tuning
#-----------------------------------------------------------------------------
# This box is expected to run as a dedicated streaming appliance, so background
# package activity, login banners and MAC enforcement are turned off, and a few
# network/kernel settings are pinned. Skip the whole phase with --skip-tuning if
# you manage host configuration with Ansible/cloud-init/etc.
phase "Tuning the host"

# disable_unit <unit> - stop, disable and mask a unit if it exists here.
# Masking matters: disabling alone still lets another unit pull it back in.
disable_unit() {
    local unit="$1"
    if [ -z "$(systemctl list-unit-files "$unit" --no-legend 2>/dev/null)" ]; then
        log "${unit} is not present on this system"
        return 0
    fi
    $SUDO systemctl disable --now "$unit" >/dev/null 2>&1 || true
    $SUDO systemctl mask "$unit" >/dev/null 2>&1 || true
    log "disabled and masked ${unit}"
}

if [ "$SKIP_TUNING" -eq 1 ]; then
    bypass "the host tuning phase (--skip-tuning)"
else
    # --- AppArmor -----------------------------------------------------------
    # Disabled at both levels: the service (removes the distro profile set now)
    # and the kernel command line (keeps it off for good, after a reboot).
    #
    # This is docker-safe. runc calls apparmor.HostSupports(), which reads
    # /sys/module/apparmor/parameters/enabled, and skips profile application
    # entirely when AppArmor is unavailable - it does not fail the container.
    # The failure mode to watch for is the opposite one: AppArmor enabled in the
    # kernel but apparmor_parser missing, so 'docker-default' cannot load.
    if [ -n "$(systemctl list-unit-files apparmor.service --no-legend 2>/dev/null)" ]; then
        disable_unit apparmor.service
        # Deliberately NOT running aa-teardown. Ubuntu's apparmor.service sets
        # ExecStop=/bin/true precisely so that stopping the service does not
        # unload the profile set, because unloading it on a running system breaks
        # anything that expects a profile to be there. aa-teardown bypasses that:
        # it unloads docker-default while the AppArmor LSM is still active, and
        # every subsequent "docker run"/"docker build" then fails with
        #   apparmor failed to apply profile: ... no such file or directory
        # Disabling and masking the unit keeps AppArmor off from the next boot,
        # and apparmor=0 on the kernel command line turns it off for real. The
        # loaded profiles stay in place until that reboot, which is what keeps
        # docker working for the rest of this install.
        log "profiles stay loaded until the reboot (so docker keeps working)"
    else
        log "the AppArmor service is not installed on this system"
    fi

    # --- kernel command line ------------------------------------------------
    # Managed as a delimited block appended to /etc/default/grub, driven by the
    # GRUB_PARAMS list at the top of this script. Nothing is written if every
    # parameter is already on the command line.
    #
    # Note the ordering trap: grub-mkconfig sources /etc/default/grub FIRST and
    # then /etc/default/grub.d/*.cfg, so a drop-in overrides this file. Ubuntu
    # cloud images ship /etc/default/grub.d/50-cloudimg-settings.cfg, which
    # reassigns GRUB_CMDLINE_LINUX_DEFAULT and would silently discard our
    # parameters. So after writing the block we recompute the value the way
    # grub-mkconfig does, and if a drop-in wins we add our own, later, drop-in.
    GRUB_FILE="/etc/default/grub"
    GRUB_D_DIR="/etc/default/grub.d"
    GRUB_D_FILE="${GRUB_D_DIR}/99-opensrthub.cfg"
    GRUB_CFG="/boot/grub/grub.cfg"
    GRUB_BEGIN="# BEGIN opensrthub (managed by setup.sh - do not edit this block)"
    GRUB_END="# END opensrthub"

    # Set one "key=value" on a command line, replacing any existing token with
    # the same key so a wrong value is corrected instead of duplicated.
    grub_set_param() {
        local cur="$1" param="$2" key esc
        key="${param%%=*}"
        # Escape regex metacharacters in the key (cpufreq.default_governor has dots).
        esc="$(printf '%s' "$key" | sed -E 's/[][^$.*+?(){}|\\/]/\\&/g')"
        cur="$(printf '%s' "$cur" \
            | sed -E "s/(^| )${esc}=[^ ]*/ /g; s/[[:space:]]+/ /g; s/^ //; s/ \$//")"
        printf '%s' "${cur:+${cur} }${param}"
    }

    # Apply every managed parameter to a command line.
    grub_apply_params() {
        local out="$1" p
        for p in "${GRUB_PARAMS[@]}"; do
            out="$(grub_set_param "$out" "$p")"
        done
        printf '%s' "$out"
    }

    # Which managed parameters are absent from a command line (space separated).
    grub_missing_params() {
        local cmdline=" $1 " p out=""
        for p in "${GRUB_PARAMS[@]}"; do
            case "$cmdline" in
                *" $p "*) ;;
                *) out="${out:+$out }$p" ;;
            esac
        done
        printf '%s' "$out"
    }

    # Replicate grub-mkconfig's own sourcing order and report the value that
    # actually reaches the kernel. $1 is the grub file; pass a non-empty $2 to
    # exclude our own drop-in from the calculation.
    grub_effective_cmdline() {
        (
            set +eu
            [ -f "$1" ] && . "$1" >/dev/null 2>&1
            for _x in "${GRUB_D_DIR}"/*.cfg; do
                [ -e "$_x" ] || continue
                [ -n "${2:-}" ] && [ "$_x" = "$GRUB_D_FILE" ] && continue
                . "$_x" >/dev/null 2>&1
            done
            printf '%s' "${GRUB_CMDLINE_LINUX_DEFAULT:-}"
        )
    }

    if [ ! -f "$GRUB_FILE" ]; then
        warn "${GRUB_FILE} does not exist; skipping the kernel parameters:"
        warn "  ${GRUB_PARAMS[*]}"
        warn "this is normal on a system that does not boot via GRUB."
    else
        GRUB_LIVE="$(grub_effective_cmdline "$GRUB_FILE")"
        GRUB_MISSING="$(grub_missing_params "$GRUB_LIVE")"

        if [ -z "$GRUB_MISSING" ]; then
            # Every managed parameter is already on the command line, whether an
            # earlier run put it there or the operator did. Touch nothing.
            skip "kernel parameters already set (${GRUB_PARAMS[*]})"
        else
            log "kernel parameters to add: ${GRUB_MISSING}"

            if [ ! -f "${GRUB_FILE}.opensrthub.bak" ]; then
                log "backing up ${GRUB_FILE} to ${GRUB_FILE}.opensrthub.bak"
                $SUDO cp "$GRUB_FILE" "${GRUB_FILE}.opensrthub.bak"
            fi

            GRUB_TMP="$(mktemp)"
            awk -v b="$GRUB_BEGIN" -v e="$GRUB_END" '
                $0 == b { skip = 1; next }
                $0 == e { skip = 0; next }
                skip == 0 { print }
            ' "$GRUB_FILE" > "$GRUB_TMP"

            # Base value from this file alone with our block already removed, so
            # repeated runs never compound. Sourcing is how grub reads it too.
            GRUB_BASE="$(
                set +eu
                # shellcheck disable=SC1090
                . "$GRUB_TMP" >/dev/null 2>&1
                printf '%s' "${GRUB_CMDLINE_LINUX_DEFAULT:-}"
            )"
            GRUB_NEW="$(grub_apply_params "$GRUB_BASE")"

            if [ -n "$GRUB_BASE" ]; then
                log "existing parameters in ${GRUB_FILE}: ${GRUB_BASE}"
            else
                log "no existing parameters in ${GRUB_FILE}"
            fi
            log "setting GRUB_CMDLINE_LINUX_DEFAULT=\"${GRUB_NEW}\""

            {
                printf '%s\n' "$GRUB_BEGIN"
                printf '# Kernel parameters required by opensrthub:\n'
                for _p in "${GRUB_PARAMS[@]}"; do
                    printf '#   %s\n' "$_p"
                done
                printf '#\n'
                printf '# This assignment intentionally overrides any GRUB_CMDLINE_LINUX_DEFAULT\n'
                printf '# set earlier in this file: grub-mkconfig sources it as shell, so the last\n'
                printf '# assignment wins. Any parameters that line had are carried over above.\n'
                printf '# To revert, delete this whole block and run: sudo update-grub\n'
                printf 'GRUB_CMDLINE_LINUX_DEFAULT="%s"\n' "$GRUB_NEW"
                printf '%s\n' "$GRUB_END"
            } >> "$GRUB_TMP"

            $SUDO cp "$GRUB_TMP" "$GRUB_FILE"
            $SUDO chmod 644 "$GRUB_FILE"
            rm -f "$GRUB_TMP"

            # Does a drop-in in grub.d override what we just wrote? Remove any
            # stale copy of ours first so the test reflects the other files.
            if [ -f "$GRUB_D_FILE" ]; then
                $SUDO rm -f "$GRUB_D_FILE"
            fi
            GRUB_EFFECTIVE="$(grub_effective_cmdline "$GRUB_FILE")"
            GRUB_STILL_MISSING="$(grub_missing_params "$GRUB_EFFECTIVE")"
            if [ -z "$GRUB_STILL_MISSING" ]; then
                log "no drop-in in ${GRUB_D_DIR} overrides it"
            else
                warn "a drop-in in ${GRUB_D_DIR} reassigns GRUB_CMDLINE_LINUX_DEFAULT"
                warn "and would discard: ${GRUB_STILL_MISSING}"
                warn "(its effective value: \"${GRUB_EFFECTIVE}\")"
                GRUB_D_NEW="$(grub_apply_params "$GRUB_EFFECTIVE")"
                log "adding ${GRUB_D_FILE} to win the ordering: \"${GRUB_D_NEW}\""
                $SUDO mkdir -p "$GRUB_D_DIR"
                $SUDO tee "$GRUB_D_FILE" >/dev/null <<GRUBD
# Managed by opensrthub setup.sh - do not edit.
#
# grub-mkconfig sources /etc/default/grub first and then this directory in
# sorted order, so a lower-numbered drop-in here was overriding the parameters
# set in /etc/default/grub. This file sorts last and restores them, carrying
# over the parameters that drop-in set.
#
# To revert, delete this file and run: sudo update-grub
GRUB_CMDLINE_LINUX_DEFAULT="${GRUB_D_NEW}"
GRUBD
            fi

            # Regenerate grub.cfg, or the parameters never reach the kernel.
            GRUB_REGENERATED=0
            if command -v update-grub >/dev/null 2>&1; then
                log "regenerating the boot configuration (update-grub)"
                if $SUDO update-grub >/dev/null 2>&1; then
                    GRUB_REGENERATED=1
                else
                    warn "update-grub failed. ${GRUB_FILE} was written, but the boot"
                    warn "configuration was NOT regenerated, so the parameters will not"
                    warn "take effect. Run 'sudo update-grub' by hand to see the error."
                fi
            elif command -v grub-mkconfig >/dev/null 2>&1; then
                log "regenerating the boot configuration (grub-mkconfig)"
                if $SUDO grub-mkconfig -o "$GRUB_CFG" >/dev/null 2>&1; then
                    GRUB_REGENERATED=1
                else
                    warn "grub-mkconfig failed; the parameters will not take effect."
                    warn "run 'sudo grub-mkconfig -o ${GRUB_CFG}' to see the error."
                fi
            else
                warn "neither update-grub nor grub-mkconfig is available, so the boot"
                warn "configuration cannot be regenerated and the parameters will not"
                warn "take effect. ${GRUB_FILE} has still been updated for a later rebuild."
            fi

            # Authoritative check: the generated boot config is what the kernel is
            # actually handed, so confirm each parameter is in it rather than
            # assuming our edits had the intended effect.
            if [ "$GRUB_REGENERATED" -eq 1 ]; then
                GRUB_CFG_BAD=""
                for _p in "${GRUB_PARAMS[@]}"; do
                    if ! $SUDO grep -q -- "$_p" "$GRUB_CFG" 2>/dev/null; then
                        GRUB_CFG_BAD="${GRUB_CFG_BAD:+$GRUB_CFG_BAD }$_p"
                    fi
                done
                if [ -z "$GRUB_CFG_BAD" ]; then
                    ok "confirmed in ${GRUB_CFG}: ${GRUB_PARAMS[*]}"
                    REBOOT_REQUIRED=1
                else
                    warn "${GRUB_CFG} was regenerated but is missing: ${GRUB_CFG_BAD}"
                    warn "something else in the GRUB configuration is overriding them;"
                    warn "check ${GRUB_D_DIR}/ and /etc/grub.d/ before relying on this."
                fi
            fi
        fi
    fi

    # --- unattended upgrades ------------------------------------------------
    # An automatic upgrade that restarts docker or node mid-stream is not
    # acceptable on an appliance, so both the service and the apt periodic
    # counters are turned off.
    disable_unit unattended-upgrades.service
    log "disabling apt periodic update/upgrade counters"
    $SUDO tee /etc/apt/apt.conf.d/20auto-upgrades >/dev/null <<'APTCONF'
// Managed by opensrthub setup.sh
// Background package activity is disabled: an unattended upgrade that restarts
// docker or node would interrupt live streams. Apply updates deliberately.
APT::Periodic::Update-Package-Lists "0";
APT::Periodic::Download-Upgradeable-Packages "0";
APT::Periodic::Unattended-Upgrade "0";
APT::Periodic::AutocleanInterval "0";
APTCONF

    # --- apt-daily timers ---------------------------------------------------
    # The timers are what actually fire; the services are masked too so nothing
    # can trigger them by hand or by dependency.
    disable_unit apt-daily.timer
    disable_unit apt-daily.service
    disable_unit apt-daily-upgrade.timer
    disable_unit apt-daily-upgrade.service

    # --- MOTD ---------------------------------------------------------------
    # Removing the execute bit is enough: pam_motd runs the scripts in this
    # directory and prints nothing when none of them are executable. Left
    # /etc/pam.d alone on purpose - a bad edit there locks you out over SSH.
    if [ -d /etc/update-motd.d ]; then
        log "disabling the dynamic MOTD scripts in /etc/update-motd.d"
        $SUDO find /etc/update-motd.d -type f -exec chmod -x {} + 2>/dev/null || true
    fi
    if [ -f /etc/default/motd-news ]; then
        log "disabling motd-news"
        $SUDO sed -i 's/^ENABLED=.*/ENABLED=0/' /etc/default/motd-news
    fi
    disable_unit motd-news.timer
    disable_unit motd-news.service
    if [ -f /etc/motd ] && [ -s /etc/motd ]; then
        log "clearing the static /etc/motd"
        $SUDO truncate -s 0 /etc/motd
    fi

    # --- crash reporting ----------------------------------------------------
    # apport collects crash dumps and whoopsie uploads them to Canonical's error
    # tracker; kerneloops does the same for kernel oopses. A core dump from this
    # box can contain whatever was in memory at the time, which for opensrthub
    # includes SRT stream passphrases - so nothing here should be leaving the
    # machine on its own.
    #
    # Note the .path and .socket units: they are activators, so masking only the
    # services would leave something able to start them again.
    log "disabling crash reporting and crash upload"
    for unit in apport.service \
                apport-autoreport.service \
                apport-autoreport.path \
                apport-forward.socket \
                whoopsie.service \
                kerneloops.service; do
        disable_unit "$unit"
    done

    if [ -f /etc/default/apport ]; then
        log "setting enabled=0 in /etc/default/apport"
        if grep -qE '^[[:space:]]*enabled=' /etc/default/apport; then
            $SUDO sed -i 's/^[[:space:]]*enabled=.*/enabled=0/' /etc/default/apport
        else
            echo 'enabled=0' | $SUDO tee -a /etc/default/apport >/dev/null
        fi
    fi

    if [ "$PURGE_TELEMETRY" -eq 1 ]; then
        # Only name packages that are actually installed; apt-get purge fails on
        # ones it has never heard of.
        APPORT_PKGS=""
        for pkg in apport apport-symptoms apport-gtk whoopsie whoopsie-preferences \
                   kerneloops ubuntu-report popularity-contest; do
            if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
                APPORT_PKGS="${APPORT_PKGS:+$APPORT_PKGS }$pkg"
            fi
        done
        if [ -n "$APPORT_PKGS" ]; then
            log "purging: ${APPORT_PKGS}"
            # shellcheck disable=SC2086
            $SUDO_E apt-get purge -y $APPORT_PKGS \
                || warn "purge failed; the units above are still masked, so crash reporting stays off."
        else
            log "no crash reporting packages are installed"
        fi
    else
        log "packages left in place (pass --purge-telemetry to remove them)"
    fi

    # --- outbound telemetry ---------------------------------------------------
    # popularity-contest submits the installed package list to Canonical weekly;
    # ubuntu-report submits a hardware and install survey. Both are described as
    # anonymous, but an appliance should not be originating traffic to third
    # parties at all, and a package list is itself a disclosure about what the
    # machine is and how it is configured.
    log "disabling outbound telemetry"

    # popularity-contest: units where they exist, plus the config and the
    # cron.daily script that actually does the submitting.
    for unit in popularity-contest.service popularity-contest.timer; do
        disable_unit "$unit"
    done
    if [ -f /etc/popularity-contest.conf ]; then
        if grep -qE '^[[:space:]]*PARTICIPATE=' /etc/popularity-contest.conf; then
            $SUDO sed -i 's/^[[:space:]]*PARTICIPATE=.*/PARTICIPATE="no"/' /etc/popularity-contest.conf
        else
            echo 'PARTICIPATE="no"' | $SUDO tee -a /etc/popularity-contest.conf >/dev/null
        fi
        log "set PARTICIPATE=\"no\" in /etc/popularity-contest.conf"
    fi
    if [ -x /etc/cron.daily/popularity-contest ]; then
        # run-parts skips anything without the execute bit, so this stops the
        # submission without deleting a packaged file.
        $SUDO chmod -x /etc/cron.daily/popularity-contest
        log "removed the execute bit from /etc/cron.daily/popularity-contest"
    fi

    # ubuntu-report ships no service and no config file - it is a CLI invoked by
    # the installer and by initial-setup - so there is nothing to mask. Dropping
    # the execute bit is the only reversible way to stop it running. Note that a
    # package upgrade restores it; --purge-telemetry removes it outright instead.
    if [ -x /usr/bin/ubuntu-report ]; then
        $SUDO chmod -x /usr/bin/ubuntu-report
        log "removed the execute bit from /usr/bin/ubuntu-report"
        log "note: a package upgrade will restore it - use --purge-telemetry to remove it"
    fi

    # --- CPU frequency governor -----------------------------------------------
    # cpufreq.default_governor=performance on the kernel command line only sets
    # the governor each cpufreq policy *starts* with. Two things in userspace then
    # overwrite it late in boot, and the last writer wins:
    #
    #   ondemand.service  shipped by systemd itself (/lib/systemd/system), runs
    #                     /lib/systemd/set-cpufreq, which picks "interactive" if
    #                     available and otherwise "ondemand", for every CPU.
    #                     Type=idle, so it runs well after the kernel default is
    #                     applied. It has ConditionVirtualization=no, so it is not
    #                     a factor on a VM.
    #
    #   cpufrequtils      its init script carries GOVERNOR="ondemand" as a built-in
    #                     default and only sources /etc/default/cpufrequtils if
    #                     that file exists. We install this package, so with no
    #                     such file it was actively setting ondemand.
    #
    # Masking the first and configuring the second makes the result deterministic
    # instead of depending on the kernel default going unchallenged.
    disable_unit ondemand.service

    log "setting GOVERNOR=performance in /etc/default/cpufrequtils"
    $SUDO tee /etc/default/cpufrequtils >/dev/null <<'CPUFREQCONF'
# Managed by opensrthub setup.sh
#
# Without this file the cpufrequtils init script falls back to its built-in
# GOVERNOR="ondemand", which overrides cpufreq.default_governor=performance from
# the kernel command line. 0 for the speed limits means "do not constrain".
ENABLE="true"
GOVERNOR="performance"
MAX_SPEED="0"
MIN_SPEED="0"
CPUFREQCONF

    # Apply now as well, so it does not wait for the reboot.
    if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
        GOV_AVAIL="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null || true)"
        case " $GOV_AVAIL " in
            *" performance "*)
                GOV_SET=0
                for gov_file in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
                    [ -e "$gov_file" ] || continue
                    if echo performance | $SUDO tee "$gov_file" >/dev/null 2>&1; then
                        GOV_SET=$((GOV_SET + 1))
                    fi
                done
                if [ "$GOV_SET" -gt 0 ]; then
                    ok "governor set to performance on ${GOV_SET} CPU(s)"
                else
                    warn "could not write scaling_governor; the setting will apply at the next boot."
                fi
                ;;
            *)
                warn "the performance governor is not offered by this cpufreq driver."
                warn "available: ${GOV_AVAIL:-none}"
                ;;
        esac
    else
        log "no cpufreq sysfs interface here (virtualised host?); nothing to apply now"
    fi

    # --- sshd hardening -------------------------------------------------------
    # sshd_config takes the FIRST value seen for each keyword, and the main file
    # Includes /etc/ssh/sshd_config.d/*.conf near its top, so a drop-in there wins
    # over anything below the Include. The 10- prefix also puts this ahead of other
    # drop-ins (a cloud image's 50-cloud-init.conf, say) for any keyword both set.
    # This is the opposite precedence to /etc/default/grub, which is sourced as
    # shell and so takes the last assignment.
    SSHD_CONFIG="/etc/ssh/sshd_config"
    SSHD_DROPIN_DIR="/etc/ssh/sshd_config.d"
    SSHD_DROPIN="${SSHD_DROPIN_DIR}/10-opensrthub-hardening.conf"
    SSHD_BIN="$(command -v sshd || echo /usr/sbin/sshd)"

    if [ ! -f "$SSHD_CONFIG" ]; then
        warn "${SSHD_CONFIG} does not exist; skipping sshd hardening."
    else
        # PermitRootLogin no locks out anyone whose only access is as root.
        if [ -n "${SSH_CONNECTION:-}" ] && [ "$(id -un)" = "root" ] && [ -z "${SUDO_USER:-}" ]; then
            warn "you are connected over SSH as root, and this sets PermitRootLogin no."
            warn "make sure a non-root account with sudo can log in before you"
            warn "disconnect, or you will be locked out of this machine."
        fi

        $SUDO mkdir -p "$SSHD_DROPIN_DIR"

        # A drop-in is only read if the Include is actually there. Without this
        # check the hardening would silently do nothing on a config that predates
        # the drop-in directory.
        if ! grep -qE '^[[:space:]]*Include[[:space:]]+/etc/ssh/sshd_config\.d/\*\.conf' "$SSHD_CONFIG"; then
            log "adding the sshd_config.d Include to ${SSHD_CONFIG}"
            [ -f "${SSHD_CONFIG}.opensrthub.bak" ] || $SUDO cp "$SSHD_CONFIG" "${SSHD_CONFIG}.opensrthub.bak"
            # At the top, because the first value for a keyword wins.
            SSHD_TMP="$(mktemp)"
            {
                printf 'Include %s/*.conf\n' "$SSHD_DROPIN_DIR"
                cat "$SSHD_CONFIG"
            } > "$SSHD_TMP"
            $SUDO cp "$SSHD_TMP" "$SSHD_CONFIG"
            rm -f "$SSHD_TMP"
        fi

        log "writing ${SSHD_DROPIN}"
        # Keep any previous copy so a bad config can be put back.
        if [ -f "$SSHD_DROPIN" ]; then
            $SUDO cp "$SSHD_DROPIN" "${SSHD_DROPIN}.prev"
        fi
        $SUDO tee "$SSHD_DROPIN" >/dev/null <<'SSHDCONF'
# Managed by opensrthub setup.sh - do not edit.
#
# sshd_config uses the FIRST value found for each keyword, and the main config
# Includes this directory near its top, so these win over the settings below it.
#
# To revert: delete this file, then
#   sudo sshd -t && sudo systemctl reload ssh

# AEAD and CTR only. No CBC.
Ciphers chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr

# No SHA1 exchanges, no diffie-hellman-group1/group14-sha1.
KexAlgorithms curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp521,ecdh-sha2-nistp384,ecdh-sha2-nistp256,diffie-hellman-group-exchange-sha256

# Encrypt-then-MAC first. No MD5, no SHA1, no truncated -96 variants.
# Note these apply only to the CTR ciphers above: the AEAD ciphers
# (chacha20-poly1305, *-gcm) carry their own integrity and ignore MACs.
MACs hmac-sha2-512-etm@openssh.com,hmac-sha2-256-etm@openssh.com,hmac-sha2-512,hmac-sha2-256

PermitRootLogin no
PermitEmptyPasswords no
IgnoreRhosts yes
HostbasedAuthentication no
LoginGraceTime 60
ClientAliveInterval 300
ClientAliveCountMax 3
MaxAuthTries 4
SSHDCONF
        $SUDO chmod 644 "$SSHD_DROPIN"

        # Validate before anything reloads. A config that sshd rejects would, on
        # the next restart, leave the machine with no sshd at all - so on failure
        # put back what was there and stop.
        # -f names the file explicitly: sshd -t would otherwise default to
        # /etc/ssh/sshd_config regardless of what we just edited.
        if $SUDO "$SSHD_BIN" -t -f "$SSHD_CONFIG" 2>/tmp/opensrthub-sshd-test.$$; then
            ok "sshd configuration validates"
            rm -f "${SSHD_DROPIN}.prev" /tmp/opensrthub-sshd-test.$$
            # Reload so it applies to new connections. Existing sessions are
            # unaffected. On a socket-activated sshd there is nothing running to
            # reload and each new connection reads the config anyway, so a failure
            # here is not an error.
            if $SUDO systemctl reload ssh >/dev/null 2>&1 ||
               $SUDO systemctl reload sshd >/dev/null 2>&1; then
                log "reloaded sshd (existing sessions are unaffected)"
            else
                log "sshd not reloaded (socket-activated or not running); new"
                log "connections will pick up the configuration regardless"
            fi
        else
            warn "sshd rejected the new configuration:"
            $SUDO sed 's/^/    /' /tmp/opensrthub-sshd-test.$$ >&2 || true
            rm -f /tmp/opensrthub-sshd-test.$$
            if [ -f "${SSHD_DROPIN}.prev" ]; then
                $SUDO mv "${SSHD_DROPIN}.prev" "$SSHD_DROPIN"
                warn "restored the previous ${SSHD_DROPIN}"
            else
                $SUDO rm -f "$SSHD_DROPIN"
                warn "removed ${SSHD_DROPIN}"
            fi
            die "sshd hardening failed validation and was rolled back; sshd is untouched."
        fi
    fi

    # --- core dump retention -------------------------------------------------
    # A core from a video application is large, and an appliance that crashloops
    # would otherwise fill its own disk and take the service down with it.
    # systemd-tmpfiles-clean.timer is active by default and runs daily, so a
    # tmpfiles rule ages them out without adding a cron job.
    log "expiring core dumps in ${DATA_DIR}/cores after 14 days"
    $SUDO tee /etc/tmpfiles.d/opensrthub-cores.conf >/dev/null <<TMPFILES
# Managed by opensrthub setup.sh
# Keep the directory; delete cores untouched for 14 days.
# Cleaned by systemd-tmpfiles-clean.timer (daily).
d ${DATA_DIR}/cores 1777 root root 14d
TMPFILES

    # --- sysctl -------------------------------------------------------------
    # Written as a delimited block in /etc/sysctl.conf so re-running replaces it
    # instead of appending duplicates. On Ubuntu /etc/sysctl.d/99-sysctl.conf is
    # a symlink to this file, so it is applied last and wins over the drop-ins.
    SYSCTL_FILE="/etc/sysctl.conf"
    SYSCTL_BEGIN="# BEGIN opensrthub (managed by setup.sh - do not edit this block)"
    SYSCTL_END="# END opensrthub"

    if [ -f "$SYSCTL_FILE" ] && [ ! -f "${SYSCTL_FILE}.opensrthub.bak" ]; then
        log "backing up ${SYSCTL_FILE} to ${SYSCTL_FILE}.opensrthub.bak"
        $SUDO cp "$SYSCTL_FILE" "${SYSCTL_FILE}.opensrthub.bak"
    fi

    SYSCTL_TMP="$(mktemp)"
    # Strip any previously managed block, keep everything else verbatim.
    if [ -f "$SYSCTL_FILE" ]; then
        awk -v b="$SYSCTL_BEGIN" -v e="$SYSCTL_END" '
            $0 == b { skip = 1; next }
            $0 == e { skip = 0; next }
            skip == 0 { print }
        ' "$SYSCTL_FILE" > "$SYSCTL_TMP"
    fi

    log "writing the opensrthub sysctl block to ${SYSCTL_FILE}"
    {
        printf '%s\n' "$SYSCTL_BEGIN"
        cat <<'SYSCTLBLOCK'
# --- network ---
# Survive SYN floods without dropping legitimate connection attempts.
net.ipv4.tcp_syncookies = 1

# Ignore ICMP redirects: on a box that routes stream traffic between
# interfaces, an accepted redirect is an unauthenticated route change.
net.ipv4.conf.all.accept_redirects = 0
net.ipv4.conf.default.accept_redirects = 0
net.ipv6.conf.all.accept_redirects = 0
net.ipv6.conf.default.accept_redirects = 0

# IPv6 is not used for SRT/UDP routing here; disabling it removes a second
# address family from the attack surface and from interface configuration.
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
net.ipv6.conf.lo.disable_ipv6 = 1

# --- kernel ---
# Full address space randomisation (stack, mmap, brk).
kernel.randomize_va_space = 2

# Append .PID to core file names so concurrent stream crashes don't overwrite
# each other's cores.
kernel.core_uses_pid = 1

SYSCTLBLOCK
        # Emitted separately so the path stays in step with DATA_DIR.
        #
        # Cores go to a fixed directory rather than the crashing process's working
        # directory. apport otherwise owns this setting (it installs
        # "|/usr/share/apport/apport ..." here), so with apport disabled and this
        # left alone every core would be handed to a program that is no longer
        # running - producing no core at all and quietly defeating fs.suid_dumpable.
        #
        # The path is resolved in the crashing process's own mount namespace, and
        # server.js bind-mounts DATA_DIR into every stream container at the same
        # path, so a core from srthub inside a container lands here on the host and
        # survives the container being removed.
        #
        # %e executable name, %p pid, %t dump time - unique even when a restarted
        # container reuses a pid. %p makes kernel.core_uses_pid redundant, harmlessly.
        printf '\n# --- core dumps ---\n'
        printf 'kernel.core_pattern = %s/cores/core.%%e.%%p.%%t\n' "$DATA_DIR"
        cat <<'SYSCTLBLOCK2'

# Allow core dumps from privileged/setuid processes. srthub runs as root inside
# its container, and without this a crash produces no core at all, which makes
# stream faults very hard to diagnose.
#
# NOTE: this is a deliberate loosening of a hardening default. A core from a
# privileged process can contain secrets held in memory - for opensrthub that
# means SRT stream passphrases. Cores are owner-read-only, but if you do not
# need crash diagnostics, set this to 0.
fs.suid_dumpable = 1
SYSCTLBLOCK2
        printf '%s\n' "$SYSCTL_END"
    } >> "$SYSCTL_TMP"

    $SUDO cp "$SYSCTL_TMP" "$SYSCTL_FILE"
    $SUDO chmod 644 "$SYSCTL_FILE"
    rm -f "$SYSCTL_TMP"

    # Non-fatal: the net.ipv6.* keys do not exist if the ipv6 module was never
    # loaded, and sysctl exits non-zero for any key it cannot set.
    if $SUDO sysctl -q --system 2>/dev/null; then
        ok "sysctl settings applied"
    else
        warn "sysctl --system reported an error; re-run 'sudo sysctl --system' to see which key failed."
        warn "this is expected if IPv6 was already disabled at the kernel level."
    fi

    ok "host tuning applied (a reboot makes all of it take effect cleanly)"
fi

#-----------------------------------------------------------------------------
# service registration
#-----------------------------------------------------------------------------
phase "Registering the ${SERVICE_NAME} service (${SERVICE_MANAGER})"

case "$SERVICE_MANAGER" in
    systemd)
        # The old instructions ran this under pm2. If that is still in place it
        # holds port 8080, so the unit below would crashloop on EADDRINUSE - and
        # pm2's boot hook would bring its copy back after a reboot as well. Hand
        # over before installing the unit.
        if command -v pm2 >/dev/null 2>&1 && \
           $SUDO pm2 describe "$SERVICE_NAME" >/dev/null 2>&1; then
            log "taking over from pm2: removing its '${SERVICE_NAME}' process"
            $SUDO pm2 delete "$SERVICE_NAME" >/dev/null 2>&1 || true
            # Re-save so pm2's boot hook no longer replays it. Leaving the hook
            # itself alone: it may be running other apps.
            $SUDO pm2 save --force >/dev/null 2>&1 || true
            log "if opensrthub was the only thing pm2 managed, you can also run:"
            log "  sudo pm2 unstartup systemd"
        fi
        install_file "${REPO_DIR}/packaging/${SERVICE_NAME}.service" "/etc/systemd/system/"
        $SUDO systemctl daemon-reload
        $SUDO systemctl enable "${SERVICE_NAME}.service"
        $SUDO systemctl restart "${SERVICE_NAME}.service"
        ok "${SERVICE_NAME}.service enabled and started"
        ;;
    pm2)
        # Mirror image of the above: if the systemd unit is in place it holds the
        # port, so stand it down before handing the app to pm2.
        if [ -f "/etc/systemd/system/${SERVICE_NAME}.service" ]; then
            log "taking over from systemd: stopping and disabling ${SERVICE_NAME}.service"
            $SUDO systemctl disable --now "${SERVICE_NAME}.service" >/dev/null 2>&1 || true
        fi
        install_file "${REPO_DIR}/packaging/ecosystem.config.js" "${APP_DIR}/"
        # 'cwd' in the ecosystem file is what makes this work from any
        # directory; the old instructions required you to be in /var/app.
        $SUDO pm2 delete "$SERVICE_NAME" >/dev/null 2>&1 || true
        $SUDO pm2 start "${APP_DIR}/ecosystem.config.js"
        $SUDO pm2 save
        # 'pm2 save' alone does NOT survive a reboot; 'pm2 startup' installs the
        # boot hook that replays the saved process list.
        $SUDO pm2 startup systemd -u root --hp /root
        ok "pm2 process '${SERVICE_NAME}' started, saved, and enabled at boot"
        ;;
    none)
        bypass "service registration (--service=none)"
        log "to run the app manually: cd ${APP_DIR} && sudo node server.js"
        ;;
esac

#-----------------------------------------------------------------------------
# ownership cleanup and verification
#-----------------------------------------------------------------------------
phase "Finishing up"

# If the build ran under sudo/root, hand the checkout back to the human who owns
# it so 'git status' and subsequent non-root builds still work.
if [ -n "$INVOKING_USER" ] && [ "$INVOKING_USER" != "root" ]; then
    log "restoring ownership of ${REPO_DIR} to ${INVOKING_USER}"
    $SUDO chown -R "${INVOKING_USER}:${INVOKING_USER}" "$REPO_DIR" 2>/dev/null || \
        warn "could not restore ownership of ${REPO_DIR}"
fi

if verify_install; then VERIFY_STATUS=0; else VERIFY_STATUS=1; fi

if [ "${REBOOT_REQUIRED:-0}" -eq 1 ]; then
    echo
    echo "${C_YELLOW}${C_BOLD}  A reboot is required.${C_RESET}"
    echo "${C_YELLOW}  These kernel parameters were added and only take effect on the next boot:"
    echo "    ${GRUB_PARAMS[*]}"
    echo "  Everything else is already active.${C_RESET}"
    echo "${C_YELLOW}  After rebooting, confirm streams still start and check:${C_RESET}"
    echo "${C_YELLOW}    cat /proc/cmdline                                   # expect both parameters${C_RESET}"
    echo "${C_YELLOW}    cat /sys/module/apparmor/parameters/enabled          # expect N${C_RESET}"
    echo "${C_YELLOW}    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor  # expect performance${C_RESET}"
    echo "${C_YELLOW}    grep . /sys/devices/system/cpu/vulnerabilities/*      # expect Vulnerable${C_RESET}"
    echo "${C_YELLOW}    sudo docker run --rm hello-world                     # expect success${C_RESET}"
fi

if [ "${GENERATED_PASSWORD:-0}" -eq 1 ]; then
    echo
    echo "${C_YELLOW}${C_BOLD}  Generated admin password: ${ADMIN_PASSWORD}${C_RESET}"
    echo "${C_YELLOW}  Save it now. It is stored in ${DATA_DIR}/users.json.${C_RESET}"
fi

echo
echo "=== opensrthub install finished $(date -Is) ==="
exit "$VERIFY_STATUS"
