"""Tests for build_payload."""

from iri_run.cli import build_payload, parse_args


class TestBasicPayload:
    """Verify payload structure with defaults."""

    def test_executable(self):
        args = parse_args(["--", "echo", "hello"])
        payload = build_payload(args)
        assert payload["executable"] == "echo"

    def test_arguments(self):
        args = parse_args(["--", "echo", "hello", "world"])
        payload = build_payload(args)
        assert payload["arguments"] == ["hello", "world"]

    def test_arguments_empty(self):
        args = parse_args(["--", "hostname"])
        payload = build_payload(args)
        assert payload["arguments"] == []

    def test_resources_defaults(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["resources"] == {"node_count": 1, "process_count": 1}

    def test_resources_custom(self):
        args = parse_args(["-N", "4", "-n", "16", "--", "echo"])
        payload = build_payload(args)
        assert payload["resources"] == {"node_count": 4, "process_count": 16}

    def test_duration_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["duration"] == 30 * 60

    def test_duration_custom(self):
        args = parse_args(["-t", "120", "--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["duration"] == 120 * 60

    def test_account_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["account"] == "amsc016"

    def test_account_custom(self):
        args = parse_args(["-A", "m3792", "--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["account"] == "m3792"


class TestJobName:
    """Verify job name is iri-run-{hint}."""

    def test_name_with_hint(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args, job_id_hint="abc123")
        assert payload["name"] == "iri-run-abc123"

    def test_name_with_empty_hint(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args, job_id_hint="")
        assert payload["name"] == "iri-run-job"

    def test_name_with_default_hint(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["name"] == "iri-run-job"


class TestOutputPaths:
    """Verify stdout/stderr paths are always derived from hint."""

    def test_stdout_path(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args, job_id_hint="abc123")
        assert payload["stdout_path"] == "/global/cfs/cdirs/amsc016/iri-run/abc123.out"

    def test_stderr_path(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args, job_id_hint="abc123")
        assert payload["stderr_path"] == "/global/cfs/cdirs/amsc016/iri-run/abc123.err"

    def test_paths_use_account(self):
        args = parse_args(["-A", "m3792", "--", "echo"])
        payload = build_payload(args, job_id_hint="xyz")
        assert "/m3792/" in payload["stdout_path"]
        assert "/m3792/" in payload["stderr_path"]

    def test_paths_with_default_hint(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["stdout_path"].endswith("/job.out")
        assert payload["stderr_path"].endswith("/job.err")

    def test_paths_always_present(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args, job_id_hint="abc")
        assert "stdout_path" in payload
        assert "stderr_path" in payload


class TestCustomAttributes:
    """Verify slurm custom_attributes in the payload."""

    def test_qos_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["custom_attributes"]["qos"] == "cron"

    def test_qos_custom(self):
        args = parse_args(["-q", "debug", "--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["custom_attributes"]["qos"] == "debug"

    def test_constraint_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["custom_attributes"]["constraint"] == "cron"

    def test_constraint_custom(self):
        args = parse_args(["-C", "gpu", "--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["custom_attributes"]["constraint"] == "gpu"

    def test_partition_absent_by_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert "partition" not in payload["attributes"]["custom_attributes"]

    def test_partition_present_when_set(self):
        args = parse_args(["-p", "regular", "--", "echo"])
        payload = build_payload(args)
        assert payload["attributes"]["custom_attributes"]["partition"] == "regular"


class TestEnvironment:
    """Verify environment variables in the payload."""

    def test_no_env(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert "environment" not in payload

    def test_single_env(self):
        args = parse_args(["--env", "FOO=bar", "--", "echo"])
        payload = build_payload(args)
        assert payload["environment"] == {"FOO": "bar"}

    def test_multiple_env(self):
        args = parse_args(["--env", "A=1", "--env", "B=2", "--", "echo"])
        payload = build_payload(args)
        assert payload["environment"] == {"A": "1", "B": "2"}

    def test_env_with_equals_in_value(self):
        args = parse_args(["--env", "CMD=x=y=z", "--", "echo"])
        payload = build_payload(args)
        assert payload["environment"] == {"CMD": "x=y=z"}

    def test_env_empty_value(self):
        args = parse_args(["--env", "EMPTY=", "--", "echo"])
        payload = build_payload(args)
        assert payload["environment"] == {"EMPTY": ""}


class TestDirectory:
    """Verify directory in payload."""

    def test_no_directory_by_default(self):
        args = parse_args(["--", "echo"])
        payload = build_payload(args)
        assert "directory" not in payload

    def test_directory(self):
        args = parse_args(["-d", "/scratch/work", "--", "echo"])
        payload = build_payload(args)
        assert payload["directory"] == "/scratch/work"
