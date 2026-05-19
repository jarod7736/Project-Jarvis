#!/usr/bin/env bash
# morning-brief deploy helper for lobsterboy.
#
# Subcommands:
#   survey   read-only checks; prints recommended next steps. Default.
#   install  create venv, pip install, write systemd unit+timer.
#   test     run the one-shot in dry-run mode (no notifier send).
#   fire     run for real, right now (skips the timer).
#
# Tunable env vars:
#   VENV_DIR          default <package_dir>/.venv
#   SYSTEMD_UNIT_DIR  default /etc/systemd/system

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${PKG_DIR}/../.." && pwd)"
VENV_DIR="${VENV_DIR:-${PKG_DIR}/.venv}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
RUN_USER="${RUN_USER:-${SUDO_USER:-${USER:-$(whoami)}}}"

COLOR_GREEN=$'\033[0;32m'
COLOR_YELLOW=$'\033[0;33m'
COLOR_RED=$'\033[0;31m'
COLOR_RESET=$'\033[0m'

ok()    { printf '%s✓%s %s\n'  "$COLOR_GREEN"  "$COLOR_RESET" "$*"; }
warn()  { printf '%s!%s %s\n'  "$COLOR_YELLOW" "$COLOR_RESET" "$*"; }
fail()  { printf '%s✗%s %s\n'  "$COLOR_RED"    "$COLOR_RESET" "$*"; }
info()  { printf '  %s\n' "$*"; }
header(){ printf '\n%s── %s ──%s\n' "$COLOR_YELLOW" "$*" "$COLOR_RESET"; }

cmd_survey() {
    header "Environment"
    info "Running as:    $(whoami)"
    info "Package dir:   ${PKG_DIR}"
    info "Project root:  ${PROJECT_ROOT}"
    info "Venv target:   ${VENV_DIR}"
    info "Unit dir:      ${SYSTEMD_UNIT_DIR}"

    header "Prerequisites"
    if command -v python3 >/dev/null; then
        local pyver
        pyver=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
        if python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)'; then
            ok   "python3: ${pyver}"
        else
            fail "python3: ${pyver} — need >= 3.11"
        fi
    else
        fail "python3: not installed"
    fi

    if command -v systemctl >/dev/null; then
        ok "systemctl: present"
    else
        warn "systemctl: not found — timer install will be skipped"
    fi

    header "Upstreams"
    if curl -fsS http://localhost:8080/healthz >/dev/null 2>&1; then
        ok "oc-personal reachable @ http://localhost:8080"
    else
        warn "oc-personal not reachable @ http://localhost:8080 — install/start it first"
    fi
    if curl -fsS http://localhost:8081/healthz >/dev/null 2>&1; then
        ok "jarvis-notifier reachable @ http://localhost:8081"
    else
        warn "jarvis-notifier not reachable @ http://localhost:8081 — install/start it first"
    fi

    header "Existing install"
    if [ -d "${VENV_DIR}" ] && [ -x "${VENV_DIR}/bin/python" ]; then
        ok "Venv: ${VENV_DIR}"
        if "${VENV_DIR}/bin/python" -c 'import morning_brief' 2>/dev/null; then
            ok "    morning_brief importable"
        else
            warn "    morning_brief NOT importable — re-run install"
        fi
    else
        info "Venv: not yet created"
    fi

    if [ -e "${SYSTEMD_UNIT_DIR}/morning-brief.service" ]; then
        ok "Systemd: morning-brief.service installed"
    else
        info "Systemd: morning-brief.service not installed yet"
    fi
    if [ -e "${SYSTEMD_UNIT_DIR}/morning-brief.timer" ]; then
        ok "Systemd: morning-brief.timer installed"
        if systemctl is-active --quiet morning-brief.timer 2>/dev/null; then
            ok "    timer active"
        else
            warn "    timer present but not active (run: sudo systemctl enable --now morning-brief.timer)"
        fi
    else
        info "Systemd: morning-brief.timer not installed yet"
    fi

    header "Next steps"
    info "  ./deploy.sh install    # venv + systemd unit + timer"
    info "  ./deploy.sh test       # dry-run (prints brief, does not send)"
    info "  ./deploy.sh fire       # one real run now"
    info "  sudo systemctl enable --now morning-brief.timer"
}

cmd_install() {
    header "Install"
    info "User:        ${RUN_USER}"
    info "Venv:        ${VENV_DIR}"
    info "Unit dir:    ${SYSTEMD_UNIT_DIR}"
    echo

    # ── Venv ────────────────────────────────────────────────────────────
    if [ ! -d "${VENV_DIR}" ]; then
        info "Creating venv at ${VENV_DIR} ..."
        python3 -m venv "${VENV_DIR}"
    fi
    info "Installing package (pip install -e .) ..."
    "${VENV_DIR}/bin/pip" install --upgrade pip
    "${VENV_DIR}/bin/pip" install -e "${PKG_DIR}"
    ok "Venv ready"

    # ── Systemd unit + timer ────────────────────────────────────────────
    for unit in morning-brief.service morning-brief.timer; do
        local src="${PKG_DIR}/systemd/${unit}"
        local dst="${SYSTEMD_UNIT_DIR}/${unit}"
        local rendered
        rendered=$(mktemp)
        sed \
            -e "s|__RUN_USER__|${RUN_USER}|g" \
            -e "s|__PROJECT_ROOT__|${PROJECT_ROOT}|g" \
            "${src}" > "${rendered}"

        if grep -q '__[A-Z_]*__' "${rendered}"; then
            fail "Unrendered placeholder(s) in ${unit}:"
            grep -n '__[A-Z_]*__' "${rendered}"
            rm -f "${rendered}"
            return 1
        fi

        if [ -f "${dst}" ] && cmp -s "${rendered}" "${dst}"; then
            ok "${unit} already current"
        else
            info "Installing ${unit} (needs sudo) ..."
            sudo install -m 0644 "${rendered}" "${dst}"
            ok "${unit} installed"
        fi
        rm -f "${rendered}"
    done
    sudo systemctl daemon-reload
    ok "systemctl daemon-reload done"

    echo
    info "To enable the daily 08:00 fire:"
    info "  sudo systemctl enable --now morning-brief.timer"
    info "To test once now (real send):"
    info "  ./deploy.sh fire"
    info "To dry-run (no send):"
    info "  ./deploy.sh test"
}

cmd_test() {
    header "Dry-run"
    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        fail "Venv not ready — run ./deploy.sh install first"
        return 1
    fi
    MORNING_BRIEF_DRY_RUN=1 "${VENV_DIR}/bin/python" -m morning_brief.main
}

cmd_fire() {
    header "Fire now (real send)"
    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        fail "Venv not ready — run ./deploy.sh install first"
        return 1
    fi
    "${VENV_DIR}/bin/python" -m morning_brief.main
}

main() {
    case "${1:-survey}" in
        survey)  cmd_survey ;;
        install) cmd_install ;;
        test)    cmd_test ;;
        fire)    cmd_fire ;;
        *)
            echo "usage: $0 {survey|install|test|fire}" >&2
            exit 64
            ;;
    esac
}

main "$@"
