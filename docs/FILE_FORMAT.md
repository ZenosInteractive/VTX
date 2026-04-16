# VTX Binary File Format

## Overview

VTX files store frame-based replay data in a chunked binary format. Each file consists of a **header**, one or more **data chunks**, and a **footer**. The format supports two serialization backends: Protocol Buffers (`VTXP`) and FlatBuffers (`VTXF`).

## File Layout

```
+------------------+
| Magic (4 bytes)  |   "VTXF" or "VTXP"
+------------------+
| Header Size (4B) |   uint32_t — compressed header size
+------------------+
| Header Blob      |   zstd-compressed serialized header
+------------------+
| Chunk 0          |   Frame data chunk
| Chunk 1          |   ...
| ...              |
| Chunk N          |
+------------------+
| Footer Blob      |   zstd-compressed serialized footer
+------------------+
| Footer Size (4B) |   uint32_t — compressed footer size
+------------------+
| Sentinel  (4B)   |   Padding / alignment
+------------------+
```

## Magic Bytes

The first 4 bytes identify the serialization format:

| Magic | Format | Description |
|---|---|---|
| `VTXF` | FlatBuffers | Binary-efficient, zero-copy reads |
| `VTXP` | Protobuf | Widely supported, schema evolution |

`OpenReplayFile()` reads these bytes to auto-detect the format and select the correct deserializer.

## Header

The header is stored immediately after the magic bytes.

**On disk:**
1. `uint32_t` — compressed size of the header blob
2. Blob — zstd-compressed serialized header (Protobuf or FlatBuffers encoding)

**Decoded fields (`FileHeader`):**

| Field | Type | Description |
|---|---|---|
| `replay_name` | `string` | Human-readable replay name |
| `replay_uuid` | `string` | Unique identifier (UUID v4) |
| `recorded_utc_timestamp` | `int64_t` | Unix timestamp (seconds) of recording start |
| `version.format_major` | `uint32_t` | File format major version |
| `version.format_minor` | `uint32_t` | File format minor version |
| `version.schema_version` | `uint32_t` | Schema version for the property layout |
| `custom_json_metadata` | `string` | Arbitrary JSON blob for user metadata |
| Property schema | Embedded | Maps property names to typed indices (int, float, vector, etc.) |

The **property schema** embedded in the header defines the layout of `PropertyContainer` fields. It maps human-readable names (e.g., "Health", "Position") to specific typed arrays and indices within each entity.

## Data Chunks

Each chunk contains a batch of serialized frames. Chunks are the unit of I/O and caching.

**On disk:**
1. `uint32_t` — chunk marker / padding (4 bytes, skipped on read)
2. Compressed blob — zstd-compressed serialized chunk data

**Chunk contents (after decompression):**
- Array of serialized frames, each containing:
  - One or more **buckets** (named entity groups)
  - Each bucket holds **unique IDs** and **PropertyContainer** arrays
  - Properties use the indices defined by the header schema

### Chunk Index Entry

The footer's seek table stores metadata for each chunk:

| Field | Type | Description |
|---|---|---|
| `chunk_index` | `int32_t` | Sequential chunk number |
| `start_frame` | `int32_t` | First frame index in this chunk |
| `end_frame` | `int32_t` | Last frame index in this chunk |
| `file_offset` | `uint64_t` | Byte offset from file start |
| `chunk_size_bytes` | `uint32_t` | Compressed size on disk |

## Footer

The footer is stored at the end of the file, before the footer-size sentinel.

**Reading the footer:**
1. Seek to end - 8 bytes
2. Read `uint32_t` footer_size
3. Seek to end - (footer_size + 8)
4. Read and decompress the footer blob

**Decoded fields (`FileFooter`):**

| Field | Type | Description |
|---|---|---|
| `total_frames` | `int32_t` | Total number of frames in the file |
| `duration_seconds` | `float` | Total replay duration |
| `payload_checksum` | `uint64_t` | xxHash64 checksum of all frame data |
| `chunk_index` | `vector<ChunkIndexEntry>` | Seek table for random access |
| `events` | `vector<TimelineEvent>` | Timestamped events (kills, spawns, etc.) |
| `times` | `ReplayTimeData` | Game timestamps, UTC timestamps, gaps, segments |

### Timeline Events

| Field | Type | Description |
|---|---|---|
| `game_time` | `float` | Game-time in seconds |
| `event_type` | `string` | Event category (e.g., "Kill", "Spawn") |
| `label` | `string` | Human-readable description |
| `location` | `Vector` | World position (x, y, z) |
| `entity_unique_id` | `string` | Related entity ID |

