import importlib.util
import os
import sys

_test_dir = os.path.dirname(__file__)
_format_dir = os.path.normpath(os.path.join(_test_dir, "../../../util/format"))
_module_path = os.path.join(_test_dir, "terascale_formats.py")
_original_sys_path = list(sys.path)
try:
    sys.path.insert(0, _format_dir)
    _module_spec = importlib.util.spec_from_file_location(
        "terascale_formats_under_test", _module_path
    )
    assert _module_spec is not None
    assert _module_spec.loader is not None
    _terascale_formats = importlib.util.module_from_spec(_module_spec)
    _module_spec.loader.exec_module(_terascale_formats)
finally:
    sys.path[:] = _original_sys_path

NumberTypeInfo = _terascale_formats.NumberTypeInfo


def test_number_type_info_is_equal_ignoring_sign_none():
    info = NumberTypeInfo("UNORM", "SNORM")
    assert info.is_equal_ignoring_sign(None) is False


def test_number_type_info_is_equal_ignoring_sign_same():
    info = NumberTypeInfo("UNORM", "SNORM")
    assert info.is_equal_ignoring_sign("UNORM") is True


def test_number_type_info_is_equal_ignoring_sign_opposite():
    info = NumberTypeInfo("UNORM", "SNORM")
    assert info.is_equal_ignoring_sign("SNORM") is True


def test_number_type_info_is_equal_ignoring_sign_different():
    info = NumberTypeInfo("UNORM", "SNORM")
    assert info.is_equal_ignoring_sign("UINT") is False


def test_number_type_info_is_equal_ignoring_sign_no_opposite():
    info = NumberTypeInfo("FLOAT")
    assert info.is_equal_ignoring_sign("FLOAT") is True
    assert info.is_equal_ignoring_sign("UNORM") is False


if __name__ == "__main__":
    import pytest

    raise SystemExit(pytest.main([__file__]))
