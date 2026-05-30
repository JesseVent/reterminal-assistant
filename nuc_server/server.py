"""
reTerminal D1001 Voice Assistant — NUC Server (Phase 1: Loopback)

Usage:
    pip install websockets
    python server.py

This server accepts a WebSocket connection from the D1001 and echoes
audio back (loopback mode). This validates the full audio pipeline
before adding STT / LLM / TTS in later phases.

Protocol:
    - Binary frames: raw PCM audio (16kHz, 16-bit, mono) — looped back
    - Text frames: JSON metadata (reserved for future use)

Endpoints:
    ws://0.0.0.0:8080/assistant
"""

import asyncio
import json
import logging
import sys
import time

try:
    import websockets
except ImportError:
    print("Error: 'websockets' package not found. Install with: pip install websockets")
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("assistant-server")

# Track stats per connection
class ConnectionStats:
    def __init__(self):
        self.audio_frames_sent = 0
        self.audio_frames_recv = 0
        self.audio_bytes_recv = 0
        self.text_messages = 0
        self.connected_at = None
        self.last_audio_at = None

    def summary(self):
        dur = time.monotonic() - self.connected_at if self.connected_at else 0
        return (
            f"duration={dur:.1f}s "
            f"audio_in={self.audio_bytes_recv}B ({self.audio_frames_recv} frames) "
            f"audio_out={self.audio_frames_sent} frames "
            f"text={self.text_messages}"
        )


async def assistant_handler(websocket):
    """Handle a single D1001 client connection."""
    stats = ConnectionStats()
    stats.connected_at = time.monotonic()
    remote = websocket.remote_address
    logger.info(f"Client connected from {remote}")

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                # Binary frame: PCM audio from D1001 microphone
                stats.audio_frames_recv += 1
                stats.audio_bytes_recv += len(message)
                stats.last_audio_at = time.monotonic()

                # Loopback: echo audio back immediately
                await websocket.send(message)
                stats.audio_frames_sent += 1

                # Log every 50th frame to avoid spam
                if stats.audio_frames_recv % 50 == 0:
                    ms = len(message) / 32  # 16kHz * 2 bytes = 32 bytes/ms
                    logger.debug(
                        f"Audio frame #{stats.audio_frames_recv}: "
                        f"{len(message)}B ({ms:.1f}ms)"
                    )

            elif isinstance(message, str):
                # Text frame: JSON metadata
                stats.text_messages += 1
                try:
                    data = json.loads(message)
                    msg_type = data.get("type", "unknown")
                    logger.info(f"Text message: type={msg_type} data={data}")

                    # Echo back acknowledgment
                    await websocket.send(json.dumps({
                        "type": "ack",
                        "for": msg_type,
                    }))
                except json.JSONDecodeError:
                    logger.warning(f"Invalid JSON: {message[:200]}")

    except websockets.exceptions.ConnectionClosed as e:
        logger.info(f"Client {remote} disconnected: code={e.code} reason={e.reason}")
    except Exception as e:
        logger.error(f"Error handling client {remote}: {e}")
    finally:
        logger.info(f"Client {remote} session ended: {stats.summary()}")


async def main():
    host = "0.0.0.0"
    port = 8080

    logger.info(f"Assistant server starting on ws://{host}:{port}")
    logger.info("Loopback mode: audio will be echoed back to the D1001")
    logger.info("Press Ctrl+C to stop")

    async with websockets.serve(
        assistant_handler,
        host,
        port,
        # Allow large audio frames (64KB max)
        max_size=65536,
        # Close connections that send pings but no data for 30s
        ping_interval=20,
        ping_timeout=10,
    ):
        await asyncio.Future()  # Run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server stopped")
