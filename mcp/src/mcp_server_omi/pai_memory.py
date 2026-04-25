"""Local PAI memory backend for Pendant Nova.

Reads/writes JSON files under ~/.claude/MEMORY/PENDANT/ instead of calling
the omi.me cloud API. Override the base directory via the PAI_MEMORY_DIR
environment variable (used by tests).

Layout:
  <root>/memories/<uuid>.json
  <root>/conversations/YYYY-MM-DDTHH-MM-SS_<id>.json

This module is the ONLY place that touches the filesystem for PAI memory.
There is NO fallback to the omi.me cloud — if the directory is missing we
raise, by design, per Pendant Nova's privacy requirements.
"""

import asyncio
import json
import os
import uuid
from datetime import datetime, timezone
from pathlib import Path


def _utcnow_iso() -> str:
    # ISO-8601 UTC with explicit Z suffix, no microseconds for stable filenames.
    return datetime.now(timezone.utc).replace(microsecond=0, tzinfo=None).isoformat() + "Z"


def _root() -> Path:
    override = os.environ.get("PAI_MEMORY_DIR")
    if override:
        return Path(override).expanduser().resolve()
    return Path.home() / ".claude" / "MEMORY" / "PENDANT"


def _memories_dir() -> Path:
    return _root() / "memories"


def _conversations_dir() -> Path:
    return _root() / "conversations"


def _require_root() -> None:
    root = _root()
    if not root.exists():
        raise ValueError(
            f"PAI memory directory does not exist: {root}. "
            "Create it (memories/, conversations/) or set PAI_MEMORY_DIR. "
            "This server does NOT fall back to the omi.me cloud."
        )
    # Ensure subdirs are present — create on demand to make first-write safe.
    _memories_dir().mkdir(parents=True, exist_ok=True)
    _conversations_dir().mkdir(parents=True, exist_ok=True)


def _read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: Path, data: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    tmp.replace(path)


def _sorted_files(directory: Path) -> list[Path]:
    files = [p for p in directory.glob("*.json") if p.is_file()]
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return files


async def _run(fn, *args, **kwargs):
    # Tiny helper so callers can `await` filesystem work without blocking the loop.
    return await asyncio.to_thread(fn, *args, **kwargs)


def _list_memories_sync(limit: int, categories: list[str] | None) -> list[dict]:
    _require_root()
    out: list[dict] = []
    for path in _sorted_files(_memories_dir()):
        try:
            mem = _read_json(path)
        except (OSError, json.JSONDecodeError):
            continue
        if categories and mem.get("category") not in categories:
            continue
        out.append(mem)
        if len(out) >= limit:
            break
    return out


def _create_memory_sync(content: str, category: str) -> dict:
    _require_root()
    mem_id = str(uuid.uuid4())
    now = _utcnow_iso()
    mem = {
        "id": mem_id,
        "content": content,
        "category": category,
        "created_at": now,
        "updated_at": now,
    }
    _write_json(_memories_dir() / f"{mem_id}.json", mem)
    return mem


def _edit_memory_sync(memory_id: str, content: str) -> dict:
    _require_root()
    path = _memories_dir() / f"{memory_id}.json"
    if not path.exists():
        raise ValueError(f"Memory not found: {memory_id}")
    mem = _read_json(path)
    mem["content"] = content
    mem["updated_at"] = _utcnow_iso()
    _write_json(path, mem)
    return mem


def _delete_memory_sync(memory_id: str) -> dict:
    _require_root()
    path = _memories_dir() / f"{memory_id}.json"
    if not path.exists():
        raise ValueError(f"Memory not found: {memory_id}")
    path.unlink()
    return {"id": memory_id, "deleted": True}


def _list_conversations_sync(limit: int, include_discarded: bool) -> list[dict]:
    _require_root()
    out: list[dict] = []
    for path in _sorted_files(_conversations_dir()):
        try:
            conv = _read_json(path)
        except (OSError, json.JSONDecodeError):
            continue
        if not include_discarded and conv.get("discarded"):
            continue
        out.append(conv)
        if len(out) >= limit:
            break
    return out


def _get_conversation_by_id_sync(conversation_id: str) -> dict:
    _require_root()
    # Files are prefixed with timestamp + id; match by suffix `_<id>.json`.
    for path in _conversations_dir().glob(f"*_{conversation_id}.json"):
        return _read_json(path)
    # Also accept a plain `<id>.json` fallback.
    direct = _conversations_dir() / f"{conversation_id}.json"
    if direct.exists():
        return _read_json(direct)
    raise ValueError(f"Conversation not found: {conversation_id}")


async def list_memories(limit: int = 100, categories: list[str] | None = None) -> list[dict]:
    return await _run(_list_memories_sync, limit, categories)


async def create_memory(content: str, category: str) -> dict:
    return await _run(_create_memory_sync, content, category)


async def edit_memory(memory_id: str, content: str) -> dict:
    return await _run(_edit_memory_sync, memory_id, content)


async def delete_memory(memory_id: str) -> dict:
    return await _run(_delete_memory_sync, memory_id)


async def list_conversations(limit: int = 25, include_discarded: bool = False) -> list[dict]:
    return await _run(_list_conversations_sync, limit, include_discarded)


async def get_conversation_by_id(conversation_id: str) -> dict:
    return await _run(_get_conversation_by_id_sync, conversation_id)


def ensure_ready() -> Path:
    """Used by server startup to fail fast if MEMORY dir is missing."""
    _require_root()
    return _root()
