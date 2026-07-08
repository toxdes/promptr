# Promptr Provider Protocol v1.0

JSON-RPC 2.0 over stdin/stdout for provider plugins.

## Transport

- **stdin/stdout** — one line-delimited JSON message per line (`\n`).
- **stderr** — reserved for plugin diagnostics, logged by promptr but never parsed.
- Promptr closes stdin to signal shutdown. Plugin exits when stdin closes.
- Content-Type: `application/json` — one object per line, no trailing whitespace.

## Message Format

Every message is a JSON-RPC 2.0 object. Two kinds:

### Request (promptr → plugin)

```json
{"jsonrpc":"2.0","id":1,"method":"provider/submit",
 "params":{...}}
```

`id` is an integer. Plugin must respond with a matching `result` or `error` (except for `shutdown`, which has no response).

### Response (plugin → promptr)

```json
{"jsonrpc":"2.0","id":1,"result":{"ok":true}}
// or
{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"Something broke"}}
```

### Notification (plugin → promptr, no response expected)

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{"tab_id":"ses_abc","type":"chunk","output":"..."}}
```

Notifications have no `id`. Promptr never responds.

---

## Lifecycle

```
    ┌──────────────────────────────────────────────────┐
    │                  LIFETIME                        │
    │                                                  │
    │  promptr              plugin                     │
    │    │                    │                         │
    │    ├─ provider/initialize ─►│                     │
    │    │◄─ provider/initialized ─┤                     │
    │    │                    │                         │
    │    ├─ provider/submit ──►│  (per tab submit)      │
    │    │◄─ provider/event ────┤  (chunk, tool_call)   │
    │    │    ...               │                        │
    │    │◄─ provider/event ────┤  (done / error)       │
    │    │                    │                         │
    │    ├─ provider/cancel ──►│  (optional)            │
    │    │                    │                         │
    │    ├─ provider/cleanup_session ─►│ (tab closed)   │
    │    │                    │                         │
    │    ├─ provider/shutdown──►│                      │
    │    │  [stdin close]      │                        │
    │    │                    │  [process exits]        │
    └──────────────────────────────────────────────────┘
```

---

## 1. Initialize

Promptr sends this once at process start. Plugin responds with its capabilities.

### Request

```json
{"jsonrpc":"2.0","id":1,"method":"provider/initialize",
 "params":{
   "protocol_version":"1.0",
   "client_capabilities":{
     "tool_execution":true,
     "agent_content":true,
     "agent_resolution":true,
     "streaming":true
   },
   "config":{
     "opencode_path":"opencode",
     "openrouter_api_key":"sk-...",
     "openrouter_base_url":"https://openrouter.ai/api/v1"
   }
 }}
```

- `protocol_version` — semver string. Promptr sends the version it speaks. Plugin must be compatible or return an error.
- `client_capabilities` — what promptr supports.
- `config` — provider-specific config keys merged from promptr's config file.

### Response

```json
{"jsonrpc":"2.0","id":1,"result":{
   "protocol_version":"1.0",
   "server_capabilities":{
     "streaming":true,
     "tools":false,
     "agent_mode":"content",
     "models":true,
     "agents":false
   }
 }}
```

#### agent_mode

| Value | Meaning |
|-------|---------|
| `"content"` | Promptr sends the full parsed agent body text. Plugin uses it as-is (e.g. system prompt for OpenRouter). |
| `"resolution"` | Promptr sends only the agent name. Plugin resolves the `.md` file itself (e.g. opencode looks in its known paths). |

#### tools

| Value | Meaning |
|-------|---------|
| `false` | Plugin does NOT use tools. Promptr expects text-only output. |
| `"internal"` | Plugin has its own tool loop. Promptr receives only text. Tool calls never cross the protocol boundary. |
| `"external"` | Plugin sends `tool_call` events back to promptr for execution. Promptr executes, returns results via `provider/tool_result`. |

### Error

```json
{"jsonrpc":"2.0","id":1,"error":{
   "code":-32001,
   "message":"Protocol version 1.0 not supported. This plugin requires 1.2+"
 }}
```

Promptr will show a user-facing error and fall back to opencode.

---

## 2. Submit

Sent each time the user submits a query. One submit at a time per `tab_id`.

### Request

```json
{"jsonrpc":"2.0","id":2,"method":"provider/submit",
 "params":{
   "tab_id":"ses_promptr_abc123",
   "messages":[
     {"role":"user","content":"write a test"},
     {"role":"assistant","content":"here's a test..."}
   ],
   "query":"add an edge case",
   "model":"deepseek-v4-flash",
   "agent":{
     "name":"linux_cmd",
     "content":"You are a Linux command expert..."
   },
   "is_follow_up":true
 }}
