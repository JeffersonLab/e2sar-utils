"""Tests for helper functions."""

from iri_run.cli import _default_output_dir


class TestDefaultOutputDir:
    def test_default_account(self):
        assert _default_output_dir("myproject") == "/global/cfs/cdirs/myproject/iri-run"

    def test_other_account(self):
        assert _default_output_dir("testproj") == "/global/cfs/cdirs/testproj/iri-run"
