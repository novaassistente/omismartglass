# mcp-server-omi: A OMI MCP server

## PAI Local Mode

This fork is part of **Pendant Nova** and replaces the omi.me cloud API with a
local file-based store under PAI's `MEMORY/` tree. There is **no** network
call, no API key, and no fallback to the omi.me cloud — by design, for
privacy.

- **Default root:** `~/.claude/MEMORY/PENDANT/`
  - `memories/<uuid>.json` — one file per memory
  - `conversations/YYYY-MM-DDTHH-MM-SS_<id>.json` — one file per conversation
- **Override:** set `PAI_MEMORY_DIR=/some/other/path` (used by tests and for
  alternate stores). The path is `expanduser`'d.
- **Startup check:** the server hard-fails if the root directory is missing.
  It will not silently create-and-use a different store and it will not call
  the cloud.
- **No API key required.** `OMI_API_KEY` and `OMI_API_BASE_URL` are ignored.
- **Backend:** `mcp_server_omi.pai_memory` — all reads/writes happen there.

The omi.me cloud branch has been removed from `server.py`; the MCP tool
surface (`get_memories`, `create_memory`, `edit_memory`, `delete_memory`,
`get_conversations`, `get_conversation_by_id`) is preserved.

---

## Overview

A Model Context Protocol server for Omi interaction and automation. This server provides tools to read, search, and manipulate Memories and Conversations.

### Tools
1. `get_memories`
   - Retrieve a list of user memories
   - Inputs:
     - `limit` (number, optional): Maximum number of memories to retrieve (default: 100)
     - `categories` (array of MemoryFilterOptions, optional): Categories of memories to retrieve (default: [])
   - Returns: JSON object containing list of memories

2. `create_memory`
   - Create a new memory
   - Inputs:
     - `content` (string): Content of the memory
     - `category` (MemoryFilterOptions): Category of the memory
   - Returns: Created memory object

3. `delete_memory`
   - Delete a memory by ID
   - Inputs:
     - `memory_id` (string): ID of the memory to delete
   - Returns: Status of the operation

4. `edit_memory`
   - Edit a memory's content
   - Inputs:
     - `memory_id` (string): ID of the memory to edit
     - `content` (string): New content for the memory
   - Returns: Status of the operation

5. `get_conversations`
   - Retrieve a list of user conversations
   - Inputs:
     - `include_discarded` (boolean, optional): Whether to include discarded conversations (default: false)
     - `limit` (number, optional): Maximum number of conversations to retrieve (default: 25)
   - Returns: List of conversation objects containing transcripts, timestamps, geolocation and structured summaries

## Configuration

### API Key

To use the Omi MCP server, you need an API key. You can generate one in the Omi app under `Settings > Developer > MCP`. The API key can be provided with each tool call. If not provided, the server will use the `OMI_API_KEY` environment variable as a fallback.

### Usage with Claude Desktop

Add this to your `claude_desktop_config.json`:

<details>
<summary>Using docker</summary>

Install docker, https://orbstack.dev/ is great.

Replace `your_api_key_here` with the key you generated in the Omi app.

```json
"mcpServers": {
  "omi": {
    "command": "docker",
    "args": ["run", "--rm", "-i", "-e", "OMI_API_KEY=your_api_key_here", "omiai/mcp-server"]
  }
}
```
</details>

<!-- <details>
<summary>Using pip installation</summary>

Requires python >= 3.11.6. 
- Check `python --version`, and `brew list --versions | grep python` (you might have other versions of python installed)
- Get the path of the python version (`which python`) or with brew

```json
"mcpServers": {
  "omi": {
    "command": "/opt/homebrew/bin/python3.12",
    "args": ["-m", "mcp_server_omi"]
  }
}
```
</details> -->

## Debugging

You can use the MCP inspector to debug the server. For uvx installations:

```
npx @modelcontextprotocol/inspector uvx mcp-server-omi
```

Or if you've installed the package in a specific directory or are developing on it:

```
cd path/to/servers/src/omi
npx @modelcontextprotocol/inspector uv run mcp-server-omi
```

Running `tail -n 20 -f ~/Library/Logs/Claude/mcp-server-omi.log` will show the logs from the server and may
help you debug any issues.

## Advanced

### Custom Backend URL

If you are self-hosting the Omi backend, you can specify the API endpoint by setting the `OMI_API_BASE_URL` environment variable.

```bash
export OMI_API_BASE_URL="https://your-backend-url.com"
```

## License

This MCP server is licensed under the MIT License. This means you are free to use, modify, and distribute the software, subject to the terms and conditions of the MIT License. For more details, please see the LICENSE file in the project repository.
