# Promptr provider protocol 2.0

A provider is an executable plugin, written in any language. Promptr owns the
conversation; a provider translates messages and Markdown context into backend
requests and streams answer text back.

Providers own authentication, backend sessions, HTTP/CLI integration, tools and
agent loops. Promptr never executes tool calls received from a provider.

## Transport and compatibility

- JSON-RPC 2.0 over stdin/stdout, one UTF-8 JSON object per newline. Strings must
  escape embedded newlines. Stdout contains protocol messages only.
- Integer RPC IDs are nonnegative and at most 2147483647. Thread/request IDs
  are nonempty strings of at most 256 bytes. Responses match the
  request ID. Notifications have no RPC ID.
- Maximum serialized frame: 64 MiB. Maximum text delta: 64 KiB of UTF-8. Split
  large output at character boundaries. Promptr rejects oversized input rather
  than silently truncating conversation history.
- Unknown optional object fields may be ignored. New optional methods are
  advertised through capabilities. Unknown methods return `-32601`.
- The manifest format and provider protocol have separate versions. The current
  host selects `2.0`; plugins must explicitly accept it or return `-32001`.
  Protocol 1.x is incompatible. Breaking changes require a new major version;
  additive optional fields do not change existing semantics.
- Stderr is diagnostic-only. The current host discards it to avoid capturing
  secrets. Return actionable, sanitized errors through JSON-RPC.
- One pending submit per process. The host starts processes lazily, isolates
  simultaneous threads in separate processes, and closes stdin when finished.
  Do not depend on a permanently resident plugin process.
- Closing stdin means release transport resources and exit. It must not delete
  a user's project, saved conversation or resumable backend session.

The host currently allows 10 seconds for initialization and writing a request,
60 seconds for a connection test or model discovery, and 5 seconds each for
cancellation and process shutdown. Normal generation has no fixed duration
limit. Startup, I/O and cleanup run outside the GTK main loop. The UI delivery
queue is bounded to 1 MiB per request; slow consumers apply backpressure.

## Initialize

```json
{"jsonrpc":"2.0","id":1,"method":"provider/initialize","params":{"protocol_version":"2.0","purpose":"submit","config":{"api_key":"resolved-secret","base_url":"https://example.com/api"}}}
{"jsonrpc":"2.0","id":1,"result":{"protocol_version":"2.0","capabilities":{"streaming":true,"models":true}}}
```

`config` contains the selected profile's settings, including resolved secret
values. Never log or echo credentials. Validate required values and backend
requirements here; return `-32010` with an actionable message when invalid.
The optional `purpose` is `submit` (default) or `models`. Providers can allow
public model discovery without requiring submission credentials. They must still
validate credentials for submission before replying successfully to initialize,
so missing credentials cannot reset a saved conversation.
Initialization must not start a conversation. Successful initialization alone
is insufficient to add a profile: Promptr also tests a real submission.

## Submit

```json
{"jsonrpc":"2.0","id":2,"method":"provider/submit","params":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid","messages":[{"role":"user","content":"Explain this function"},{"role":"assistant","content":"It parses a record."},{"role":"user","content":"What happens with empty input?"}],"agent":{"name":"reviewer","content":"Review carefully. Explain edge cases."},"model":"example/model","project_dir":"/home/user/project","resume_state":"opaque-provider-state","connection_test":false}}
```

| Field | Contract |
| --- | --- |
| `thread_id` | Stable conversation identifier. Never a widget address or PID. |
| `request_id` | Unique submission identifier. Include it on every event and successful terminal response. |
| `messages` | Canonical ordered history, ending in the current user message exactly once. Roles are `user`, `assistant`, `system`; content is a string. Messages need not alternate. Partial replies can occur in saved history. |
| `agent` | Optional selected document name and complete Markdown text. Use the content; do not resolve the name through backend-specific agent directories. |
| `model` | Optional model ID. Omission uses the profile/backend default. |
| `project_dir` | Optional absolute directory. Apply it only where the backend supports project context. It is never provider-owned scratch space. |
| `resume_state` | Optional opaque string previously returned by this profile for this thread. |
| `connection_test` | When true, perform a tiny real backend request. Avoid tool execution and return normal answer deltas and completion. |

The host snapshots settings and context per request. Changes made while a reply
is pending apply to a subsequent submission. With Follow up disabled, the host
starts fresh and omits prior history and resume state.

Every request supplies portable history even when resume state is present. The
provider decides whether that state still matches the history, context, model,
project and backend configuration. State may be stale after another provider
answered, settings changed or a backend session expired. Rebuild from messages
when necessary. Never silently omit messages to fit a backend context window;
report a clear error if the backend cannot accept them. A provider may use its
own backend-supported context management, but must preserve the host transcript.

### Answer events

```json
{"jsonrpc":"2.0","method":"provider/event","params":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid","type":"delta","output":"For empty input, "}}
{"jsonrpc":"2.0","method":"provider/event","params":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid","type":"delta","output":"the function returns no record."}}
```

Deltas contain new text only. The host appends and saves them in order. Do not
repeat the full answer in a terminal result. Optional `type:"progress"` events
may contain informational text; the current host ignores them. Tools remain
inside the provider/backend; `tool_call` and `tool_result` are unsupported.

Events for a different thread or request are ignored. Malformed events for the
current request fail it. Do not send further events after its terminal response.

### Exactly one terminal response

```json
{"jsonrpc":"2.0","id":2,"result":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid","status":"completed","resume_state":"updated-opaque-state"}}
```

