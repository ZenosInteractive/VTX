# vtx_cli (VTX Command Line Inspector)

`vtx_cli` is a headless VTX replay inspector. It accepts line-based commands and returns JSON responses.

If you need a GUI, use `vtx_inspector`.

## Quick Start

### Interactive (stdin/stdout)

```powershell
vtx_cli.exe
```

### TCP server mode

```powershell
vtx_cli.exe --port 5555
```

In TCP mode, the process waits for one client connection.

## Startup and Response Format

### Startup message

On startup, `vtx_cli` emits a JSON ready message:

```json
{
  "status": "ready",
  "tool": "vtx_cli",
  "version": "0.1.0",
  "commands": ["open", "close", "help", "..."]
}
```

### Success response

```json
{
  "status": "ok",
  "command": "<command>",
  "data": { ... }
}
```

### Error response

```json
{
  "status": "error",
  "command": "<command>",
  "error": "<message>",
  "hint": "<optional hint or null>"
}
```

## CLI Options

| Option | Alias | Description |
| --- | --- | --- |
| `--help` | `-h` | Print usage and command list, then exit |
| `--version` |  | Print CLI version, then exit |
| `--verbose` | `-v` | Print human-readable startup banner/logs |
| `--port <port>` | `-p <port>` | Use TCP transport instead of stdin/stdout |

Notes:

- Unknown CLI arguments return an error and exit.

## Input Rules

- One command per line.
- Tokens are space-separated.
- Double quotes group tokens, for example: `open "C:\\path with spaces\\file.vtx"`.
- For `open`, everything after `open` is treated as the file path.
- `open` also supports `--json-only`: `open <path> [--json-only]`.

## Command Reference

All commands are case-sensitive.

### Session Commands

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `help` | `help` | No | List all interactive commands |
| `exit` | `exit` | No | End the session |
| `quit` | `quit` | No | Alias for `exit` |

### File Commands

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `open` | `open <path> [--json-only]` | No | Open a `.vtx` file |
| `close` | `close` | Yes | Close the current file |

`open` returns: `file`, `format`, `total_frames`, `duration_seconds`, `file_size_mb`.

### Info Commands

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `info` | `info` | Yes | Replay summary |
| `header` | `header` | Yes | Header data |
| `footer` | `footer` | Yes | Footer data |
| `schema` | `schema` | Yes | Contextual schema |
| `chunks` | `chunks` | Yes | Chunk seek table |
| `events` | `events` | Yes | Timeline events |

### Navigation

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `frame` | `frame [n]` | Yes | Show current frame or move to frame `n` |

`frame` always includes: `current_frame`, `total_frames`, `bucket_count`, `entity_count`.

### Inspection Commands

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `buckets` | `buckets` | Yes | List buckets in current frame |
| `entities` | `entities <bucket>` | Yes | List entities in a bucket |
| `entity` | `entity <bucket> <id_or_index>` | Yes | Dump full entity payload |
| `entity` | `entity <bucket> --index <n>` | Yes | Select entity by index |
| `entity` | `entity <bucket> --id <uid>` | Yes | Select entity by unique ID |
| `property` | `property <bucket> <entity> <name>` | Yes | Read one property value |
| `property` | `property <bucket> --index <n> <name>` | Yes | Read property by entity index |
| `property` | `property <bucket> --id <uid> <name>` | Yes | Read property by entity unique ID |
| `types` | `types [name]` | Yes | List all types or inspect one type |

Bucket argument:

- `<bucket>` can be bucket name or numeric index.
- Bucket name matching is case-insensitive.
- If runtime bucket names are missing, the CLI falls back to contextual schema bucket names (order-based).

Entity indexing:

- Entity indices can change between frames as entities are added or removed. Use `--id <uid>` for stable cross-frame references. Reserve `--index` for one-off inspection of a single frame.

Important response fields:

- `buckets`: each item includes `index`, `name` (can be `null`), `entity_count`.
- `entities`: includes `bucket_index`, `bucket_name`, and `entities`.
- `entity` and `property`: include `bucket_index`, `bucket_name`, `entity_index`, `unique_id`.

### Analysis Commands (WIP)

