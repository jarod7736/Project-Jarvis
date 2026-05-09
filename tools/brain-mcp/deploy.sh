#!/usr/bin/env bash
# brain-mcp deploy helper for lobsterboy.
#
# Subcommands:
#   survey   read-only checks; prints recommended next steps. Default.
#   install  create venv, pip install, write systemd units, enable timer.
#            Prompts before each destructive step.
#   test     smoke-test the installed package against $BRAIN_VAULT_PATH.
#
# Run from anywhere — the script resolves its own location to find the
# package source. Designed to be re-run safely; install steps short-circuit
# if their work is already done.
#
# Tunable env vars:
#   BRAIN_VAULT_PATH  default /srv/2ndbrain
#   BRAIN_REPO_URL    default git@github.com:jarod7736/2ndBrain.git
#   VENV_DIR          default <package_dir>/.venv
#   SYSTEMD_UNIT_DIR  default /etc/systemd/system

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VAULT_PATH="${BRAIN_VAULT_PATH:-/srv/2ndbrain}"
VAULT_REPO="${BRAIN_REPO_URL:-git@github.com:jarod7736/2ndBrain.git}"
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
    info "Vault path:    ${VAULT_PATH}"
    info "Venv target:   ${VENV_DIR}"

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

    if command -v git >/dev/null; then
        ok "git: $(git --version | awk '{print $3}')"
    else
        fail "git: not installed"
    fi

    if command -v systemctl >/dev/null; then
        ok "systemctl: present"
    else
        warn "systemctl: not found — timer install will be skipped"
    fi

    if [ -e "${PKG_DIR}/pyproject.toml" ]; then
        ok "Package: ${PKG_DIR} (pyproject.toml present)"
    else
        fail "Package: ${PKG_DIR}/pyproject.toml missing — wrong dir?"
    fi

    header "Vault clone"
    if [ -d "${VAULT_PATH}" ]; then
        if [ -d "${VAULT_PATH}/.git" ]; then
            local origin branch
            origin=$(git -C "${VAULT_PATH}" remote get-url origin 2>/dev/null || echo "<none>")
            branch=$(git -C "${VAULT_PATH}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "<none>")
            ok   "${VAULT_PATH}: git working copy"
            info "    origin: ${origin}"
            info "    branch: ${branch}"
            local ahead behind
            git -C "${VAULT_PATH}" fetch -q origin 2>/dev/null || true
            ahead=$(git -C "${VAULT_PATH}" rev-list --count "@{u}..HEAD" 2>/dev/null || echo "?")
            behind=$(git -C "${VAULT_PATH}" rev-list --count "HEAD..@{u}" 2>/dev/null || echo "?")
            info "    ahead/behind upstream: ${ahead}/${behind}"
        else
            warn "${VAULT_PATH}: directory exists but not a git working copy"
        fi
    else
        warn "${VAULT_PATH}: missing — vault not cloned yet"
        info "    suggested: git clone ${VAULT_REPO} ${VAULT_PATH}"
    fi

    header "Existing install"
    if [ -d "${VENV_DIR}" ]; then
        if [ -x "${VENV_DIR}/bin/python" ]; then
            ok "Venv: ${VENV_DIR}"
            if "${VENV_DIR}/bin/python" -c 'import brain_mcp' 2>/dev/null; then
                ok "    brain_mcp importable"
            else
                warn "    brain_mcp NOT importable — re-run install"
            fi
        else
            warn "Venv dir exists but lacks bin/python: ${VENV_DIR}"
        fi
    else
        info "Venv: not yet created"
    fi

    if [ -e "${SYSTEMD_UNIT_DIR}/brain-sync.timer" ]; then
        ok "Systemd: brain-sync.timer installed"
        if systemctl is-active --quiet brain-sync.timer 2>/dev/null; then
            ok "    timer active"
        else
            warn "    timer present but not active (run: sudo systemctl enable --now brain-sync.timer)"
        fi
    else
        info "Systemd: brain-sync.timer not installed yet"
    fi

    header "Next steps"
    info "  ./deploy.sh install    # set up venv + systemd timer"
    info "  ./deploy.sh test       # smoke-test against the vault"
}

