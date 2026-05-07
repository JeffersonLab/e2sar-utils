"""Tests for script resolution in iri-bash."""

import io
from unittest.mock import patch

import pytest

from iri_run.bash import _resolve_script, parse_args


class TestResolveFromArgs:
    """Command-line arguments are joined into a single script string."""

    def test_single_word(self):
        args = parse_args(["hostname"])
        assert _resolve_script(args) == "hostname"

    def test_multiple_words_joined(self):
        args = parse_args(["echo", "hello", "world"])
        assert _resolve_script(args) == "echo hello world"

    def test_quoted_complex_command(self):
        args = parse_args(["for i in 1 2 3; do echo $i; done"])
        assert _resolve_script(args) == "for i in 1 2 3; do echo $i; done"


class TestResolveFromFile:
    """The -f flag reads the script from a file."""

    def test_reads_file_content(self, tmp_path):
        script = tmp_path / "test.sh"
        script.write_text("#!/bin/bash\necho hello\n")
        args = parse_args(["-f", str(script)])
        result = _resolve_script(args)
        assert result == "#!/bin/bash\necho hello\n"

    def test_file_takes_precedence_over_args(self, tmp_path):
        script = tmp_path / "test.sh"
        script.write_text("from file")
        args = parse_args(["-f", str(script), "ignored", "args"])
        result = _resolve_script(args)
        assert result == "from file"


class TestResolveFromStdin:
    """When no args and no file, reads from stdin (if not a tty)."""

    def test_reads_stdin(self):
        args = parse_args([])
        # Simulate empty command (parse_args won't error since REMAINDER can be empty)
        args.command = []
        args.script_file = None
        with patch("sys.stdin", io.StringIO("echo from stdin\n")), \
             patch("sys.stdin.isatty", return_value=False):
            result = _resolve_script(args)
        assert result == "echo from stdin\n"

    def test_exits_if_tty_and_no_input(self):
        args = parse_args([])
        args.command = []
        args.script_file = None
        with patch("sys.stdin.isatty", return_value=True), \
             pytest.raises(SystemExit):
            _resolve_script(args)
