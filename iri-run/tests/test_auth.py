"""Tests for authentication functions."""

import subprocess
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from iri_run.api import (
    IRI_RESOURCE_SERVER,
    IRI_SCOPE,
    _find_globus_cli,
    _load_token_from_store,
    _run_globus,
    load_token,
)


class TestFindGlobusCli:
    """Tests for _find_globus_cli."""

    def test_found_on_path(self):
        with patch("iri_run.api.shutil.which", return_value="/usr/bin/globus"):
            assert _find_globus_cli() == "/usr/bin/globus"

    def test_fallback_to_local_bin(self, tmp_path):
        fake_home = tmp_path / "home"
        fake_globus = fake_home / ".local" / "bin" / "globus"
        fake_globus.parent.mkdir(parents=True)
        fake_globus.touch()

        with patch("iri_run.api.shutil.which", return_value=None), \
             patch("iri_run.api.Path.home", return_value=fake_home):
            assert _find_globus_cli() == str(fake_globus)

    def test_not_found_exits(self, tmp_path):
        fake_home = tmp_path / "home"
        fake_home.mkdir()

        with patch("iri_run.api.shutil.which", return_value=None), \
             patch("iri_run.api.Path.home", return_value=fake_home), \
             pytest.raises(SystemExit):
            _find_globus_cli()


class TestRunGlobus:
    """Tests for _run_globus."""

    def test_success(self):
        with patch("iri_run.api.subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(returncode=0)
            _run_globus("/usr/bin/globus", ["login"])
            mock_run.assert_called_once_with(["/usr/bin/globus", "login"])

    def test_failure_exits(self):
        with patch("iri_run.api.subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(returncode=1)
            with pytest.raises(SystemExit):
                _run_globus("/usr/bin/globus", ["login"])

    def test_passes_all_args(self):
        with patch("iri_run.api.subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(returncode=0)
            _run_globus("/usr/bin/globus", ["session", "consent", "some-scope"])
            mock_run.assert_called_once_with(
                ["/usr/bin/globus", "session", "consent", "some-scope"]
            )


class TestLoadTokenFromStore:
    """Tests for _load_token_from_store."""

    def _make_storage_mock(self, token_data):
        mock_cls = MagicMock()
        mock_instance = MagicMock()
        mock_instance.get_token_data.return_value = token_data
        mock_cls.return_value = mock_instance
        return mock_cls

    def test_returns_token_data_when_valid(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        token_data = {"access_token": "tok123", "expires_at_seconds": 9999999999}
        mock_cls = self._make_storage_mock(token_data)
        mock_storage_mod = MagicMock()
        mock_storage_mod.SQLiteTokenStorage = mock_cls

        with patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch.dict("sys.modules", {"globus_sdk.tokenstorage": mock_storage_mod, "globus_sdk": MagicMock()}):
            result = _load_token_from_store()

        assert result == token_data

    def test_returns_none_when_no_db(self, tmp_path):
        with patch("iri_run.api.Path.home", return_value=tmp_path):
            assert _load_token_from_store() is None

    def test_returns_none_when_no_token(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        mock_cls = self._make_storage_mock(None)
        mock_storage_mod = MagicMock()
        mock_storage_mod.SQLiteTokenStorage = mock_cls

        with patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch.dict("sys.modules", {"globus_sdk.tokenstorage": mock_storage_mod, "globus_sdk": MagicMock()}):
            assert _load_token_from_store() is None

    def test_returns_none_when_empty_access_token(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        mock_cls = self._make_storage_mock({"access_token": "", "expires_at_seconds": 0})
        mock_storage_mod = MagicMock()
        mock_storage_mod.SQLiteTokenStorage = mock_cls

        with patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch.dict("sys.modules", {"globus_sdk.tokenstorage": mock_storage_mod, "globus_sdk": MagicMock()}):
            assert _load_token_from_store() is None

    def test_returns_none_when_expired(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        mock_cls = self._make_storage_mock({"access_token": "tok", "expires_at_seconds": 1})
        mock_storage_mod = MagicMock()
        mock_storage_mod.SQLiteTokenStorage = mock_cls

        with patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch.dict("sys.modules", {"globus_sdk.tokenstorage": mock_storage_mod, "globus_sdk": MagicMock()}):
            assert _load_token_from_store() is None

    def test_returns_none_on_storage_exception(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        mock_cls = MagicMock(side_effect=RuntimeError("corrupt db"))
        mock_storage_mod = MagicMock()
        mock_storage_mod.SQLiteTokenStorage = mock_cls

        with patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch.dict("sys.modules", {"globus_sdk.tokenstorage": mock_storage_mod, "globus_sdk": MagicMock()}):
            assert _load_token_from_store() is None


class TestLoadToken:
    """Tests for the top-level load_token orchestrator."""

    def test_returns_token_when_already_valid(self):
        with patch("iri_run.api._find_globus_cli", return_value="/usr/bin/globus"), \
             patch("iri_run.api._load_token_from_store", return_value={"access_token": "good-token"}):
            assert load_token() == "good-token"

    def test_runs_login_when_no_db(self, tmp_path):
        call_count = 0

        def fake_load():
            nonlocal call_count
            call_count += 1
            if call_count >= 3:
                return {"access_token": "new-token"}
            return None

        with patch("iri_run.api._find_globus_cli", return_value="/usr/bin/globus"), \
             patch("iri_run.api._load_token_from_store", side_effect=fake_load), \
             patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch("iri_run.api._run_globus") as mock_run:
            token = load_token()

        assert token == "new-token"
        calls = [c[0][1] for c in mock_run.call_args_list]
        assert ["login"] in calls

    def test_runs_consent_when_no_iri_token(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        call_count = 0

        def fake_load():
            nonlocal call_count
            call_count += 1
            if call_count >= 3:
                return {"access_token": "consented-token"}
            return None

        with patch("iri_run.api._find_globus_cli", return_value="/usr/bin/globus"), \
             patch("iri_run.api._load_token_from_store", side_effect=fake_load), \
             patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch("iri_run.api._run_globus") as mock_run:
            token = load_token()

        assert token == "consented-token"
        calls = [c[0][1] for c in mock_run.call_args_list]
        assert ["session", "consent", IRI_SCOPE] in calls

    def test_exits_when_all_attempts_fail(self, tmp_path):
        db = tmp_path / ".globus" / "cli" / "storage.db"
        db.parent.mkdir(parents=True)
        db.touch()

        with patch("iri_run.api._find_globus_cli", return_value="/usr/bin/globus"), \
             patch("iri_run.api._load_token_from_store", return_value=None), \
             patch("iri_run.api.Path.home", return_value=tmp_path), \
             patch("iri_run.api._run_globus"), \
             pytest.raises(SystemExit):
            load_token()