### Replay Time Data

| Field | Type | Description |
|---|---|---|
| `game_time` | `vector<float>` | Per-frame game timestamps |
| `created_utc` | `vector<int64_t>` | Per-frame UTC timestamps |
| `gaps` | `vector<int32_t>` | Frame indices where recording gaps occurred |
| `segments` | `vector<int32_t>` | Frame indices marking segment boundaries |

## Frame, Bucket & PropertyContainer Data Model

Every frame in a VTX file follows a hierarchical data model: **Frame > Buckets > Entities (PropertyContainer)**. Understanding this hierarchy is essential for reading, writing, and diffing replay data.

### Frame

A `Frame` represents a single simulation tick. It contains one or more named **Buckets**, accessed by name via an internal map.

```
Frame
 ├─ bucket_map     { "Players" -> 0, "Projectiles" -> 1 }
 └─ buckets[]
      ├─ Bucket 0  ("Players")
      └─ Bucket 1  ("Projectiles")
```

Bucket names are user-defined and typically represent logical categories in the game world (e.g., `"Players"`, `"Vehicles"`, `"World"`, `"Projectiles"`). A frame can have any number of buckets.

### Bucket

A `Bucket` groups entities of related types. It stores two parallel arrays:

| Field | Type | Description |
|---|---|---|
| `unique_ids` | `vector<string>` | Unique identifier per entity (e.g., `"player_042"`, `"proj_789"`) |
| `entities` | `vector<PropertyContainer>` | Property data for each entity (same index as `unique_ids`) |
| `type_ranges` | `vector<EntityRange>` | Optional index ranges grouping entities by type for fast lookup |

The arrays are always parallel: `unique_ids[i]` identifies the entity whose data is in `entities[i]`.

```
Bucket "Players"
 ├─ unique_ids:  [ "player_001",            "player_002",            "player_003"           ]
 ├─ entities:    [ PropertyContainer {...},  PropertyContainer {...}, PropertyContainer {...} ]
 └─ type_ranges: [ { start: 0, count: 3 } ]  // all 3 are the same type
```

`type_ranges` allows O(1) access to all entities of a specific type within the bucket via `GetEntitiesOfType(typeId)`, avoiding a full scan.

### PropertyContainer

The `PropertyContainer` is the core data unit. It holds **all properties of a single entity** organized by type. Instead of using a map or variant, it uses **explicit typed vectors** — one per supported value type. The property schema in the file header maps human-readable names to (type, index) pairs.

#### Metadata

| Field | Type | Description |
|---|---|---|
| `entity_type_id` | `int32_t` | Type identifier (maps to schema-generated enum). `-1` = unknown |
| `content_hash` | `uint64_t` | xxHash64 digest of all property data. Used for fast equality checks in diffing |

#### Scalar Properties

Single values per property slot. The property schema maps names to indices.

| Vector | Value type | Example usage |
|---|---|---|
| `bool_properties` | `bool` | IsAlive, IsVisible |
| `int32_properties` | `int32_t` | Score, Ammo, TeamId |
| `int64_properties` | `int64_t` | SteamId, SessionId |
| `float_properties` | `float` | Health, Speed, Cooldown |
| `double_properties` | `double` | Precision timers |
| `string_properties` | `string` | PlayerName, WeaponType |

#### Spatial Properties

Geometric types for 3D state.

| Vector | Value type | Components |
|---|---|---|
| `vector_properties` | `Vector` | `{ x, y, z }` (double precision) |
| `quat_properties` | `Quat` | `{ x, y, z, w }` (float precision) |
| `transform_properties` | `Transform` | `{ translation: Vector, rotation: Quat, scale: Vector }` |
| `range_properties` | `FloatRange` | `{ min, max, value_normalized }` (e.g., health bar 50/100) |

#### Array Properties (FlatArray\<T\> — Structure of Arrays)

When an entity needs **arrays of values** (e.g., an inventory, a list of buffs, waypoints), VTX uses `FlatArray<T>`. This is a Structure of Arrays (SoA) container that stores all elements contiguously in a single `data` vector with `offsets` delimiting sub-arrays — avoiding the overhead of `vector<vector<T>>`.

```
FlatArray<int32_t>
  data:    [ 10, 20, 30,  100, 200 ]
  offsets: [  0,           3        ]
             ^sub-array 0  ^sub-array 1
```

