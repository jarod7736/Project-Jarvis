#!/usr/bin/env bash
# jarvis-notifier deploy helper for lobsterboy.
#
# Subcommands:
#   survey   read-only checks; prints recommended next steps. Default.
#   install  create venv, pip install, write systemd unit, create state dir.
#            Prompts before each destructive step.
#   test     smoke-test the installed service.
#
# Tunable env vars:
#   STATE_DIR         default /var/lib/jarvis-notifier
#   VENV_DIR          default <package_dir>/.venv
#   SYSTEMD_UNIT_DIR  default /etc/systemd/system
#   SECRETS_DIR       default /etc/jarvis-notifier
#
# Same shape as tools/brain-mcp/deploy.sh and tools/oc-personal-runner/
# deploy.sh; intentional, so operators have one mental model for all
# three lobsterboy-side services.

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${PKG_DIR}/../.." && pwd)"
STATE_DIR="${STATE_DIR:-/var/lib/jarvis-notifier}"
SECRETS_DIR="${SECRETS_DIR:-/etc/jarvis-notifier}"
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

confirm() {
    local prompt="${1:-Continue?}"
    read -r -p "$prompt [y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]]
}

cmd_survey() {
    header "Environment"
    info "Running as:    $(whoami)"
    info "Hostname:      $(hostname -f 2>/dev/null || hostname)"
    info "Package dir:   ${PKG_DIR}"
    info "Project root:  ${PROJECT_ROOT}"
    info "State dir:     ${STATE_DIR}"
    info "Secrets dir:   ${SECRETS_DIR}"
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
        warn "systemctl: not found — service install will be skipped"
    fi

    if command -v mosquitto_pub >/dev/null; then
        ok "mosquitto_pub: present (smoke test will work)"
    else
        warn "mosquitto_pub: not installed (apt install mosquitto-clients) — smoke test needs it"
    fi

    header "Existing install"
    if [ -d "${VENV_DIR}" ] && [ -x "${VENV_DIR}/bin/python" ]; then
        ok "Venv: ${VENV_DIR}"
        if "${VENV_DIR}/bin/python" -c 'import jarvis_notifier' 2>/dev/null; then
            ok "    jarvis_notifier importable"
        else
            warn "    jarvis_notifier NOT importable — re-run install"
        fi
    else
        info "Venv: not yet created"
    fi

    if [ -e "${SYSTEMD_UNIT_DIR}/jarvis-notifier.service" ]; then
        ok "Systemd: jarvis-notifier.service installed"
        if systemctl is-active --quiet jarvis-notifier.service 2>/dev/null; then
            ok "    service active"
        else
            warn "    service present but not active"
        fi
    else
        info "Systemd: jarvis-notifier.service not installed yet"
    fi

    if [ -d "${STATE_DIR}" ]; then
        ok "State dir: ${STATE_DIR}"
    else
        info "State dir: not yet created"
    fi

    if [ -f "${SECRETS_DIR}/secrets.env" ]; then
        ok "Secrets env: ${SECRETS_DIR}/secrets.env"
        local perms
        perms=$(stat -c '%a' "${SECRETS_DIR}/secrets.env" 2>/dev/null || echo "?")
        if [ "${perms}" != "600" ] && [ "${perms}" != "640" ]; then
            warn "    perms are ${perms} — recommend chmod 600"
        else
            info "    perms ${perms} ✓"
        fi
    else
        warn "Secrets env: ${SECRETS_DIR}/secrets.env not present"
        info "    create with NOTIFIER_MQTT_PASS, PUSHOVER_TOKEN, PUSHOVER_USER_KEY"
    fi

    header "Next steps"
    info "  ./deploy.sh install    # create venv + state dir + systemd unit"
    info "  ./deploy.sh test       # smoke-test against /healthz"
}

