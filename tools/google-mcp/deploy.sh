#!/usr/bin/env bash
# google-mcp deploy helper.
#
# google-mcp is a stdio child of oc-personal-runner, not its own systemd
# service. This script just creates the venv + installs deps; the runner's
# systemd unit references the venv's Python directly via OC_MCP_SERVERS.
#
# After install, run the one-time OAuth consent flow:
#   .venv/bin/google-mcp-auth
# See src/google_mcp/oauth_cli.py for the SSH-port-forward variant.
#
# Subcommands:
#   survey   read-only checks. Default.
#   install  create venv, pip install. Prompts before destructive steps.
#   test     import smoke + (if a token is present) list calendars.

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${VENV_DIR:-${PKG_DIR}/.venv}"
CREDENTIALS_PATH="${GOOGLE_CREDENTIALS_PATH:-${HOME}/.config/oc-personal/google-credentials.json}"
TOKEN_PATH="${GOOGLE_TOKEN_PATH:-${HOME}/.config/oc-personal/google-token.json}"

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
    info "Package dir:       ${PKG_DIR}"
    info "Venv target:       ${VENV_DIR}"
    info "Credentials path:  ${CREDENTIALS_PATH}"
    info "Token path:        ${TOKEN_PATH}"

    header "Prerequisites"
    if command -v python3 >/dev/null && python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)'; then
        ok "python3: $(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
    else
        fail "python3 >= 3.11 required"
    fi

    header "Existing install"
    if [ -d "${VENV_DIR}" ] && [ -x "${VENV_DIR}/bin/python" ]; then
        ok "Venv: ${VENV_DIR}"
        if "${VENV_DIR}/bin/python" -c 'import google_mcp' 2>/dev/null; then
            ok "    google_mcp importable"
        else
            warn "    google_mcp NOT importable — re-run install"
        fi
    else
        info "Venv: not yet created"
    fi

    header "OAuth artifacts"
    if [ -f "${CREDENTIALS_PATH}" ]; then
        ok "client-secrets: ${CREDENTIALS_PATH}"
    else
        warn "client-secrets: ${CREDENTIALS_PATH} (missing)"
        info "    Create one at https://console.cloud.google.com/apis/credentials"
        info "    (Desktop app OAuth client; enable Gmail + Calendar APIs)."
    fi
    if [ -f "${TOKEN_PATH}" ]; then
        ok "token: ${TOKEN_PATH}"
    else
        info "token: ${TOKEN_PATH} (missing — run google-mcp-auth after install)"
    fi

    header "Next steps"
    info "  ./deploy.sh install"
    info "  .venv/bin/google-mcp-auth     # one-time OAuth consent"
    info "  ./deploy.sh test"
}

cmd_install() {
    header "Install"
    info "Venv: ${VENV_DIR}"
    echo

    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        info "Creating venv: ${VENV_DIR}"
        python3 -m venv "${VENV_DIR}"
        ok "Venv created"
    else
        ok "Venv exists"
    fi

    info "Installing google-mcp package (editable)"
    "${VENV_DIR}/bin/pip" install --upgrade pip >/dev/null
    "${VENV_DIR}/bin/pip" install -e "${PKG_DIR}"
    ok "Package installed"

    "${VENV_DIR}/bin/python" -c 'import google_mcp; from google_mcp import server, gcal, gmail, auth'
    ok "Import smoke test passed"

    echo
    ok "Install complete."
    info "Next: .venv/bin/google-mcp-auth     # one-time OAuth consent"
    info "Then add to oc-personal-runner OC_MCP_SERVERS — see"
    info "tools/oc-personal-runner/systemd/oc-personal.service for the shape."
}

cmd_test() {
    header "Smoke test"
    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        fail "Venv missing — run: ./deploy.sh install"
        exit 1
    fi

    info "Import smoke"
    "${VENV_DIR}/bin/python" -c 'from google_mcp import server, gcal, gmail; print("OK")'

    if [ ! -f "${TOKEN_PATH}" ]; then
        warn "No token at ${TOKEN_PATH} — skipping live API check."
        info "Run: ${VENV_DIR}/bin/google-mcp-auth"
        return
    fi

    info "Listing next 24h of events on primary calendar..."
    "${VENV_DIR}/bin/python" -c "
from google_mcp import gcal
print(gcal.list_events())
" | head -60
}

usage() {
    cat <<USAGE
Usage: $0 [survey|install|test]

  survey   Read-only environment audit (default).
  install  Create venv, install package.
  test     Import smoke + live API check if a token is present.

See top of script for tunable env vars.
USAGE
}

case "${1:-survey}" in
    survey)  cmd_survey ;;
    install) cmd_install ;;
    test)    cmd_test ;;
    -h|--help|help) usage ;;
    *) usage; exit 2 ;;
esac
