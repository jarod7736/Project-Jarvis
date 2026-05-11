#!/usr/bin/env bash
# oc-personal-runner deploy helper for lobsterboy.
#
# Companion to tools/brain-mcp/deploy.sh. Run that one first; it sets up
# the brain MCP server which this runner spawns as a stdio child.
#
# Subcommands:
#   survey   read-only checks. Default.
#   install  create venv, pip install, write systemd unit, enable service.
#   test     hit /healthz and /v1/chat/completions on the running service.

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${PKG_DIR}/../.." && pwd)"
BRAIN_MCP_DIR="${PROJECT_ROOT}/tools/brain-mcp"

LISTEN_PORT="${OC_LISTEN_PORT:-8080}"
LMSTUDIO_URL="${OC_LMSTUDIO_URL:-http://192.168.1.108:1234}"
VENV_DIR="${VENV_DIR:-${PKG_DIR}/.venv}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
SECRETS_FILE="${OC_SECRETS_FILE:-/etc/oc-personal/secrets.env}"
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
    info "User:                ${RUN_USER}"
    info "Hostname:            $(hostname -f 2>/dev/null || hostname)"
    info "Runner pkg dir:      ${PKG_DIR}"
    info "Venv target:         ${VENV_DIR}"
    info "Listen port:         ${LISTEN_PORT}"
    info "LM Studio passthru:  ${LMSTUDIO_URL}"
    info "Secrets file:        ${SECRETS_FILE}"

    header "Prerequisites"
    if command -v python3 >/dev/null && python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)'; then
        ok "python3: $(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
    else
        fail "python3 >= 3.11 required"
    fi

    if command -v systemctl >/dev/null; then
        ok "systemctl: present"
    else
        warn "systemctl: not found — service install will be skipped"
    fi

    header "brain-mcp dependency"
    if [ -x "${BRAIN_MCP_DIR}/.venv/bin/python" ]; then
        ok "brain-mcp venv: ${BRAIN_MCP_DIR}/.venv"
        if "${BRAIN_MCP_DIR}/.venv/bin/python" -c 'import brain_mcp' 2>/dev/null; then
            ok "    brain_mcp importable"
        else
            warn "    brain_mcp NOT importable in that venv"
        fi
    else
        fail "brain-mcp venv missing — run tools/brain-mcp/deploy.sh install first"
    fi

    header "Existing install"
    if [ -d "${VENV_DIR}" ] && [ -x "${VENV_DIR}/bin/python" ]; then
        ok "Venv: ${VENV_DIR}"
        if "${VENV_DIR}/bin/python" -c 'import oc_personal' 2>/dev/null; then
            ok "    oc_personal importable"
        else
            warn "    oc_personal NOT importable — re-run install"
        fi
    else
        info "Venv: not yet created"
    fi

    if [ -f "${SECRETS_FILE}" ]; then
        if [ "$(stat -c '%a' "${SECRETS_FILE}")" = "600" ]; then
            ok "Secrets file: ${SECRETS_FILE} (0600)"
        else
            warn "Secrets file: ${SECRETS_FILE} (chmod to 0600)"
        fi
    else
        info "Secrets file: ${SECRETS_FILE} — not present"
        info "    Create with:  echo 'ANTHROPIC_API_KEY=sk-ant-...' | sudo tee ${SECRETS_FILE} && sudo chmod 600 ${SECRETS_FILE}"
    fi

    if [ -e "${SYSTEMD_UNIT_DIR}/oc-personal.service" ]; then
        ok "Systemd: oc-personal.service installed"
        if systemctl is-active --quiet oc-personal.service 2>/dev/null; then
            ok "    service active"
            info "    journalctl -u oc-personal.service -n 20"
        else
            warn "    service present but not active"
        fi
    else
        info "Systemd: oc-personal.service not installed yet"
    fi

    header "Next steps"
    info "  ./deploy.sh install    # set up venv + systemd service"
    info "  ./deploy.sh test       # hit /healthz + smoke chat completion"
}