cmd_install() {
    header "Install"
    info "User:        ${RUN_USER}"
    info "State dir:   ${STATE_DIR}"
    info "Secrets dir: ${SECRETS_DIR}"
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

    # ── State dir ───────────────────────────────────────────────────────
    if [ ! -d "${STATE_DIR}" ]; then
        info "Creating ${STATE_DIR} (needs sudo) ..."
        sudo install -d -m 0750 -o "${RUN_USER}" "${STATE_DIR}"
    fi
    ok "State dir ready: ${STATE_DIR}"

    # ── Secrets dir / env template ──────────────────────────────────────
    if [ ! -d "${SECRETS_DIR}" ]; then
        info "Creating ${SECRETS_DIR} (needs sudo) ..."
        sudo install -d -m 0750 -o "${RUN_USER}" "${SECRETS_DIR}"
    fi
    if [ ! -f "${SECRETS_DIR}/secrets.env" ]; then
        warn "Writing template ${SECRETS_DIR}/secrets.env — fill in real values"
        sudo tee "${SECRETS_DIR}/secrets.env" >/dev/null <<EOF
# Secrets for jarvis-notifier. 0600 perms; not in git.
# Uncomment + populate before starting the service.
#NOTIFIER_MQTT_USER=jarvis
#NOTIFIER_MQTT_PASS=replace-me
#PUSHOVER_TOKEN=replace-me
#PUSHOVER_USER_KEY=replace-me
EOF
        sudo chown "${RUN_USER}:${RUN_USER}" "${SECRETS_DIR}/secrets.env"
        sudo chmod 0600 "${SECRETS_DIR}/secrets.env"
    fi
    ok "Secrets env present: ${SECRETS_DIR}/secrets.env"

    # ── Systemd unit ────────────────────────────────────────────────────
    local unit_src="${PKG_DIR}/systemd/jarvis-notifier.service"
    local unit_dst="${SYSTEMD_UNIT_DIR}/jarvis-notifier.service"
    local rendered
    rendered=$(mktemp)
    sed \
        -e "s|__RUN_USER__|${RUN_USER}|g" \
        -e "s|__PROJECT_ROOT__|${PROJECT_ROOT}|g" \
        "${unit_src}" > "${rendered}"

    # Refuse to install if any sentinel survives on a non-comment line.
    # We strip `^\s*#` lines first because the documentation block at the
    # top of the unit intentionally mentions __FOO__ as a literal example
    # — and that would otherwise self-trigger.
    if grep -vE '^\s*#' "${rendered}" | grep -qE '__[A-Z_]+__'; then
        fail "Unrendered placeholder(s) survived sed:"
        grep -nE '__[A-Z_]+__' "${rendered}" | grep -vE ':\s*#' | sed 's/^/    /'
        rm -f "${rendered}"
        return 1
    fi

    if [ -f "${unit_dst}" ] && cmp -s "${rendered}" "${unit_dst}"; then
        ok "Systemd unit already current: ${unit_dst}"
    else
        info "Installing systemd unit (needs sudo) ..."
        sudo install -m 0644 "${rendered}" "${unit_dst}"
        sudo systemctl daemon-reload
        ok "Systemd unit installed: ${unit_dst}"
    fi
    rm -f "${rendered}"

    echo
    info "To start the service:"
    info "  sudo systemctl enable --now jarvis-notifier.service"
    info "Then:"
    info "  ./deploy.sh test"
}

cmd_test() {
    header "Smoke test"
    local port
    port="${NOTIFIER_LISTEN_PORT:-8081}"
    info "Hitting http://localhost:${port}/healthz ..."
    if curl -fsS "http://localhost:${port}/healthz" | python3 -m json.tool; then
        ok "healthz responded"
    else
        fail "healthz unreachable — is the service running?"
        return 1
    fi

    echo
    info "Sending a low-tier test note (writes to log, no device push) ..."
    curl -fsS -X POST "http://localhost:${port}/notify" \
        -H 'Content-Type: application/json' \
        -d '{"text":"deploy.sh test","tier":"low","source":"deploy-test"}' \
        | python3 -m json.tool
    ok "Low-tier dispatch accepted"

    echo
    info "Inspect the log:"
    info "  tail -n5 ${STATE_DIR}/log.jsonl"
}

main() {
    case "${1:-survey}" in
        survey)  cmd_survey ;;
        install) cmd_install ;;
        test)    cmd_test ;;
        *)
            echo "usage: $0 {survey|install|test}" >&2
            exit 64
            ;;
    esac
}

main "$@"
