"""MCP server for Pendant Nova — local PAI memory backend only.

The omi.me cloud API has been removed. All reads/writes go through
``pai_memory``, which is backed by ``~/.claude/MEMORY/PENDANT/`` (override
with ``PAI_MEMORY_DIR``). There is intentionally NO fallback to the cloud.
"""

import json
import logging
from enum import Enum
from typing import List, Optional

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool
from pydantic import BaseModel, Field

from . import pai_memory


class MemoryCategory(str, Enum):
    core = "core"
    hobbies = "hobbies"
    lifestyle = "lifestyle"
    interests = "interests"
    habits = "habits"
    work = "work"
    skills = "skills"
    learnings = "learnings"
    other = "other"


class OmiTools(str, Enum):
    GET_MEMORIES = "get_memories"
    CREATE_MEMORY = "create_memory"
    DELETE_MEMORY = "delete_memory"
    EDIT_MEMORY = "edit_memory"
    GET_CONVERSATIONS = "get_conversations"
    GET_CONVERSATION_BY_ID = "get_conversation_by_id"


class GetMemories(BaseModel):
    categories: List[MemoryCategory] = Field(
        description="The categories of memories to filter by.", default=[]
    )
    limit: int = Field(description="The number of memories to retrieve.", default=100)


class CreateMemory(BaseModel):
    content: str = Field(description="The content of the memory.")
    category: MemoryCategory = Field(description="The category of the memory to create.")


class DeleteMemory(BaseModel):
    memory_id: str = Field(description="The ID of the memory to delete.")


class EditMemory(BaseModel):
    memory_id: str = Field(description="The ID of the memory to edit.")
    content: str = Field(description="The new content for the memory.")


class GetConversations(BaseModel):
    include_discarded: bool = Field(
        description="Whether to include discarded conversations.", default=False
    )
    limit: int = Field(description="The number of conversations to retrieve.", default=25)


class GetConversationById(BaseModel):
    conversation_id: str = Field(description="The ID of the conversation to retrieve.")


async def serve(uid: Optional[str]) -> None:
    logger = logging.getLogger(__name__)

    # Hard-fail at startup if the local PAI memory directory is missing.
    root = pai_memory.ensure_ready()
    logger.info(f"mcp-omi server started — PAI memory root: {root}")

    server = Server("mcp-omi")

    @server.list_tools()
    async def list_tools() -> list[Tool]:
        return [
            Tool(
                name=OmiTools.GET_MEMORIES,
                description="Retrieve memories from the local PAI store.",
                inputSchema=GetMemories.model_json_schema(),
            ),
            Tool(
                name=OmiTools.CREATE_MEMORY,
                description="Create a new memory in the local PAI store.",
                inputSchema=CreateMemory.model_json_schema(),
            ),
            Tool(
                name=OmiTools.DELETE_MEMORY,
                description="Delete a memory by ID from the local PAI store.",
                inputSchema=DeleteMemory.model_json_schema(),
            ),
            Tool(
                name=OmiTools.EDIT_MEMORY,
                description="Edit a memory's content in the local PAI store.",
                inputSchema=EditMemory.model_json_schema(),
            ),
            Tool(
                name=OmiTools.GET_CONVERSATIONS,
                description="Retrieve conversations from the local PAI store.",
                inputSchema=GetConversations.model_json_schema(),
            ),
            Tool(
                name=OmiTools.GET_CONVERSATION_BY_ID,
                description="Retrieve a single conversation from the local PAI store.",
                inputSchema=GetConversationById.model_json_schema(),
            ),
        ]

    @server.call_tool()
    async def call_tool(name: str, arguments: dict) -> list[TextContent]:
        logger.info(f"Calling tool: {name} with arguments: {arguments}")

        if name == OmiTools.GET_MEMORIES:
            raw_categories = arguments.get("categories", []) or []
            if not isinstance(raw_categories, list):
                raise ValueError(f"categories must be a list, got {type(raw_categories)}")
            categories = [str(c) for c in raw_categories] or None
            result = await pai_memory.list_memories(
                limit=int(arguments.get("limit", 100)),
                categories=categories,
            )
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == OmiTools.CREATE_MEMORY:
            result = await pai_memory.create_memory(
                content=arguments["content"],
                category=str(arguments["category"]),
            )
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == OmiTools.DELETE_MEMORY:
            result = await pai_memory.delete_memory(memory_id=arguments["memory_id"])
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == OmiTools.EDIT_MEMORY:
            result = await pai_memory.edit_memory(
                memory_id=arguments["memory_id"],
                content=arguments["content"],
            )
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == OmiTools.GET_CONVERSATIONS:
            result = await pai_memory.list_conversations(
                limit=int(arguments.get("limit", 25)),
                include_discarded=bool(arguments.get("include_discarded", False)),
            )
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        if name == OmiTools.GET_CONVERSATION_BY_ID:
            result = await pai_memory.get_conversation_by_id(
                conversation_id=arguments["conversation_id"],
            )
            return [TextContent(type="text", text=json.dumps(result, indent=2))]

        raise ValueError(f"Unknown tool: {name}")

    options = server.create_initialization_options()
    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, options, raise_exceptions=True)
