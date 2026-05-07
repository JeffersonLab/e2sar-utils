"""Tests for API interaction functions: submit_job, wait_for_task, read_file_chunk, ensure_remote_dir."""

import time
from unittest.mock import MagicMock, patch

import pytest
import requests

from iri_run.api import (
    API_BASE,
    FS_RESOURCE,
    RESOURCE,
    auth_headers,
    ensure_remote_dir,
    read_file_chunk,
    wait_for_task,
    submit_job,
)


class TestAuthHeaders:
    def test_format(self):
        assert auth_headers("mytoken") == {"Authorization": "Bearer mytoken"}

    def test_empty_token(self):
        assert auth_headers("") == {"Authorization": "Bearer "}


class TestSubmitJob:
    """Tests for submit_job."""

    def _mock_session(self, status_code, json_data=None, text=""):
        resp = MagicMock()
        resp.status_code = status_code
        resp.ok = 200 <= status_code < 300
        resp.reason = "OK" if resp.ok else "Error"
        resp.json.return_value = json_data or {}
        resp.text = text
        resp.headers = {}
        mock_session = MagicMock()
        mock_session.return_value.send.return_value = resp
        return mock_session, resp

    def test_success(self):
        mock_session, _ = self._mock_session(200, {"id": "12345", "status": {"state": "queued"}})
        with patch("iri_run.api.requests.Session", mock_session):
            job_id, state = submit_job("tok", {"name": "test"})
        assert job_id == "12345"
        assert state == "queued"

    def test_state_defaults_to_unknown(self):
        mock_session, _ = self._mock_session(200, {"id": "12345"})
        with patch("iri_run.api.requests.Session", mock_session):
            _, state = submit_job("tok", {})
        assert state is None

    def test_http_error_exits(self):
        mock_session, _ = self._mock_session(403, {"error": "forbidden"})
        with patch("iri_run.api.requests.Session", mock_session), \
             pytest.raises(SystemExit):
            submit_job("tok", {})

    def test_http_error_non_json_exits(self):
        mock_session, resp = self._mock_session(500, text="Internal Server Error")
        resp.json.side_effect = ValueError("not json")
        with patch("iri_run.api.requests.Session", mock_session), \
             pytest.raises(SystemExit):
            submit_job("tok", {})

    def test_no_id_exits(self):
        mock_session, _ = self._mock_session(200, {"status": {"state": "queued"}})
        with patch("iri_run.api.requests.Session", mock_session), \
             pytest.raises(SystemExit):
            submit_job("tok", {})

    def test_sends_correct_url(self):
        mock_session, _ = self._mock_session(200, {"id": "1", "status": {"state": "queued"}})
        with patch("iri_run.api.requests.Session", mock_session):
            submit_job("tok", {"name": "test"})
        prepared = mock_session.return_value.send.call_args[0][0]
        assert prepared.url == f"{API_BASE}/compute/job/{RESOURCE}"

    def test_sends_auth_header(self):
        mock_session, _ = self._mock_session(200, {"id": "1", "status": {"state": "queued"}})
        with patch("iri_run.api.requests.Session", mock_session):
            submit_job("my-secret-token", {})
        prepared = mock_session.return_value.send.call_args[0][0]
        assert prepared.headers["Authorization"] == "Bearer my-secret-token"

    def test_sends_json_payload(self):
        mock_session, _ = self._mock_session(200, {"id": "1", "status": {"state": "queued"}})
        payload = {"name": "test", "executable": "/bin/echo"}
        with patch("iri_run.api.requests.Session", mock_session):
            submit_job("tok", payload)
        prepared = mock_session.return_value.send.call_args[0][0]
        import json as _json
        assert _json.loads(prepared.body) == payload


class TestWaitForTask:
    """Tests for wait_for_task."""

    def _mock_response(self, status_code, json_data):
        resp = MagicMock()
        resp.status_code = status_code
        resp.ok = 200 <= status_code < 300
        resp.json.return_value = json_data
        return resp

    def test_completed_returns_result(self):
        resp = self._mock_response(200, {"status": "completed", "result": {"data": "hello"}})
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1]):
            result = wait_for_task("tok", "task-1")
        assert result == {"data": "hello"}

    def test_failed_returns_none(self):
        resp = self._mock_response(200, {"status": "failed"})
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1]):
            assert wait_for_task("tok", "task-1") is None

    def test_canceled_returns_none(self):
        resp = self._mock_response(200, {"status": "canceled"})
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1]):
            assert wait_for_task("tok", "task-1") is None

    def test_timeout_returns_none(self):
        resp = self._mock_response(200, {"status": "active"})
        # monotonic: start=0, check=0 (enter loop), check=999 (exceeds deadline)
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1, 999]):
            assert wait_for_task("tok", "task-1") is None

    def test_polls_until_complete(self):
        pending = self._mock_response(200, {"status": "pending"})
        active = self._mock_response(200, {"status": "active"})
        done = self._mock_response(200, {"status": "completed", "result": "ok"})

        with patch("iri_run.api.requests.get", side_effect=[pending, active, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1, 2, 3]):
            assert wait_for_task("tok", "task-1") == "ok"

    def test_retries_on_request_exception(self):
        done = self._mock_response(200, {"status": "completed", "result": "ok"})

        with patch("iri_run.api.requests.get", side_effect=[requests.ConnectionError, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1, 2]):
            assert wait_for_task("tok", "task-1") == "ok"

    def test_retries_on_http_error(self):
        error_resp = self._mock_response(500, {})
        done = self._mock_response(200, {"status": "completed", "result": "ok"})

        with patch("iri_run.api.requests.get", side_effect=[error_resp, done]), \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1, 2]):
            assert wait_for_task("tok", "task-1") == "ok"

    def test_sends_correct_url(self):
        resp = self._mock_response(200, {"status": "completed", "result": None})
        with patch("iri_run.api.requests.get", return_value=resp) as mock_get, \
             patch("iri_run.api.time.sleep"), \
             patch("iri_run.api.time.monotonic", side_effect=[0, 1]):
            wait_for_task("tok", "task-abc")
        assert mock_get.call_args[0][0] == f"{API_BASE}/task/task-abc"


