"""Tests for the main() entry point."""

from unittest.mock import MagicMock, patch

import pytest

from iri_run.cli import main


class TestMainNoWait:
    """Tests for main() without --wait."""

    def test_submits_and_prints_job_id(self, capsys):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["--", "echo", "hi"])

        out = capsys.readouterr().out.strip()
        assert out == "12345"

    def test_does_not_poll(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.poll_job") as mock_poll, \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["--", "echo"])

        mock_poll.assert_not_called()

    def test_ensures_remote_dir_always(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.ensure_remote_dir") as mock_mkdir:
            main(["--", "echo"])

        mock_mkdir.assert_called_once()
        path_arg = mock_mkdir.call_args[0][1]
        assert "iri-run" in path_arg


class TestMainWithWait:
    """Tests for main() with --wait."""

    def test_ensures_remote_dir(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.ensure_remote_dir") as mock_mkdir, \
             patch("iri_run.cli.poll_job", return_value=0), \
             pytest.raises(SystemExit):
            main(["-w", "--", "echo"])

        mock_mkdir.assert_called_once()
        path_arg = mock_mkdir.call_args[0][1]
        assert "iri-run" in path_arg

    def test_calls_poll_job(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.ensure_remote_dir"), \
             patch("iri_run.cli.poll_job", return_value=0) as mock_poll, \
             pytest.raises(SystemExit):
            main(["-w", "--", "echo"])

        mock_poll.assert_called_once()
        args = mock_poll.call_args[0]
        assert args[0] == "tok"
        assert args[1] == "12345"
        assert args[2].endswith(".out")
        assert args[3].endswith(".err")

    def test_does_not_print_job_id(self, capsys):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("12345", "PENDING")), \
             patch("iri_run.cli.ensure_remote_dir"), \
             patch("iri_run.cli.poll_job", return_value=0), \
             pytest.raises(SystemExit):
            main(["-w", "--", "echo"])

        out = capsys.readouterr().out
        assert "12345" not in out


class TestMainPayloadContent:
    """Verify main passes correctly-built payloads to submit_job."""

    def test_payload_has_correct_executable(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("1", "OK")) as mock_submit, \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["--", "python", "script.py"])

        payload = mock_submit.call_args[0][1]
        assert payload["executable"] == "python"
        assert payload["arguments"] == ["script.py"]

    def test_payload_uses_custom_options(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("1", "OK")) as mock_submit, \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["-A", "testproj", "-q", "debug", "-N", "4", "--", "srun", "hostname"])

        payload = mock_submit.call_args[0][1]
        assert payload["attributes"]["account"] == "testproj"
        assert payload["attributes"]["custom_attributes"]["qos"] == "debug"
        assert payload["resources"]["node_count"] == 4

    def test_payload_name_contains_uuid(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("1", "OK")) as mock_submit, \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["--", "echo"])

        payload = mock_submit.call_args[0][1]
        assert payload["name"].startswith("iri-run-")
        # The hint portion should be a 12-char hex string
        hint = payload["name"][len("iri-run-"):]
        assert len(hint) == 12
        int(hint, 16)  # should not raise

    def test_output_paths_match_job_name(self):
        with patch("iri_run.cli.load_token", return_value="tok"), \
             patch("iri_run.cli.submit_job", return_value=("1", "OK")) as mock_submit, \
             patch("iri_run.cli.ensure_remote_dir"):
            main(["--", "echo"])

        payload = mock_submit.call_args[0][1]
        hint = payload["name"][len("iri-run-"):]
        assert payload["stdout_path"].endswith(f"/{hint}.out")
        assert payload["stderr_path"].endswith(f"/{hint}.err")
