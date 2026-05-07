"""Tests for helper functions."""

from iri_run.cli import _default_output_dir


class TestDefaultOutputDir:
    def test_amsc016(self):
        assert _default_output_dir("amsc016") == "/global/cfs/cdirs/amsc016/iri-run"

    def test_other_account(self):
        assert _default_output_dir("m3792") == "/global/cfs/cdirs/m3792/iri-run"
