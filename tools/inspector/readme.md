# vtx_inspector Usage

This document covers how to use the Inspector GUI.

## 1. Start the App

Open `VTX Inspector` from your installed applications.

## 2. Open a Replay

1. In the app menu, go to `File` -> `Open VTX Replay...`
2. Select a `.vtx` file.
3. Optional: use `File` -> `Open Recent` for previously opened files.
4. Use `File` -> `Close Replay` to unload the current file.

The `File` menu also offers two file operations:

- `File` -> `Repair Replay...` — recover a crashed recording from its `.vtx.recovery`
  sidecar (see "Repair Replay" below). Available even with no replay loaded.
- `File` -> `Cut Replay...` — export a sub-range of the loaded replay as a new `.vtx`
  (see "Cut Replay" below).

## 3. Main Workflow

1. Use `Timeline` to move through frames.
2. Use `Buckets` to find and select an entity.
3. Inspect selected entity fields in `Entity Details`.
4. Cross-reference structure and property mapping in `Contextual Schema`.

## 4. Window Usage

### Timeline

- Drag the slider to scrub — the axis is **wall-clock time** (equal distance = equal
  recorded time), resolved to frames through the footer time table. Inside a gap the
  grab snaps to the gap's start.
- Recording gaps are painted **red**: time-proportional bands on the slider, flagged
  frames on the strip (hover for gap duration and estimated missing frames).
- Adjust the `Drop FPS` expected rate to tune gap detection (the entered value is
  derated by 0.75 — captures rarely sustain their nominal rate).
- Use `Go to` and press Enter to jump to a frame.
- Click bars in the lower frame strip to seek.
- Hold `Ctrl` + mouse wheel to zoom the frame strip.
- Files without a footer time table fall back to frame-index scrubbing and no gap
  detection.

### Repair Replay (`File` menu)

- Pick the crashed `.vtx`; the `.vtx.recovery` sidecar auto-fills if it sits next to
  it (or browse to it — a sidecar from elsewhere is staged safely beside the file).
- Click `Repair`. The repair runs in the background; the window reports progress and
  then success (recovered chunks/frames) or the failure detail.
- A file that was already complete reports "nothing to repair" and is left untouched.

### Cut Replay (`File` menu)

- Choose the range as elapsed **Time**, **Frames**, or absolute **UTC** (ISO-8601 or
  unix seconds/ms/ticks) — or `Chunk` mode to snap to whole chunks.
- The preview shows the resolved chunks, frames, duration, and estimated size live.
- `Save As...` writes the cut as a new `.vtx` on a background thread: exact frame
  bounds (edge chunks are rewritten), frame numbering rebased to 0, time table sliced
  with absolute UTC stamps preserved. Timeline events are not carried over.

### Buckets

- Use `Filter by UniqueID or Type...` to narrow entities.
- Toggle `Show Schema Names` to display schema type names.
- Click an entity to focus it in `Entity Details`.

### Entity Details

- Left-click a property to highlight it in `Contextual Schema`.
- Right-click a property to copy its raw value.
- Right-click struct/group nodes to jump to schema mapping.
- Toggle `Show Schema Names` to switch labels.

### Contextual Schema

- Browse structs and mapped properties for the loaded replay.
- Click `Export JSON` to save `property_mapping` as a `.json` file.
- Click struct-type buttons in the `Type` column to jump to related structs.

### Dynamic Chunk Loading

- Adjust `Backward Chunks` and `Forward Chunks`.
- Monitor loaded/active/evicted chunk state in the live table.

### Time Data

- Switch between `GameTime`, `UTC`, `Gaps`, and `Segments` tabs.
- Toggle `Formatted` for human-readable time vs raw ticks.
- Click any row to seek timeline frame.

### File Properties / Chunk Index / Timeline Events / Logs

- `File Properties`: header/footer summary and metadata.
- `Chunk Index`: chunk boundaries, frame ranges, and offsets.
- `Timeline Events`: event stream table from footer.
- `Logs`: replay load, warnings, and errors.

## 5. Layout Controls

- `Windows` menu shows all docked panels.
- Use `Windows` -> `Reset Layout to Default` if docking layout gets messy.

## 6. Notes

- `vtx_inspector` is GUI-only; it does not use command-line replay arguments.
- Supported open flow is through `File` -> `Open VTX Replay...`.
