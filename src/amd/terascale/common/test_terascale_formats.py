import pytest
import sys
import os
from unittest.mock import MagicMock

# Mock yaml before it's imported by u_format_parse
sys.modules['yaml'] = MagicMock()

# Ensure u_format_parse is findable before importing terascale_formats
# terascale_formats.py uses sys.argv[0] to find u_format_parse, which is unreliable in tests.
sys.path.append(os.path.join(os.path.dirname(__file__), "../../../util/format"))

from terascale_formats import NumberTypeInfo

def test_number_type_info_is_equal_ignoring_sign_none():
    info = NumberTypeInfo('UNORM', 'SNORM')
    assert info.is_equal_ignoring_sign(None) is False

def test_number_type_info_is_equal_ignoring_sign_same():
    info = NumberTypeInfo('UNORM', 'SNORM')
    assert info.is_equal_ignoring_sign('UNORM') is True

def test_number_type_info_is_equal_ignoring_sign_opposite():
    info = NumberTypeInfo('UNORM', 'SNORM')
    assert info.is_equal_ignoring_sign('SNORM') is True

def test_number_type_info_is_equal_ignoring_sign_different():
    info = NumberTypeInfo('UNORM', 'SNORM')
    assert info.is_equal_ignoring_sign('UINT') is False

def test_number_type_info_is_equal_ignoring_sign_no_opposite():
    info = NumberTypeInfo('FLOAT')
    assert info.is_equal_ignoring_sign('FLOAT') is True
    assert info.is_equal_ignoring_sign('UNORM') is False
