"""Tests for poll_job."""

from unittest.mock import MagicMock, call, patch

import pytest
import requests

from iri_run.api import API_BASE, RESOURCE, poll_job


def _mock_status_response(state, exit_code=None):
    resp = MagicMock()
    resp.ok = True
    resp.status_code = 200
    status = {"state": state}
    if exit_code is not None:
        status["exit_code"] = exit_code
    resp.json.return_value = {"id": "job-1", "status": status}
    return resp


class TestPollJobCompletion:
    """Tests for terminal job states."""

    def test_completed_exit_0(self):
        resp = _mock_status_response("completed", exit_code=0)
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 0

    def test_completed_exit_nonzero(self):
        resp = _mock_status_response("completed", exit_code=1)
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 1

    def test_completed_exit_none(self):
        resp = _mock_status_response("completed")
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 1  # None != "0"

    def test_failed(self):
        resp = _mock_status_response("failed")
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 1

    def test_canceled(self):
        resp = _mock_status_response("canceled")
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 1


class TestPollJobStateTransitions:
    """Verify state transition reporting."""

    def test_prints_state_changes(self, capsys):
        queued = _mock_status_response("queued")
        active = _mock_status_response("active")
        done = _mock_status_response("completed", exit_code=0)

        with patch("iri_run.api.requests.get", side_effect=[queued, active, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            poll_job("tok", "job-1", "/out", "/err")

        stderr = capsys.readouterr().err
        assert "queued" in stderr
        assert "active" in stderr
        assert "completed" in stderr

    def test_does_not_repeat_same_state(self, capsys):
        active1 = _mock_status_response("active")
        active2 = _mock_status_response("active")
        done = _mock_status_response("completed", exit_code=0)

        with patch("iri_run.api.requests.get", side_effect=[active1, active2, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            poll_job("tok", "job-1", "/out", "/err")

        stderr = capsys.readouterr().err
        assert stderr.count("active") == 1


class TestPollJobErrorRecovery:
    """Verify resilience to transient errors."""

    def test_continues_on_request_exception(self):
        done = _mock_status_response("completed", exit_code=0)
        with patch("iri_run.api.requests.get", side_effect=[requests.ConnectionError, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 0

    def test_continues_on_http_error(self):
        error_resp = MagicMock()
        error_resp.ok = False
        error_resp.status_code = 503
        done = _mock_status_response("completed", exit_code=0)

        with patch("iri_run.api.requests.get", side_effect=[error_resp, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", return_value=None):
            rc = poll_job("tok", "job-1", "/out", "/err")
        assert rc == 0


class TestPollJobOutputStream:
    """Verify stdout/stderr streaming during polling."""

    def test_streams_stdout(self, capsys):
        active = _mock_status_response("active")
        done = _mock_status_response("completed", exit_code=0)

        chunks = iter(["hello ", None, "world\n", None, None, None])

        def fake_read(token, path, offset, size=1048576):
            return next(chunks)

        with patch("iri_run.api.requests.get", side_effect=[active, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", side_effect=fake_read):
            poll_job("tok", "job-1", "/out", "/err")

        stdout = capsys.readouterr().out
        assert "hello " in stdout

    def test_tracks_byte_offset(self):
        active = _mock_status_response("active")
        done = _mock_status_response("completed", exit_code=0)

        read_calls = []

        def fake_read(token, path, offset, size=1048576):
            read_calls.append((path, offset))
            if path == "/out" and offset == 0:
                return "hello"  # 5 bytes
            return None

        with patch("iri_run.api.requests.get", side_effect=[active, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", side_effect=fake_read):
            poll_job("tok", "job-1", "/out", "/err")

        out_calls = [(p, o) for p, o in read_calls if p == "/out"]
        assert len(out_calls) >= 2
        assert out_calls[0] == ("/out", 0)
        assert out_calls[1] == ("/out", 5)

    def test_multibyte_offset_tracking(self):
        """Verify offset tracks bytes, not characters (for UTF-8 content)."""
        active = _mock_status_response("active")
        done = _mock_status_response("completed", exit_code=0)

        read_calls = []

        def fake_read(token, path, offset, size=1048576):
            read_calls.append((path, offset))
            if path == "/out" and offset == 0:
                return "\u00e9\u00e9"  # 2 chars, 4 bytes in UTF-8
            return None

        with patch("iri_run.api.requests.get", side_effect=[active, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", side_effect=fake_read):
            poll_job("tok", "job-1", "/out", "/err")

        out_calls = [(p, o) for p, o in read_calls if p == "/out"]
        assert out_calls[1] == ("/out", 4)

    def test_final_read_on_completion(self):
        """completed should trigger an extra stream_output call."""
        done = _mock_status_response("completed", exit_code=0)

        call_count = 0

        def fake_read(token, path, offset, size=1048576):
            nonlocal call_count
            call_count += 1
            return None

        with patch("iri_run.api.requests.get", return_value=done), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", side_effect=fake_read):
            poll_job("tok", "job-1", "/out", "/err")

        # Should read stdout+stderr at least twice (once in loop, once final)
        assert call_count >= 4

    def test_final_read_on_failure(self):
        """failed should also trigger a final stream_output call."""
        failed = _mock_status_response("failed")

        call_count = 0

        def fake_read(token, path, offset, size=1048576):
            nonlocal call_count
            call_count += 1
            return None

        with patch("iri_run.api.requests.get", return_value=failed), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.read_file_chunk", side_effect=fake_read):
            poll_job("tok", "job-1", "/out", "/err")

        assert call_count >= 4
