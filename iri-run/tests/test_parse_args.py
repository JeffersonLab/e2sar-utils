"""Tests for parse_args."""

import pytest

from iri_run.cli import parse_args


class TestDefaults:
    """Verify all default values when only a command is given."""

    def test_account_default(self):
        args = parse_args(["--", "echo", "hi"])
        assert args.account == "amsc016"

    def test_qos_default(self):
        args = parse_args(["--", "echo"])
        assert args.qos == "cron"

    def test_constraint_default(self):
        args = parse_args(["--", "echo"])
        assert args.constraint == "cron"

    def test_time_default(self):
        args = parse_args(["--", "echo"])
        assert args.time_minutes == 30

    def test_nodes_default(self):
        args = parse_args(["--", "echo"])
        assert args.nodes == 1

    def test_ntasks_default(self):
        args = parse_args(["--", "echo"])
        assert args.ntasks == 1

    def test_partition_default(self):
        args = parse_args(["--", "echo"])
        assert args.partition is None

    def test_directory_default(self):
        args = parse_args(["--", "echo"])
        assert args.directory is None

    def test_env_default(self):
        args = parse_args(["--", "echo"])
        assert args.env == []

    def test_wait_default(self):
        args = parse_args(["--", "echo"])
        assert args.wait is False


class TestExplicitOptions:
    """Verify each flag overrides its default."""

    def test_account(self):
        args = parse_args(["-A", "m3792", "--", "echo"])
        assert args.account == "m3792"

    def test_account_long(self):
        args = parse_args(["--account", "m3792", "--", "echo"])
        assert args.account == "m3792"

    def test_qos(self):
        args = parse_args(["-q", "debug", "--", "echo"])
        assert args.qos == "debug"

    def test_qos_long(self):
        args = parse_args(["--qos", "regular", "--", "echo"])
        assert args.qos == "regular"

    def test_constraint(self):
        args = parse_args(["-C", "gpu", "--", "echo"])
        assert args.constraint == "gpu"

    def test_constraint_long(self):
        args = parse_args(["--constraint", "gpu", "--", "echo"])
        assert args.constraint == "gpu"

    def test_time(self):
        args = parse_args(["-t", "60", "--", "echo"])
        assert args.time_minutes == 60

    def test_time_long(self):
        args = parse_args(["--time", "120", "--", "echo"])
        assert args.time_minutes == 120

    def test_nodes(self):
        args = parse_args(["-N", "4", "--", "echo"])
        assert args.nodes == 4

    def test_nodes_long(self):
        args = parse_args(["--nodes", "8", "--", "echo"])
        assert args.nodes == 8

    def test_ntasks(self):
        args = parse_args(["-n", "16", "--", "echo"])
        assert args.ntasks == 16

    def test_ntasks_long(self):
        args = parse_args(["--ntasks", "32", "--", "echo"])
        assert args.ntasks == 32

    def test_partition(self):
        args = parse_args(["-p", "regular", "--", "echo"])
        assert args.partition == "regular"

    def test_partition_long(self):
        args = parse_args(["--partition", "debug", "--", "echo"])
        assert args.partition == "debug"

    def test_directory(self):
        args = parse_args(["-d", "/scratch/work", "--", "echo"])
        assert args.directory == "/scratch/work"

    def test_directory_long(self):
        args = parse_args(["--directory", "/scratch/work", "--", "echo"])
        assert args.directory == "/scratch/work"

    def test_wait(self):
        args = parse_args(["-w", "--", "echo"])
        assert args.wait is True

    def test_wait_long(self):
        args = parse_args(["--wait", "--", "echo"])
        assert args.wait is True


class TestEnvVars:
    """Verify --env accumulation behavior."""

    def test_single_env(self):
        args = parse_args(["--env", "FOO=bar", "--", "echo"])
        assert args.env == ["FOO=bar"]

    def test_multiple_env(self):
        args = parse_args(["--env", "A=1", "--env", "B=2", "--", "echo"])
        assert args.env == ["A=1", "B=2"]

    def test_env_with_equals_in_value(self):
        args = parse_args(["--env", "CMD=a=b=c", "--", "echo"])
        assert args.env == ["CMD=a=b=c"]


class TestCommandParsing:
    """Verify command extraction from argv."""

    def test_simple_command(self):
        args = parse_args(["--", "echo", "hello"])
        assert args.command == ["echo", "hello"]

    def test_command_without_separator(self):
        args = parse_args(["echo", "hello"])
        assert args.command == ["echo", "hello"]

    def test_single_word_command(self):
        args = parse_args(["--", "hostname"])
        assert args.command == ["hostname"]

    def test_command_with_flags(self):
        args = parse_args(["--", "python", "-u", "script.py", "--verbose"])
        assert args.command == ["python", "-u", "script.py", "--verbose"]

    def test_no_command_raises(self):
        with pytest.raises(SystemExit):
            parse_args([])

    def test_only_separator_raises(self):
        with pytest.raises(SystemExit):
            parse_args(["--"])

    def test_options_before_separator(self):
        args = parse_args(["-A", "m1234", "-N", "2", "--", "bash", "-c", "echo hi"])
        assert args.account == "m1234"
        assert args.nodes == 2
        assert args.command == ["bash", "-c", "echo hi"]


class TestCombinedOptions:
    """Verify multiple options used together."""

    def test_full_option_set(self):
        args = parse_args([
            "-A", "m3792",
            "-q", "debug",
            "-C", "gpu",
            "-t", "10",
            "-N", "2",
            "-n", "8",
            "-p", "regular",
            "-d", "/work",
            "--env", "X=1",
            "-w",
            "--", "srun", "hostname",
        ])
        assert args.account == "m3792"
        assert args.qos == "debug"
        assert args.constraint == "gpu"
        assert args.time_minutes == 10
        assert args.nodes == 2
        assert args.ntasks == 8
        assert args.partition == "regular"
        assert args.directory == "/work"
        assert args.env == ["X=1"]
        assert args.wait is True
        assert args.command == ["srun", "hostname"]