| FlatArray field | Element type | Example usage |
|---|---|---|
| `byte_array_properties` | `uint8_t` | Raw binary blobs, thumbnails |
| `int32_arrays` | `int32_t` | Inventory item IDs |
| `int64_arrays` | `int64_t` | Achievement timestamps |
| `float_arrays` | `float` | Damage history, stat buffers |
| `double_arrays` | `double` | Precision measurement arrays |
| `string_arrays` | `string` | Status effect names, chat log |
| `bool_arrays` | `uint8_t` | Unlock flags, visibility masks |
| `vector_arrays` | `Vector` | Waypoints, trail positions |
| `quat_arrays` | `Quat` | Rotation keyframes |
| `transform_arrays` | `Transform` | Bone transforms, path nodes |
| `range_arrays` | `FloatRange` | Multi-bar HUD elements |

#### Nested & Recursive Structures

For complex hierarchies that don't fit flat typed arrays.

| Field | Type | Description |
|---|---|---|
| `any_struct_properties` | `vector<PropertyContainer>` | Nested structs (recursive). E.g., equipment slots each with their own properties |
| `any_struct_arrays` | `FlatArray<PropertyContainer>` | Arrays of nested structs in SoA layout |
| `map_properties` | `vector<MapContainer>` | Key-value maps (`MapContainer` = parallel `keys[]` + `values[]`) |
| `map_arrays` | `FlatArray<MapContainer>` | Arrays of maps in SoA layout |

A `MapContainer` holds parallel string keys and `PropertyContainer` values:

```
MapContainer
  keys:   [ "helmet", "chest", "boots" ]
  values: [ PropertyContainer {...}, PropertyContainer {...}, PropertyContainer {...} ]
```

### Schema Mapping

Properties are stored by **index**, not by name. The property schema in the file header maps each property name to a specific typed vector and slot index:

```
Schema mapping example:
  "Health"     -> float_properties[0]
  "Position"   -> vector_properties[0]
  "PlayerName" -> string_properties[0]
  "TeamId"     -> int32_properties[0]
  "Inventory"  -> int32_arrays, sub-array 0
  "IsAlive"    -> bool_properties[0]
```

The `PropertyAddressCache` provides O(1) runtime lookup from property name to (type, index), built once from the header schema on file open.

### Complete Example

A single frame from a hypothetical game:

```
Frame (tick 1042)
 └─ Bucket "Players"
      ├─ unique_ids:  [ "player_001",  "player_002" ]
      └─ entities:
           ├─ [0] PropertyContainer (entity_type_id = 0, content_hash = 0xA3F...)
           │    ├─ float_properties:     [ 85.0, 12.5 ]       // Health=85, Speed=12.5
           │    ├─ int32_properties:     [ 1, 42 ]             // TeamId=1, Score=42
           │    ├─ string_properties:    [ "Alice" ]           // PlayerName
           │    ├─ vector_properties:    [ {100, 0, 200} ]     // Position
           │    ├─ bool_properties:      [ true ]              // IsAlive
           │    └─ int32_arrays:                               // Inventory
           │         data:    [ 5, 12, 3 ]
           │         offsets: [ 0 ]                            // 1 sub-array: items [5,12,3]
           │
           └─ [1] PropertyContainer (entity_type_id = 0, content_hash = 0xB7E...)
                ├─ float_properties:     [ 100.0, 10.0 ]      // Health=100, Speed=10
                ├─ int32_properties:     [ 2, 18 ]             // TeamId=2, Score=18
                ├─ string_properties:    [ "Bob" ]             // PlayerName
                ├─ vector_properties:    [ {-50, 0, 300} ]     // Position
                ├─ bool_properties:      [ true ]              // IsAlive
                └─ int32_arrays:                               // Inventory
                     data:    [ 7, 1 ]
                     offsets: [ 0 ]                            // 1 sub-array: items [7,1]
```

## Compression

All blobs (header, chunks, footer) are compressed with **zstd** (Zstandard). The SDK's `ReplayUnpacker::Decompress()` handles decompression transparently.

## Schema Definitions

The underlying serialization schemas live in `sdk/src/schemas/`:

- `vtx_schema.proto` — Protobuf schema
- `vtx_schema.fbs` — FlatBuffers schema

Both define the same logical structure. Code generation produces:
- `vtx_schema.pb.h` / `vtx_schema.pb.cc` (Protobuf)
- `vtx_schema_generated.h` (FlatBuffers)

## Random Access

The reader uses the footer's **chunk index** (seek table) as a B-tree-like index:

1. Binary search the seek table for the chunk containing the target frame
2. If the chunk is cached, return the frame directly
3. If not cached, trigger an async load and optionally block
4. The cache window evicts chunks outside the configured range

This enables O(log N) seek to any frame in the file, where N is the number of chunks.
