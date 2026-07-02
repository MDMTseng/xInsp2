#!/usr/bin/env python3
"""ctest runner for the Python leg of the canonical three-way cross-validation.

Runs the canonical-codec pytest suite (shared vectors + the C++ golden-fixture
cross-check). Self-skips (exit 0, loud "SKIP - NOT A PASS") when pytest is not
installed for the configuring interpreter, mirroring the doc_coverage /
contract_schema / contract_live gates so a pytest-less box still configures,
builds, and runs ctest green. Run from tools/xinsp2_py so `xinsp2` imports.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Put tools/xinsp2_py on sys.path so `import xinsp2` resolves regardless of cwd.
if HERE not in sys.path:
    sys.path.insert(0, HERE)

try:
    import pytest
except ImportError:
    print("SKIP - NOT A PASS: pytest is not installed for this interpreter "
          f"({sys.executable}); the canonical Python cross-check leg did not run.")
    sys.exit(0)

sys.exit(pytest.main(["-q", os.path.join(HERE, "tests", "test_canonical.py")]))