cmd_install() {
    header "Install"
    info "User:        ${RUN_USER}"
    info "Venv:        ${VENV_DIR}"
    info "Unit dir:    ${SYSTEMD_UNIT_DIR}"
    echo

    if [ ! -d "${BRAIN_MCP_DIR}/.venv" ]; then
        fail "brain-mcp venv missing at ${BRAIN_MCP_DIR}/.venv"
        info "Run tools/brain-mcp/deploy.sh install first."
        exit 1
    fi

    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        info "Creating venv: ${VENV_DIR}"
        python3 -m venv "${VENV_DIR}"
        ok "Venv created"
    else
        ok "Venv exists"
    fi

    info "Installing oc-personal-runner package (editable)"
    "${VENV_DIR}/bin/pip" install --upgrade pip >/dev/null
    "${VENV_DIR}/bin/pip" install -e "${PKG_DIR}"
    ok "Package installed"

    "${VENV_DIR}/bin/python" -c 'import oc_personal; from oc_personal import agent, proxy, server, config'
    ok "Import smoke test passed"

    if [ ! -f "${SECRETS_FILE}" ]; then
        warn "Secrets file missing: ${SECRETS_FILE}"
        info "After install, create it with:"
        info "  sudo install -d -m 0700 \"\$(dirname ${SECRETS_FILE})\""
        info "  echo 'ANTHROPIC_API_KEY=sk-ant-...' | sudo tee ${SECRETS_FILE}"
        info "  sudo chmod 600 ${SECRETS_FILE}"
        info "Then: sudo systemctl restart oc-personal.service"
    fi

    if ! command -v systemctl >/dev/null; then
        warn "systemctl not present — skipping service install"
        return
    fi

    if confirm "Install oc-personal.service into ${SYSTEMD_UNIT_DIR} (sudo)?"; then
        local svc_src="${PKG_DIR}/systemd/oc-personal.service"
        local svc_tmp
        svc_tmp="$(mktemp)"
        # The checked-in unit is a template with __RUN_USER__, __PROJECT_ROOT__,
        # and __USER_HOME__ placeholders (see
        # tools/oc-personal-runner/systemd/oc-personal.service). Replace
        # those first, then key-anchored substitutions handle the
        # configurable knobs.
        local user_home
        user_home="$(getent passwd "${RUN_USER}" | cut -d: -f6)"
        if [ -z "${user_home}" ]; then
            user_home="${HOME}"
        fi
        sed -e "s|__RUN_USER__|${RUN_USER}|g" \
            -e "s|__PROJECT_ROOT__|${PROJECT_ROOT}|g" \
            -e "s|__USER_HOME__|${user_home}|g" \
            -e "s|^Environment=OC_LMSTUDIO_URL=.*|Environment=OC_LMSTUDIO_URL=${LMSTUDIO_URL}|" \
            -e "s|^Environment=OC_LISTEN_PORT=.*|Environment=OC_LISTEN_PORT=${LISTEN_PORT}|" \
            -e "s|^EnvironmentFile=.*|EnvironmentFile=-${SECRETS_FILE}|" \
            "${svc_src}" > "${svc_tmp}"

        # Refuse to install if any sentinel survived on a directive line. A
        # leftover placeholder means we'd hand systemd a unit pointing at a
        # path that doesn't exist; the service would crash-loop opaquely.
        # Skip comments (the documentation block above intentionally mentions
        # __FOO__ as a literal example).
        if grep -vE '^\s*#' "${svc_tmp}" | grep -qE '__[A-Z_]+__'; then
            fail "Unsubstituted placeholders in rendered unit — aborting install."
            info "Offending lines:"
            grep -nE '__[A-Z_]+__' "${svc_tmp}" | grep -vE ':\s*#' | sed 's/^/    /'
            rm -f "${svc_tmp}"
            exit 1
        fi

        sudo install -m 0644 "${svc_tmp}" "${SYSTEMD_UNIT_DIR}/oc-personal.service"
        rm -f "${svc_tmp}"
        ok "Unit installed"

        sudo systemctl daemon-reload
        sudo systemctl enable --now oc-personal.service
        ok "oc-personal.service enabled"

        echo
        info "Service status (last 5 log lines):"
        systemctl --no-pager status oc-personal.service | head -10 || true
    else
        warn "Skipping systemd install"
    fi

    echo
    ok "Install complete."
    info "Next: ./deploy.sh test"
    info "Then on Jarvis, set NVS oc_host to: http://<this-host>:${LISTEN_PORT}"
}

cmd_test() {
    header "Smoke test"
    local base="http://localhost:${LISTEN_PORT}"

    info "GET ${base}/healthz"
    if ! curl -fsS "${base}/healthz" | sed 's/^/  /'; then
        fail "healthz failed — is oc-personal.service running?"
        info "  sudo systemctl status oc-personal.service"
        exit 1
    fi
    echo
    ok "healthz OK"
    echo

    info "POST ${base}/v1/chat/completions  (model=oc-personal)"
    info "Question: 'what do I know about jarvis'"
    echo
    curl -fsS -X POST "${base}/v1/chat/completions" \
        -H 'Content-Type: application/json' \
        -d '{"model":"oc-personal","messages":[{"role":"user","content":"what do I know about jarvis"}]}' \
        | python3 -m json.tool || {
            fail "chat-completions request failed"
            exit 1
        }
    echo
    ok "oc-personal chat completion OK"
    echo

    info "POST ${base}/v1/chat/completions  (model=google/gemma-4-e4b — proxied to LM Studio)"
    info "If LM Studio at ${LMSTUDIO_URL} is reachable, expect a real reply."
    info "If not, expect a proxy-error response (still 200, with 'upstream:' message)."
    echo
    curl -fsS -X POST "${base}/v1/chat/completions" \
        -H 'Content-Type: application/json' \
        -d '{"model":"google/gemma-4-e4b","messages":[{"role":"user","content":"hi"}],"max_tokens":20}' \
        | python3 -m json.tool || true
    echo
    ok "proxy path exercised"
}

usage() {
    cat <<USAGE
Usage: $0 [survey|install|test]

  survey   Read-only environment audit (default).
  install  Create venv, install package, write systemd unit (interactive).
  test     Hit /healthz and /v1/chat/completions on the running service.

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