cmd_install() {
    header "Install"
    info "User:        ${RUN_USER}"
    info "Vault path:  ${VAULT_PATH}"
    info "Venv:        ${VENV_DIR}"
    info "Unit dir:    ${SYSTEMD_UNIT_DIR}"
    echo

    # ── Vault ───────────────────────────────────────────────────────────
    if [ ! -d "${VAULT_PATH}" ]; then
        warn "Vault not present at ${VAULT_PATH}"
        if confirm "Clone ${VAULT_REPO} → ${VAULT_PATH}?"; then
            local parent
            parent="$(dirname "${VAULT_PATH}")"
            if [ ! -d "${parent}" ]; then
                info "Creating parent dir (sudo): ${parent}"
                sudo install -d -o "${RUN_USER}" -g "${RUN_USER}" "${parent}"
            fi
            git clone "${VAULT_REPO}" "${VAULT_PATH}"
            ok "Vault cloned"
        else
            warn "Skipping vault clone — install will continue but tools won't work"
        fi
    else
        ok "Vault already present"
    fi

    # ── Venv + pip install ──────────────────────────────────────────────
    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        info "Creating venv: ${VENV_DIR}"
        python3 -m venv "${VENV_DIR}"
        ok "Venv created"
    else
        ok "Venv exists"
    fi

    info "Installing brain-mcp package (editable)"
    "${VENV_DIR}/bin/pip" install --upgrade pip >/dev/null
    "${VENV_DIR}/bin/pip" install -e "${PKG_DIR}"
    ok "Package installed"

    "${VENV_DIR}/bin/python" -c 'import brain_mcp; from brain_mcp.tools import search, capture, lint, ingest'
    ok "Import smoke test passed"

    # ── Systemd ─────────────────────────────────────────────────────────
    if ! command -v systemctl >/dev/null; then
        warn "systemctl not present — skipping timer install"
        return
    fi

    if confirm "Install brain-sync.{service,timer} into ${SYSTEMD_UNIT_DIR} (sudo)?"; then
        local svc_src="${PKG_DIR}/systemd/brain-sync.service"
        local tim_src="${PKG_DIR}/systemd/brain-sync.timer"
        local svc_tmp tim_tmp
        svc_tmp="$(mktemp)"
        tim_tmp="$(mktemp)"
        # Substitute User= and BRAIN_VAULT_PATH so the unit reflects this host.
        sed -e "s|^User=.*|User=${RUN_USER}|" \
            -e "s|^Environment=BRAIN_VAULT_PATH=.*|Environment=BRAIN_VAULT_PATH=${VAULT_PATH}|" \
            "${svc_src}" > "${svc_tmp}"
        cp "${tim_src}" "${tim_tmp}"

        sudo install -m 0644 "${svc_tmp}" "${SYSTEMD_UNIT_DIR}/brain-sync.service"
        sudo install -m 0644 "${tim_tmp}" "${SYSTEMD_UNIT_DIR}/brain-sync.timer"
        rm -f "${svc_tmp}" "${tim_tmp}"
        ok "Units installed"

        sudo systemctl daemon-reload
        sudo systemctl enable --now brain-sync.timer
        ok "brain-sync.timer enabled"

        echo
        info "Timer status:"
        systemctl list-timers brain-sync.timer --no-pager || true
    else
        warn "Skipping systemd install"
    fi

    echo
    ok "Install complete."
    info "Next: ./deploy.sh test"
    info "Then wire OpenClaw — see PR #26 description for the spawn config sketch."
}

cmd_test() {
    header "Smoke test"
    if [ ! -x "${VENV_DIR}/bin/python" ]; then
        fail "Venv missing — run: ./deploy.sh install"
        exit 1
    fi
    if [ ! -d "${VAULT_PATH}" ]; then
        fail "Vault missing at ${VAULT_PATH}"
        exit 1
    fi

    info "Running brain_lint() against ${VAULT_PATH} (truncated to 60 lines)..."
    BRAIN_VAULT_PATH="${VAULT_PATH}" BRAIN_PUSH=0 \
        "${VENV_DIR}/bin/python" -c 'from brain_mcp.tools import lint; print(lint.run())' \
        | head -60
    echo
    ok "lint OK"

    info "Running brain_search('jarvis') against ${VAULT_PATH} (top 50 lines)..."
    BRAIN_VAULT_PATH="${VAULT_PATH}" BRAIN_PUSH=0 \
        "${VENV_DIR}/bin/python" -c 'from brain_mcp.tools import search; print(search.run("jarvis", k=3))' \
        | head -50
    echo
    ok "search OK"

    info "Running brain_ingest_status() against ${VAULT_PATH}..."
    BRAIN_VAULT_PATH="${VAULT_PATH}" BRAIN_PUSH=0 \
        "${VENV_DIR}/bin/python" -c 'from brain_mcp.tools import ingest; print(ingest.status())'
    echo
    ok "ingest_status OK"

    info "brain_capture is NOT exercised here (would commit + push to GitHub)."
    info "To test capture end-to-end, on a scratch branch in the vault, run:"
    info "  BRAIN_VAULT_PATH=${VAULT_PATH} BRAIN_PUSH=0 \\"
    info "    ${VENV_DIR}/bin/python -c \"from brain_mcp.tools import capture; print(capture.run('test from deploy script'))\""
}

usage() {
    cat <<USAGE
Usage: $0 [survey|install|test]

  survey   Read-only environment audit (default).
  install  Create venv, install package, write systemd units (interactive).
  test     Smoke-test installed package against \$BRAIN_VAULT_PATH.

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
