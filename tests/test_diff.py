"""pytest entry points for the differential tests (smaller than the command-line runs in docs/TESTING.md)."""
import os
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "diff"))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

pytest.importorskip("unicorn")
pytest.importorskip("ziglang")

import func_diff  # noqa: E402
import insn_diff  # noqa: E402


@pytest.mark.parametrize("seed", [101, 202])
def test_single_instructions_match_unicorn(seed):
    _, failures = insn_diff.run(seed, 2500, 2, os.path.join(ROOT, "build", "tests", f"pytest_insn_{seed}"),
                                verbose=False)
    assert not failures, [f"{i!r}: {m[:3]}" for i, _, m in failures[:10]]


@pytest.mark.parametrize("opt", ["O0", "O2"])
def test_whole_functions_match_unicorn_and_native(opt):
    assert func_diff.run(opt, 4, os.path.join(ROOT, "build", "tests", "pytest_func")) == 0
