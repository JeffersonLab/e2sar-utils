"""Tests for iri-bash main entry points (_cmd_run and _cmd_reap)."""

from unittest.mock import MagicMock, call, patch

import pytest

from iri_run.bash import main


class TestCmdRunWait:
    """Default behavior: submit and wait."""

    def test_submits_and_polls(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")), \
             patch("iri_run.bash.poll_job", return_value=0) as mock_poll, \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit) as exc_info:
            main(["echo", "hello"])

        mock_poll.assert_called_once()
        args = mock_poll.call_args[0]
        assert args[0] == "tok"
        assert args[1] == "99"
        assert args[2].endswith("/stdout")
        assert args[3].endswith("/stderr")
        assert exc_info.value.code == 0

    def test_payload_wraps_in_bash_c(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")) as mock_submit, \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit):
            main(["echo", "hello", "world"])

        payload = mock_submit.call_args[0][1]
        assert payload["executable"] == "/bin/bash"
        assert payload["arguments"] == ["-c", "echo hello world"]

    def test_does_not_print_uuid(self, capsys):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")), \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit):
            main(["echo"])

        out = capsys.readouterr().out
        assert out == ""

    def test_reaps_on_success(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")), \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path") as mock_rm, \
             pytest.raises(SystemExit):
            main(["echo"])

        mock_rm.assert_called_once()
        path = mock_rm.call_args[0][1]
        assert "/iri-bash/" in path

    def test_does_not_reap_on_failure(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")), \
             patch("iri_run.bash.poll_job", return_value=1), \
             patch("iri_run.bash.remove_remote_path") as mock_rm, \
             pytest.raises(SystemExit) as exc_info:
            main(["echo"])

        mock_rm.assert_not_called()
        assert exc_info.value.code == 1


class TestCmdRunNoWait:
    """--no-wait: submit and print UUID."""

    def test_prints_uuid(self, capsys):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")):
            main(["--no-wait", "echo"])

        out = capsys.readouterr().out.strip()
        # Should be a 12-char hex UUID
        assert len(out) == 12
        int(out, 16)  # should not raise

    def test_does_not_poll(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")), \
             patch("iri_run.bash.poll_job") as mock_poll:
            main(["--no-wait", "echo"])

        mock_poll.assert_not_called()


class TestCmdRunOptions:
    """Verify options are passed through correctly."""

    def test_custom_account(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")) as mock_submit, \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit):
            main(["-A", "testproj", "echo"])

        payload = mock_submit.call_args[0][1]
        assert payload["attributes"]["account"] == "testproj"
        assert "/testproj/" in payload["directory"]

    def test_custom_time(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")) as mock_submit, \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit):
            main(["-t", "10", "echo"])

        payload = mock_submit.call_args[0][1]
        assert payload["attributes"]["duration"] == 600


class TestCmdRunInputSources:
    """Verify script from file and stdin."""

    def test_from_file(self, tmp_path):
        script = tmp_path / "test.sh"
        script.write_text("echo from file")
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.submit_job", return_value=("99", "queued")) as mock_submit, \
             patch("iri_run.bash.poll_job", return_value=0), \
             patch("iri_run.bash.remove_remote_path"), \
             pytest.raises(SystemExit):
            main(["-f", str(script)])

        payload = mock_submit.call_args[0][1]
        assert "echo from file" in payload["arguments"][1]


class TestCmdReap:
    """Tests for the reap subcommand."""

    def test_fetches_stdout(self, capsys):
        chunks = iter(["hello world", None])

        def fake_read(token, path, offset, size=1048576):
            if "stdout" in path:
                return next(chunks)
            return None

        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.read_file_chunk", side_effect=fake_read), \
             patch("iri_run.bash.remove_remote_path"):
            main(["reap", "abc123def456"])

        out = capsys.readouterr().out
        assert "hello world" in out

    def test_fetches_stderr(self, capsys):
        chunks = iter(["error output", None])

        def fake_read(token, path, offset, size=1048576):
            if "stderr" in path:
                return next(chunks)
            return None

        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.read_file_chunk", side_effect=fake_read), \
             patch("iri_run.bash.remove_remote_path"):
            main(["reap", "abc123def456"])

        err = capsys.readouterr().err
        assert "error output" in err

    def test_calls_remove_remote_path(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.read_file_chunk", return_value=None), \
             patch("iri_run.bash.remove_remote_path") as mock_rm:
            main(["reap", "abc123def456"])

        mock_rm.assert_called_once()
        path = mock_rm.call_args[0][1]
        assert "abc123def456" in path

    def test_uses_correct_directory(self):
        with patch("iri_run.bash.load_token", return_value="tok"), \
             patch("iri_run.bash.read_file_chunk", return_value=None) as mock_read, \
             patch("iri_run.bash.remove_remote_path"):
            main(["reap", "-A", "testproj", "myuuid"])

        read_paths = [c[0][1] for c in mock_read.call_args_list]
        assert any("/testproj/iri-bash/myuuid/stdout" in p for p in read_paths)
        assert any("/testproj/iri-bash/myuuid/stderr" in p for p in read_paths)
