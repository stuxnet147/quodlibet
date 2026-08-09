"""The binding is a compiled CPython extension, not a runtime FFI.

These tests are about the shape of the binding itself rather than about any
verdict, so they run even without an SMT backend.
"""

from __future__ import annotations

import os
import sys

import pytest

import quodlibet
from quodlibet import _quodlibet


def test_the_module_is_a_compiled_extension():
    path = _quodlibet.__file__
    assert path is not None
    assert os.path.splitext(path)[1] in {".pyd", ".so", ".dylib"}


def test_no_ffi_layer_is_involved():
    """ctypes and cffi redeclare the ABI at runtime. Neither is imported by
    importing quodlibet, and neither appears in the package source."""
    before = set(sys.modules)
    assert "cffi" not in before
    source = open(quodlibet.__file__, encoding="utf-8").read()
    assert "ctypes" not in source
    assert "cffi" not in source


def test_it_is_built_against_the_stable_abi():
    assert _quodlibet.ABI_TAG == "abi3"
    # abi3 starts at 3.11 here, so the module must import on any later minor.
    assert sys.version_info >= (3, 11)


def test_core_version_and_backend_are_reported():
    assert quodlibet.core_version()
    info = quodlibet.backend_info()
    assert info["name"] == "bitwuzla"
    assert info["version"] == "0.9.1"
    assert isinstance(info["available"], bool)


def test_unknown_relation_and_policy_names_are_rejected_locally(
    equivalent_pair,
):
    with pytest.raises(ValueError):
        quodlibet.check(relation="isomorphism", **equivalent_pair)
    with pytest.raises(ValueError):
        quodlibet.check(ub_policy="whatever", **equivalent_pair)
    with pytest.raises(ValueError):
        quodlibet.check(observations=["colour"], **equivalent_pair)
    with pytest.raises(ValueError):
        quodlibet.check(budget={"forever": 1}, **equivalent_pair)


def test_a_missing_function_is_an_exception_not_a_verdict(equivalent_pair):
    spec = dict(equivalent_pair)
    spec["left_function"] = "absent"
    with pytest.raises(quodlibet.QuodlibetError) as raised:
        quodlibet.check(**spec)
    assert raised.value.status_name
    assert isinstance(raised.value.status, int)