class TestReadFileChunk:
    """Tests for read_file_chunk."""

    def _mock_response(self, status_code, json_data):
        resp = MagicMock()
        resp.status_code = status_code
        resp.ok = 200 <= status_code < 300
        resp.json.return_value = json_data
        return resp

    def test_returns_string_result(self):
        view_resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=view_resp), \
             patch("iri_run.api.wait_for_task", return_value="hello world"):
            assert read_file_chunk("tok", "/path/out", 0) == "hello world"

    def test_returns_nested_output_content(self):
        """The real API returns {"output": {"content": "...", ...}}."""
        view_resp = self._mock_response(200, {"task_id": "t1"})
        result = {"output": {"content": "data here", "content_type": "bytes", "start_position": 0}}
        with patch("iri_run.api.requests.get", return_value=view_resp), \
             patch("iri_run.api.wait_for_task", return_value=result):
            assert read_file_chunk("tok", "/path/out", 0) == "data here"

    def test_returns_string_output(self):
        view_resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=view_resp), \
             patch("iri_run.api.wait_for_task", return_value={"output": "plain string"}):
            assert read_file_chunk("tok", "/path/out", 0) == "plain string"

    def test_returns_flat_content(self):
        view_resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=view_resp), \
             patch("iri_run.api.wait_for_task", return_value={"content": "flat"}):
            assert read_file_chunk("tok", "/path/out", 0) == "flat"

    def test_returns_empty_for_unknown_dict(self):
        view_resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=view_resp), \
             patch("iri_run.api.wait_for_task", return_value={"unknown_key": "val"}):
            assert read_file_chunk("tok", "/path/out", 0) == ""

    def test_returns_none_on_request_exception(self):
        with patch("iri_run.api.requests.get", side_effect=requests.ConnectionError):
            assert read_file_chunk("tok", "/path/out", 0) is None

    def test_returns_none_on_http_error(self):
        resp = self._mock_response(500, {})
        with patch("iri_run.api.requests.get", return_value=resp):
            assert read_file_chunk("tok", "/path/out", 0) is None

    def test_returns_none_when_no_task_id(self):
        resp = self._mock_response(200, {"something": "else"})
        with patch("iri_run.api.requests.get", return_value=resp):
            assert read_file_chunk("tok", "/path/out", 0) is None

    def test_returns_none_when_task_fails(self):
        resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.wait_for_task", return_value=None):
            assert read_file_chunk("tok", "/path/out", 0) is None

    def test_returns_none_for_non_str_non_dict_result(self):
        resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=resp), \
             patch("iri_run.api.wait_for_task", return_value=12345):
            assert read_file_chunk("tok", "/path/out", 0) is None

    def test_passes_offset_and_size(self):
        resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=resp) as mock_get, \
             patch("iri_run.api.wait_for_task", return_value=""):
            read_file_chunk("tok", "/my/file", 1024, size=4096)
        params = mock_get.call_args[1]["params"]
        assert params == {"path": "/my/file", "offset": 1024, "size": 4096}

    def test_default_size(self):
        resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.get", return_value=resp) as mock_get, \
             patch("iri_run.api.wait_for_task", return_value=""):
            read_file_chunk("tok", "/my/file", 0)
        params = mock_get.call_args[1]["params"]
        assert params["size"] == 1048576


class TestEnsureRemoteDir:
    """Tests for ensure_remote_dir."""

    def _mock_response(self, status_code, json_data):
        resp = MagicMock()
        resp.status_code = status_code
        resp.ok = 200 <= status_code < 300
        resp.json.return_value = json_data
        return resp

    def test_creates_dir_and_waits(self):
        resp = self._mock_response(200, {"task_id": "t-mkdir"})
        with patch("iri_run.api.requests.post", return_value=resp) as mock_post, \
             patch("iri_run.api.wait_for_task") as mock_wait:
            ensure_remote_dir("tok", "/my/dir")
        mock_post.assert_called_once()
        assert mock_post.call_args[1]["json"] == {"path": "/my/dir", "parent": True}
        mock_wait.assert_called_once_with("tok", "t-mkdir")

    def test_no_task_id_skips_wait(self):
        resp = self._mock_response(200, {})
        with patch("iri_run.api.requests.post", return_value=resp), \
             patch("iri_run.api.wait_for_task") as mock_wait:
            ensure_remote_dir("tok", "/my/dir")
        mock_wait.assert_not_called()

    def test_http_error_is_nonfatal(self):
        resp = self._mock_response(500, {})
        with patch("iri_run.api.requests.post", return_value=resp), \
             patch("iri_run.api.wait_for_task") as mock_wait:
            ensure_remote_dir("tok", "/my/dir")  # should not raise
        mock_wait.assert_not_called()

    def test_request_exception_is_nonfatal(self):
        with patch("iri_run.api.requests.post", side_effect=requests.ConnectionError):
            ensure_remote_dir("tok", "/my/dir")  # should not raise

    def test_sends_correct_url(self):
        resp = self._mock_response(200, {"task_id": "t1"})
        with patch("iri_run.api.requests.post", return_value=resp) as mock_post, \
             patch("iri_run.api.wait_for_task"):
            ensure_remote_dir("tok", "/some/path")
        url = mock_post.call_args[0][0]
        assert url == f"{API_BASE}/filesystem/mkdir/{FS_RESOURCE}"