`status` is `completed` or `cancelled`. `resume_state` is optional and must be
independently versioned by the plugin if its representation changes. The host
stores it without interpretation, scoped to the profile and conversation.
A successful response without state clears that profile's previous state.

For rejection or failure, use the submit's RPC ID:

```json
{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Backend rejected the configured model; choose another model."}}
```

A completed answer must contain nonempty text. Do not send separate `done`/`error` notifications. Partial output is retained
on failure, cancellation, unexpected EOF or application interruption.

## Cancel

```json
{"jsonrpc":"2.0","id":3,"method":"provider/cancel","params":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid"}}
{"jsonrpc":"2.0","id":3,"result":{"ok":true}}
{"jsonrpc":"2.0","id":2,"result":{"thread_id":"stable-thread-uuid","request_id":"unique-request-uuid","status":"cancelled"}}
```

Cancellation acknowledgement is separate from submit completion. Stop backend
work, finish cleanup, and complete the pending submit. Completion may win a
race with cancellation. A late cancellation must not affect another request.
If a plugin fails to stop, the host terminates its isolated process group.

## Optional model discovery

Advertise `capabilities.models:true` only when implemented.

```json
{"jsonrpc":"2.0","id":2,"method":"provider/models","params":{}}
{"jsonrpc":"2.0","id":2,"result":{"models":["example/fast","example/large"]}}
```

The host discovers models for the selected profile and caches the result.
Users can refresh the list in Preferences or enter a model ID.
A model list may contain at most 10000 IDs, with at most 1024 bytes per ID. Tab restoration never starts discovery.

## Installing a plugin

Place an executable and `plugin.json` in a plugin directory:

1. `PROMPTR_PLUGIN_DIR/<plugin>/` — explicit development override.
2. `$XDG_CONFIG_HOME/promptr/plugins/<plugin>/` — user installation.
3. `<installation-prefix>/lib/promptr/plugins/<plugin>/` — bundled installation.
4. The compiled default directory and `/usr/lib/promptr/plugins/` — fallbacks.

First matching name wins. Debug builds use `promptr-debug` for user settings,
profiles and conversations; bundled system providers remain under `promptr`.
The executable-relative lookup supports custom prefixes and relocated archives.
The GUI does not copy or install arbitrary executable dependencies.

```json
{
  "manifest_version": 1,
  "protocol_versions": ["2.0"],
  "name": "example",
  "version": "1.0.0",
  "type": "provider",
  "command": "example-provider",
  "description": "Example backend",
  "config_schema": {
    "api_key": {"type":"string","secret":true,"comment":"API key"},
    "base_url": {"type":"string","default":"https://example.com/api","comment":"Endpoint"},
    "verify_tls": {"type":"boolean","default":true,"comment":"Verify TLS"},
    "temperature": {"type":"number","default":0.2,"comment":"Temperature"},
    "mode": {"type":"choice","choices":["fast","careful"],"default":"fast","comment":"Mode"}
  }
}
```

The executable is resolved relative to the manifest, or through an absolute
path/PATH. A profile can override the executable and supply quoted arguments;
arguments are parsed into an argv array, never executed by a shell.

Supported settings types are strings, booleans, numbers and choices. Secrets
are string fields marked `secret:true`. `label` supplies a short visible label (`comment` is the fallback);
`advanced:true` places a field in the Advanced section. `setup_hint` on a
manifest supplies provider-owned login guidance. `default` supplies an initial value. Backend-specific validation belongs to the
plugin and is exercised by Test connection. Arbitrary schema-driven code or UI
widgets are unsupported.

Manifests may include a `models` array of initial model IDs and a
`bundled_profile` object with a stable UUID `id`, display `name`, and optional
default `model`. These provide ready-to-select profiles without writing user
configuration on startup. The bundled OpenRouter profile is the configured
default. Bundled profiles can be used immediately; plugins validate their
credentials at submission. The host queries `provider/models` asynchronously
only for the selected provider and caches the result for the application session.
Manifest models remain available if discovery fails. Selecting a different
profile updates the existing picker and cancels obsolete discovery, without
cancelling conversation replies.

Users edit or create named profiles in the modal **Edit → Preferences**. Only a
successful tiny submission enables saving an edited or new profile. Changing settings invalidates the
test. Connection tests accept any nonempty successful answer; they do not
compare it to `pong`. Profiles contain settings and secret field references;
secret values entered in the form are saved in the system Secret Service.
There is no plaintext fallback when the keyring is unavailable. Stored secrets
use versioned references so an unsuccessful profile save leaves the previous
configuration and pending requests usable. Deleting a profile with a pending
reply is blocked until that reply finishes or is cancelled. Backend-owned
login or environment credentials remain available to plugins that support them.

## Errors and reference implementations

| Code | Meaning |
| --- | --- |
| `-32600` | Invalid request/envelope |
| `-32601` | Unsupported method |
| `-32602` | Invalid parameters |
| `-32000` | Backend failure |
| `-32001` | Incompatible protocol |
| `-32010` | Invalid configuration / rejected initialization |
| `-32020` | A request is already pending |

Plugins run with the user's permissions and are trusted when installed.
Promptr supplies no tool sandbox and does not enforce backend filesystem or
permission policy. A connection test is a real, potentially billable request;
it cannot guarantee that every model, tool or future service request will work.

`provider-server.h` supplies an optional C server helper. The bundled OpenRouter
and OpenCode adapters demonstrate HTTP and CLI backends. `tests/fixture_provider.py`
is a small independent Python peer used to test the public contract; it is not
an adapter for a real service.