```

| Field | Description |
|-------|-------------|
| `tab_id` | Session identifier. Same `tab_id` across submits = continuation. New `tab_id` = fresh conversation. |
| `messages` | Full conversation history (provider-agnostic format). |
| `query` | The user's latest input. Already appended to `messages` as the last user entry. Duplicated for convenience. |
| `model` | Model identifier string. `"None"` or empty means plugin default. |
| `agent` | If `agent_mode` is `"content"`: `agent.content` has the full agent body. If `"resolution"`: only `agent.name` is set. |
| `is_follow_up` | True if continuing an existing conversation (new submit for same tab_id). False if starting fresh. |

### Response

Promptr expects a stream of `provider/event` notifications, terminated by a `done` or `error` event. When the plugin finishes processing, it sends a `result` response:

```json
{"jsonrpc":"2.0","id":2,"result":{"ok":true}}
```

This `result` is sent **after** the final event (done/error). Plugin must not send events after sending the result.

If the plugin cannot accept the submit (bad parameters, API key not set, etc.), it returns an **error** immediately instead of streaming events:

```json
{"jsonrpc":"2.0","id":2,"error":{
   "code":-32010,
   "message":"OpenRouter API key not configured"
 }}
```

---

## 3. Events (streaming output)

Plugin sends these as notifications during a submit. Multiple events interleaved in any order.

### Types

| Type | Description |
|------|-------------|
| `chunk` | Text fragment. Multiple chunks are appended in order. |
| `tool_call` | Plugin requests a tool execution (only if `tools:"external"`). |
| `done` | Output complete. No more events for this `tab_id`. |
| `error` | Something went wrong. Processing stopped. |

### Chunk

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{"tab_id":"ses_promptr_abc123","type":"chunk","output":"Sure, let me add"}}
```

### Tool Call

Only sent if server capability `tools` is `"external"`. Plugin pauses generation, sends this, waits for `provider/tool_result`, then resumes.

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{
   "tab_id":"ses_promptr_abc123",
   "type":"tool_call",
   "tool_call":{
     "id":"call_abc123",
     "name":"read",
     "args":{"path":"/home/user/project/main.c"}
   }
 }}
```

- `id` — correlation ID. Plugin matches this with the incoming `provider/tool_result`.
- `name` — tool name. Standard: `read`, `glob`, `grep`, `list`, `write`, `edit`, `bash`, `webfetch`. Plugin can also define custom tool names.
- `args` — tool arguments (JSON object). Schema depends on tool name.

### Done

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{
   "tab_id":"ses_promptr_abc123",
   "type":"done",
   "output":"Done. The test now covers edge cases.",
   "elapsed_us":4500000
 }}
```

`elapsed_us` is optional. Plugin should provide it if available.

### Error

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{
   "tab_id":"ses_promptr_abc123",
   "type":"error",
   "error_msg":"API rate limit exceeded"
 }}
```

If `error_msg` is empty (`""`), promptr treats it as a user-initiated cancellation. If non-empty, it's shown as an error dialog.

---

## 4. Tool Result (bidirectional)

Only used when server capability `tools` is `"external"`. Promptr sends this in response to a `tool_call` event.

```json
{"jsonrpc":"2.0","id":3,"method":"provider/tool_result",
 "params":{
   "tab_id":"ses_promptr_abc123",
   "tool_call_id":"call_abc123",
   "result":{
     "content":"int main() { return 0; }",
     "is_error":false
   }
 }}
```

- `tool_call_id` — matches the `id` from the plugin's `tool_call` event.
- `result.content` — the output (file contents, command output, etc.).
- `result.is_error` — true if the tool execution failed (e.g. file not found, permission denied).

Promptr sends this only during an active submit. Plugin resumes processing after receiving it.

---

## 5. Cancel

User cancels a running submit.

```json
{"jsonrpc":"2.0","id":4,"method":"provider/cancel",
 "params":{"tab_id":"ses_promptr_abc123"}}
```

Plugin should stop processing for this `tab_id` and send an `error` event with empty `error_msg`:

```json
{"jsonrpc":"2.0","method":"provider/event",
 "params":{"tab_id":"ses_promptr_abc123","type":"error","error_msg":""}}
```

Then the plugin should respond to the cancel request:

```json
{"jsonrpc":"2.0","id":4,"result":{"ok":true}}
```

If plugin doesn't respond within a timeout, promptr may kill the process.

---

## 6. Cleanup Session

Tab is being destroyed. Plugin should release any per-tab resources.

```json
{"jsonrpc":"2.0","id":5,"method":"provider/cleanup_session",
 "params":{"tab_id":"ses_promptr_abc123"}}
