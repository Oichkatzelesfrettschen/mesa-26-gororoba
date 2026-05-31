import pytest
from gitlab_common import pretty_duration

@pytest.mark.parametrize(
    "seconds, expected",
    [
        (0, "0s"),
        (1, "1s"),
        (59, "59s"),
        (60, "1m00s"),
        (61, "1m01s"),
        (119, "1m59s"),
        (120, "2m00s"),
        (3599, "59m59s"),
        (3600, "1h00m00s"),
        (3601, "1h00m01s"),
        (3661, "1h01m01s"),
        (7261, "2h01m01s"),
        (1.5, "1s"),
        (60.9, "1m00s"),
    ],
)
def test_pretty_duration(seconds, expected):
    assert pretty_duration(seconds) == expected
