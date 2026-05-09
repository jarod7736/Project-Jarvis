"""Git helpers for the working clone of the 2nd Brain vault.

Everything goes through `git` as a subprocess — keeps the dependency
footprint small (no GitPython) and matches what the user runs by hand.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

from . import config


class GitError(RuntimeError):
    pass


def _run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=config.VAULT_PATH,
        capture_output=True,
        text=True,
        check=check,
    )


def pull_ff_only() -> None:
    """Pull from origin/<branch>, fast-forward only.

    Raises GitError on conflict — the daemon shouldn't auto-resolve. Surface
    the failure so the user can investigate via brain_lint or manually.
    """
    result = _run(
        "pull", "--ff-only", config.GITHUB_REMOTE, config.GITHUB_BRANCH,
        check=False,
    )
    if result.returncode != 0:
        raise GitError(f"pull failed: {result.stderr.strip() or result.stdout.strip()}")


def commit_and_push(message: str, paths: list[Path]) -> None:
    """Stage `paths`, commit with `message`, push.

    On non-fast-forward push reject, retry once after a pull --rebase. If that
    also fails, raise — caller decides whether to surface the error to the
    user or log and move on.
    """
    rels = [str(p.relative_to(config.VAULT_PATH)) for p in paths]
    _run("add", "--", *rels)
    _run("commit", "-m", message)

    push = _run("push", config.GITHUB_REMOTE, config.GITHUB_BRANCH, check=False)
    if push.returncode == 0:
        return

    # Most likely cause: laptop's Obsidian Git plugin pushed in the meantime.
    rebase = _run(
        "pull", "--rebase", config.GITHUB_REMOTE, config.GITHUB_BRANCH,
        check=False,
    )
    if rebase.returncode != 0:
        raise GitError(
            f"push rejected and rebase failed: "
            f"{rebase.stderr.strip() or rebase.stdout.strip()}"
        )
    push2 = _run("push", config.GITHUB_REMOTE, config.GITHUB_BRANCH, check=False)
    if push2.returncode != 0:
        raise GitError(f"push failed after rebase: {push2.stderr.strip()}")


def is_git_repo() -> bool:
    if not (config.VAULT_PATH / ".git").exists():
        return False
    try:
        _run("rev-parse", "--git-dir")
        return True
    except subprocess.CalledProcessError:
        return False
