# vtx_inspector Usage

This document covers how to use the Inspector GUI.

## 1. Start the App

Open `VTX Inspector` from your installed applications.

## 2. Open a Replay

1. In the app menu, go to `File` -> `Open VTX Replay...`
2. Select a `.vtx` file.
3. Optional: use `File` -> `Open Recent` for previously opened files.
4. Use `File` -> `Close Replay` to unload the current file.

## 3. Main Workflow

1. Use `Timeline` to move through frames.
2. Use `Buckets` to find and select an entity.
3. Inspect selected entity fields in `Entity Details`.
4. Cross-reference structure and property mapping in `Contextual Schema`.

## 4. Window Usage

### Timeline

- Drag the slider to scrub frames.
- Use `Go to` and press Enter to jump to a frame.
- Click bars in the lower frame strip to seek.
- Hold `Ctrl` + mouse wheel to zoom the frame strip.

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