| Command | Usage | Needs open file | Description |
| --- | --- | --- | --- |
| `diff` | `diff <frame_a> <frame_b> [--epsilon <val>]` | Yes | Compare two frames |
| `track` | `track <bucket> <entity> <prop> <start> <end> [step]` | Yes | Sample property across frames |
| `track` | `track <bucket> --index <n> <prop> <start> <end> [step]` | Yes | Track by entity index |
| `track` | `track <bucket> --id <uid> <prop> <start> <end> [step]` | Yes | Track by entity unique ID |
| `search` | `search <property> <op> <value> [bucket]` | Yes | Filter entities in current frame |

Search operators:

- `==`, `!=`, `>`, `<`, `>=`, `<=`

Behavior notes:

- `search` runs on the current frame only and returns `match_count`.
- `search` supports scalar fields (bool/int32/int64/float/double/string).
- `track` validates frame range and selected property before sampling.
- `diff` validates frame indices and can use custom float epsilon via `--epsilon`.
- `diff` returns `changed`, `added`, `removed`; when no differences exist, it also returns a `reason`.

## Parsing Notes

- `schema.property_mapping` and `header.custom_metadata` can contain raw JSON.
- A single response can span multiple lines.
- With `--verbose`, human-readable logs are printed.
- With `open ... --json-only`, open-time non-JSON diagnostics are redirected to stderr.

Use a streaming JSON parser, or buffer output until a full JSON object is parsed.

### JSON Framing and Buffering (Important)

When integrating with `vtx_cli`, treat output as a JSON stream, not newline-delimited JSON.

- Do not assume one line equals one response.
- A single response may be pretty-printed or contain embedded newlines (notably `schema`).
- Framing is transport-agnostic: stdin/stdout mode and TCP mode follow the same JSON-object stream behavior.

Recommended client behavior:

- For machine consumers, keep `--verbose` disabled.
- Use `open <path> --json-only` so open-time diagnostics do not pollute stdout JSON.
- Accumulate bytes/chars in a buffer and emit only complete top-level JSON objects.
- Track JSON state while scanning:
  - object/array depth (`{`,`[`,`}`,`]`)
  - in-string state (`"..."`)
  - escape state (`\\`)
- Only treat a response as complete when depth returns to 0 outside a string.

Failure handling:

- If EOF/connection-close occurs with non-empty partial JSON, treat it as truncated response.
- If a framed object fails JSON parsing, log raw payload for diagnostics and continue or fail fast per your app policy.
- Ignore leading/trailing whitespace between JSON objects.

Minimal framing example (Python):

```python
import json
import sys


def emit_json_object(json_text: str) -> None:
    msg = json.loads(json_text)
    print("command:", msg.get("command"), "status:", msg.get("status"))


def iter_framed_json(stream):
    buf = ""
    depth = 0
    in_string = False
    escaped = False
    start = -1

    while True:
        chunk = stream.read(4096)
        if not chunk:
            break
        buf += chunk

        i = 0
        while i < len(buf):
            ch = buf[i]

            if start == -1:
                if ch.isspace():
                    i += 1
                    continue
                if ch in "{[":
                    start = i
                    depth = 1
                    in_string = False
                    escaped = False
                    i += 1
                    continue
                # Skip non-JSON leading characters (for example accidental logs).
                i += 1
                continue

            if in_string:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == '"':
                    in_string = False
                i += 1
                continue

            if ch == '"':
                in_string = True
            elif ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
                if depth == 0:
                    obj = buf[start : i + 1]
                    yield obj
                    buf = buf[i + 1 :]
                    i = 0
                    start = -1
                    continue
            i += 1

    # Optional strict mode: reject trailing partial payload.
    if start != -1 and buf[start:].strip():
        raise RuntimeError("truncated JSON response at EOF")


if __name__ == "__main__":
    for json_obj in iter_framed_json(sys.stdin):
        emit_json_object(json_obj)
```

## Example Session

```text
> open C:\\replays\\example.vtx
< {"status":"ok","command":"open","data":{...}}

> frame
< {"status":"ok","command":"frame","data":{"current_frame":0,"total_frames":...,"bucket_count":...,"entity_count":...}}

> entity 0 --index 0
< {"status":"ok","command":"entity","data":{"bucket_index":0,"entity_index":0,...}}

> property 0 --id Ball_FX_Ball UniqueId
< {"status":"ok","command":"property","data":{"property":"UniqueId","value":"..."}}

> diff 100 110 --epsilon 0.0001
< {"status":"ok","command":"diff","data":{"total_ops":...}}

> close
< {"status":"ok","command":"close","data":{}}
```