```

Plugin cleans up (removes tmpdir, deletes opencode sessions, etc.) and responds:

```json
{"jsonrpc":"2.0","id":5,"result":{"ok":true}}
```

---

## 7. Shutdown

Promptr is quitting. Plugin should flush state, close connections, and prepare to exit.

```json
{"jsonrpc":"2.0","id":6,"method":"provider/shutdown",
 "params":{}}
```

After sending shutdown, promptr closes stdin. Plugin should exit within a grace period (5s default).

```json
{"jsonrpc":"2.0","id":6,"result":{"ok":true}}
```

---

## 8. Reference Sequence (External Tools)

```
promptr                          openrouter-provider
  │                                    │
  ├─ provider/initialize ──────────────►│
  │◄─ provider/initialized ─────────────┤
  │                                    │
  ├─ provider/submit(tab_id="a") ──────►│
  │◄─ event(chunk, "Let me look at") ───┤
  │◄─ event(tool_call, read("main.c")) ─┤
  ├─ provider/tool_result ─────────────►│
  │◄─ event(chunk, "I see the file") ───┤
  │◄─ event(done, "Done!") ─────────────┤
  │◄─ result(ok) ───────────────────────┤
  │                                    │
  ├─ provider/cleanup_session("a") ───►│
  │◄─ result(ok) ──────────────────────┤
  │                                    │
  ├─ provider/shutdown ────────────────►│
  │  [stdin close]                      │
  │                         [exit]     │
```

## Reference Sequence (Internal Tools)

```
promptr                          opencode-provider
  │                                    │
  ├─ provider/initialize ──────────────►│
  │◄─ provider/initialized ─────────────┤
  │ (tools=false)                       │
  │                                    │
  ├─ provider/submit(tab_id="a") ──────►│
  │   opencode internally:              │
  │   - spawn opencode CLI              │
  │   - CLI handles own tools           │
  │◄─ event(chunk, "Running...") ───────┤
  │◄─ event(chunk, "Result:") ──────────┤
  │◄─ event(done, "...") ───────────────┤
  │◄─ result(ok) ──────────────────────┤
  │                                    │
  ├─ provider/shutdown ────────────────►│
```

---

## Plugin Manifest (plugin.json)

Each plugin directory must contain a `plugin.json`:

```json
{
  "name": "openrouter",
  "version": "1.0.0",
  "command": "promptr-openrouter",
  "description": "OpenRouter API provider",
  "homepage": "https://github.com/promptr/openrouter-provider",
  "type": "provider",
  "methods": [
    "provider/initialize",
    "provider/submit",
    "provider/cancel",
    "provider/cleanup_session",
    "provider/shutdown"
  ],
  "config_schema": {
    "api_key": {
      "type": "string",
      "required": true,
      "secret": true,
      "comment": "API key for OpenRouter"
    },
    "base_url": {
      "type": "string",
      "default": "https://openrouter.ai/api/v1",
      "comment": "Custom base URL"
    }
  }
}
```

- `name` — unique identifier. Matches directory name.
- `command` — executable filename (on PATH or in plugin directory).
- `type` — `"provider"` only for now. Future: `"tool"`, `"channel"`.
- `methods` — which protocol methods the plugin implements. Promptr uses this to validate.
- `config_schema` — defines plugin-specific config keys. Merged into promptr's config file. `secret: true` entries are never logged.

### Discovery

Plugins are discovered at these paths (scanned at startup in order):

1. `~/.config/promptr/plugins/<name>/plugin.json` — user-installed
2. `/usr/lib/promptr/plugins/<name>/plugin.json` — system/bundled

User-installed plugins with the same name as a bundled plugin override it.

---

## Error Codes

| Code | Meaning |
|------|---------|
| `-32600` | Invalid Request (malformed JSON, missing fields) |
| `-32601` | Method not found |
| `-32603` | Internal error |
| `-32000` | Provider error (generic) |
| `-32001` | Protocol version mismatch |
| `-32010` | Submit rejected (bad params, missing API key, etc.) |
| `-32020` | Plugin busy (submit already in progress and cannot queue) |

---

## Security Considerations

- Plugin processes have the same user permissions as promptr. Trust-by-install.
- For `tools:"external"` mode, promptr validates all tool arguments against agent path allow/deny rules before execution. Plugin never touches the filesystem directly.
- For `tools:"internal"` mode, the plugin is entirely responsible for its own security. Promptr cannot enforce path rules inside an opaque CLI.
- `config_schema` entries marked `"secret":true` are redacted from logs and never written to promptr's own config file in plaintext if a secret storage backend is available.
- Plugins are free to exit at any time. Promptr detects the process exit and shows an error.
