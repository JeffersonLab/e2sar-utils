"""Tests for iri-bash payload construction."""

from iri_run.bash import _build_run_payload, _job_dir, parse_args


class TestJobDir:
    def test_default_account(self):
        assert _job_dir("myproject", "abc123") == "/global/cfs/cdirs/myproject/iri-bash/abc123"

    def test_other_account(self):
        assert _job_dir("testproj", "xyz") == "/global/cfs/cdirs/testproj/iri-bash/xyz"


class TestBuildRunPayload:
    """Verify the run payload structure."""

    def test_executable_is_bash(self):
        args = parse_args(["echo", "hello"])
        payload = _build_run_payload(args, "abc123", "echo hello")
        assert payload["executable"] == "/bin/bash"

    def test_arguments_are_bash_c(self):
        args = parse_args(["echo", "hello"])
        payload = _build_run_payload(args, "abc123", "echo hello")
        assert payload["arguments"] == ["-c", "echo hello"]

    def test_name(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["name"] == "iri-bash-abc123"

    def test_directory(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["directory"] == "/global/cfs/cdirs/myproject/iri-bash/abc123"

    def test_stdout_path(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["stdout_path"] == "/global/cfs/cdirs/myproject/iri-bash/abc123/stdout"

    def test_stderr_path(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["stderr_path"] == "/global/cfs/cdirs/myproject/iri-bash/abc123/stderr"

    def test_resources_no_node_count(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert "node_count" not in payload["resources"]
        assert payload["resources"]["process_count"] == 1

    def test_queue_name_is_cron(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["queue_name"] == "cron"

    def test_constraint_is_cron(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["custom_attributes"]["constraint"] == "cron"

    def test_duration_default(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["duration"] == 30 * 60

    def test_duration_custom(self):
        args = parse_args(["-t", "10", "echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["duration"] == 10 * 60

    def test_account_default(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["account"] == "myproject"

    def test_account_custom(self):
        args = parse_args(["-A", "testproj", "echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert payload["attributes"]["account"] == "testproj"
        assert "/testproj/" in payload["directory"]

    def test_multiline_script(self):
        script = "module load python; python my_script.py"
        args = parse_args(["dummy"])
        payload = _build_run_payload(args, "abc", script)
        assert script in payload["arguments"][1]

    def test_no_environment(self):
        args = parse_args(["echo"])
        payload = _build_run_payload(args, "abc123", "echo")
        assert "environment" not in payload


