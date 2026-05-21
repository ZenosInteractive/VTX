#!/usr/bin/env python3
"""Minimal WebSocket server that pushes synthetic JSON frames.

Companion producer for the C++ `vtx_sample_websocket_consumer`.  Each
WebSocket message is one frame, matching the shape the sample's
JsonWebSocketAdapter expects:

    {
      "game_time": 0.0167,
      "entities": [
        {"id": "player_0", "type_id": 0,
         "floats": [100.5],
         "translation": [0.0, 0.0, 50.0]}
      ]
    }

Requirements
    pip install websockets

Usage
    python websocket_server.py [num_frames] [delay_ms] [host] [port]

    num_frames  default: 0 -> stream continuously until the client
                disconnects.  A value > 0 sends exactly that many
                frames and then closes the connection.
    delay_ms    default: 16   (~60 fps; 0 = as fast as possible)
    host        default: 127.0.0.1
    port        default: 8765

Example
    python websocket_server.py          # stream until the consumer disconnects
    python websocket_server.py 200 16   # send exactly 200 frames, then close
    # then, in another shell:
    vtx_sample_websocket_consumer ws://127.0.0.1:8765/ out.vtx schema.json
"""

import asyncio
import json
import sys

import websockets

NUM_FRAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 0  # 0 = until disconnect
DELAY_MS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
HOST = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 8765

FPS = 60.0
ENTITIES_PER_FRAME = 20


def build_entity(frame_index: int, entity_index: int) -> dict:
    """Builds one synthetic entity for a frame."""
    return {
        "id": f"player_{entity_index}",
        "type_id": 0,
        # a "health"-like value that decays over the frames, clamped at 0
        "floats": [max(0.0, 100.0 - float(frame_index))],
        # spread entities out along X, drift them along Z over time
        "translation": [float(entity_index) * 10.0, 0.0, float(frame_index)],
    }


def build_frame(frame_index: int) -> dict:
    """Builds one synthetic frame with ENTITIES_PER_FRAME entities."""
    return {
        "game_time": frame_index / FPS,
        "entities": [build_entity(frame_index, e) for e in range(ENTITIES_PER_FRAME)],
    }


async def produce(websocket, *_):
    """Streams JSON frames to a connected client.

    With NUM_FRAMES == 0 it streams continuously until the client
    disconnects; with NUM_FRAMES > 0 it sends exactly that many frames
    and then closes the connection itself.

    The trailing *_ swallows the `path` argument passed by older
    `websockets` releases (<11), keeping this handler version-agnostic.
    """
    if NUM_FRAMES > 0:
        print(f"client connected -- streaming {NUM_FRAMES} frames")
    else:
        print("client connected -- streaming until the client disconnects")

    sent = 0
    try:
        while NUM_FRAMES == 0 or sent < NUM_FRAMES:
            await websocket.send(json.dumps(build_frame(sent)))
            sent += 1
            if DELAY_MS > 0:
                await asyncio.sleep(DELAY_MS / 1000.0)
    except websockets.ConnectionClosed:
        print(f"client disconnected -- sent {sent} frames")
        return

    print(f"done -- sent {sent} frames, closing connection")
    await websocket.close()


async def main():
    print(f"WebSocket server listening on ws://{HOST}:{PORT}")
    async with websockets.serve(produce, HOST, PORT):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nserver stopped")
