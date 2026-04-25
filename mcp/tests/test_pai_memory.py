"""Tests for the local PAI memory backend.

Each test sets PAI_MEMORY_DIR to a pytest tmp_path so we never touch the
real ~/.claude/MEMORY/PENDANT/ store.
"""

import asyncio
import os
from pathlib import Path

import pytest

from mcp_server_omi import pai_memory


def _prep(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setenv("PAI_MEMORY_DIR", str(tmp_path))
    (tmp_path / "memories").mkdir(parents=True, exist_ok=True)
    (tmp_path / "conversations").mkdir(parents=True, exist_ok=True)
    return tmp_path


def test_create_and_list_memory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _prep(tmp_path, monkeypatch)

    created = asyncio.run(pai_memory.create_memory("hello pendant", "work"))
    assert created["content"] == "hello pendant"
    assert created["category"] == "work"
    assert created["id"]

    listed = asyncio.run(pai_memory.list_memories(limit=10))
    assert len(listed) == 1
    assert listed[0]["id"] == created["id"]
    assert listed[0]["content"] == "hello pendant"


def test_edit_memory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _prep(tmp_path, monkeypatch)

    created = asyncio.run(pai_memory.create_memory("v1", "personal"))
    edited = asyncio.run(pai_memory.edit_memory(created["id"], "v2"))

    assert edited["id"] == created["id"]
    assert edited["content"] == "v2"
    assert edited["created_at"] == created["created_at"]
    # updated_at may equal created_at at second resolution; just assert presence.
    assert edited["updated_at"]

    listed = asyncio.run(pai_memory.list_memories(limit=10))
    assert listed[0]["content"] == "v2"


def test_delete_memory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _prep(tmp_path, monkeypatch)

    created = asyncio.run(pai_memory.create_memory("ephemeral", "tech"))
    res = asyncio.run(pai_memory.delete_memory(created["id"]))
    assert res == {"id": created["id"], "deleted": True}

    listed = asyncio.run(pai_memory.list_memories(limit=10))
    assert listed == []
