"""Tests for iri-bash argument parsing."""

import pytest

from iri_run.bash import parse_args


class TestRunDefaults:
    """Verify defaults when running a command."""

    def test_account_default(self):
        args = parse_args(["echo", "hi"])
        assert args.account == "amsc016"

    def test_time_default(self):
        args = parse_args(["echo"])
        assert args.time_minutes == 30

    def test_no_wait_default(self):
        args = parse_args(["echo"])
        assert args.no_wait is False

    def test_script_file_default(self):
        args = parse_args(["echo"])
        assert args.script_file is None

    def test_subcommand_is_none(self):
        args = parse_args(["echo"])
        assert args.subcommand is None


class TestRunOptions:
    """Verify run options can be overridden."""

    def test_account(self):
        args = parse_args(["-A", "m3792", "echo"])
        assert args.account == "m3792"

    def test_account_long(self):
        args = parse_args(["--account", "m3792", "echo"])
        assert args.account == "m3792"

    def test_time(self):
        args = parse_args(["-t", "60", "echo"])
        assert args.time_minutes == 60

    def test_time_long(self):
        args = parse_args(["--time", "120", "echo"])
        assert args.time_minutes == 120

    def test_no_wait(self):
        args = parse_args(["--no-wait", "echo"])
        assert args.no_wait is True


class TestRunCommand:
    """Verify command parsing for run mode."""

    def test_single_word(self):
        args = parse_args(["hostname"])
        assert args.command == ["hostname"]

    def test_multiple_words(self):
        args = parse_args(["echo", "hello", "world"])
        assert args.command == ["echo", "hello", "world"]

    def test_with_separator(self):
        args = parse_args(["--", "echo", "hello"])
        assert args.command == ["echo", "hello"]

    def test_complex_command(self):
        args = parse_args(["for i in 1 2 3; do echo $i; done"])
        assert args.command == ["for i in 1 2 3; do echo $i; done"]

    def test_options_before_command(self):
        args = parse_args(["-A", "m3792", "-t", "10", "echo", "hi"])
        assert args.account == "m3792"
        assert args.time_minutes == 10
        assert args.command == ["echo", "hi"]


class TestRunWithFile:
    """Verify -f flag."""

    def test_file_flag(self, tmp_path):
        script = tmp_path / "test.sh"
        script.write_text("echo hello")
        args = parse_args(["-f", str(script)])
        assert args.script_file is not None
        content = args.script_file.read()
        args.script_file.close()
        assert content == "echo hello"


class TestReapSubcommand:
    """Verify reap subcommand parsing."""

    def test_reap_uuid(self):
        args = parse_args(["reap", "abc123def456"])
        assert args.subcommand == "reap"
        assert args.uuid == "abc123def456"

    def test_reap_with_account(self):
        args = parse_args(["reap", "-A", "m3792", "abc123"])
        assert args.subcommand == "reap"
        assert args.account == "m3792"
        assert args.uuid == "abc123"

    def test_reap_default_account(self):
        args = parse_args(["reap", "abc123"])
        assert args.account == "amsc016"
