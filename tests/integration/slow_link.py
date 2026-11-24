#!/usr/bin/env python3
"""
Slow-link relay for testing pj_bridge with a slow client.

A TCP relay that sits between a client (PlotJuggler, test_client.py, a browser)
and pj_bridge. It limits the rate of the data flowing from the bridge to the
client and adds latency in both directions, so the slow-client behaviour can be
reproduced on one machine, without root and without touching other traffic.

    client  --->  slow_link.py (--listen)  --->  pj_bridge (--target)

With a rate limit, the relay reads from the bridge only as fast as it forwards
and keeps little data in its own buffers (about 100 KiB, or half a second of
data when that is more), so the backlog builds up in the bridge's socket send
buffer, as on a real slow link. It forwards bytes without parsing them, so
ws:// and wss:// both work.

Example: 1 Mbit/s toward the client and 80 ms of round-trip time, in front of
a bridge on the default port. Connect the client to ws://localhost:9091.

    python3 slow_link.py --listen 9091 --target 127.0.0.1:9090 --rate-kib 128 --delay-ms 40

No dependencies outside the standard library.
"""

import argparse
import asyncio
import math
import socket
import time
from urllib.parse import urlsplit

# A rate-limited direction forwards one chunk per slice, so the limit is smooth
# at the time scale of the bridge's publish cycle.
SLICE_SECONDS = 0.02
MAX_READ_BYTES = 64 * 1024
# Sleeps shorter than this are accumulated instead of taken one by one.
MIN_SLEEP_SECONDS = 0.001
# Upper bound on the data delayed inside the relay, per direction. A direction
# holds rate * delay bytes; one without a rate limit (client to bridge, or
# --rate-kib 0) can fill this bound when a delay is set and its receiver stalls.
MAX_IN_FLIGHT_BYTES = 64 * 1024 * 1024
# Receive buffer of the relay's socket to the bridge, as seconds of data at the
# configured rate. With the kernel default (megabytes on Linux) the relay would
# absorb a backlog that a real slow link leaves in the bridge's send buffer.
# The floor is the smallest size that did not reduce the rate on loopback.
RECEIVE_BUFFER_SECONDS = 0.25
MIN_RECEIVE_BUFFER_BYTES = 32 * 1024


def chunk_bytes(rate):
    return int(min(MAX_READ_BYTES, max(1, rate * SLICE_SECONDS)))


class DelayLine:
    """FIFO that releases each chunk `delay` seconds after it was put, in order.

    Bounded in bytes: put() waits while the line is full, so a stalled writer
    stops the reader instead of growing memory. One producer, one consumer.
    """

    def __init__(self, delay, max_bytes):
        self._delay = delay
        self._max_bytes = max_bytes
        self._chunks = asyncio.Queue()
        self._bytes = 0
        self._has_room = asyncio.Event()
        self._has_room.set()

    async def put(self, data):
        await self._has_room.wait()
        self._bytes += len(data)
        if self._bytes >= self._max_bytes:
            self._has_room.clear()
        self._chunks.put_nowait((time.monotonic() + self._delay, data))

    def close(self):
        self._chunks.put_nowait(None)

    async def get(self):
        """Return the next chunk once its delay has passed, or None after close()."""
        item = await self._chunks.get()
        if item is None:
            return None
        release_time, data = item
        await asyncio.sleep(max(0.0, release_time - time.monotonic()))
        self._bytes -= len(data)
        self._has_room.set()
        return data


async def read_paced(reader, line, rate):
    """Move data from `reader` into `line`, at most `rate` bytes/s (math.inf = no limit)."""
    read_bytes = chunk_bytes(rate)
    next_read = time.monotonic()
    try:
        while data := await reader.read(read_bytes):
            await line.put(data)
            now = time.monotonic()
            # Keep the time owed by late wake-ups, up to one slice, so the average rate holds.
            next_read = max(next_read, now - SLICE_SECONDS) + len(data) / rate
            if next_read - now > MIN_SLEEP_SECONDS:
                await asyncio.sleep(next_read - now)
    except OSError:
        pass
    line.close()


async def write_delayed(line, writer):
    try:
        while (data := await line.get()) is not None:
            writer.write(data)
            await writer.drain()
    except OSError:
        pass
    finally:
        writer.close()


async def relay(reader, writer, rate, delay):
    delayed_bytes = rate * delay if delay else 0.0
    line = DelayLine(delay, min(MAX_IN_FLIGHT_BYTES, delayed_bytes) + chunk_bytes(rate))
    reading = asyncio.create_task(read_paced(reader, line, rate))
    try:
        await write_delayed(line, writer)
    finally:
        # The writer can stop first (peer gone) while the reader waits for room in the line.
        reading.cancel()


async def handle_client(client_reader, client_writer, args):
    peer = client_writer.get_extra_info("peername")
    host, port = args.target
    try:
        bridge_reader, bridge_writer = await asyncio.open_connection(host, port, limit=chunk_bytes(args.rate))
    except (OSError, ValueError) as error:  # ValueError: a host name that cannot be encoded
        print(f"[{peer}] cannot connect to {host}:{port}: {error}")
        client_writer.close()
        return

    if math.isfinite(args.rate):
        receive_buffer_bytes = max(MIN_RECEIVE_BUFFER_BYTES, int(args.rate * RECEIVE_BUFFER_SECONDS))
        bridge_writer.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer_bytes)

    print(f"[{peer}] connected")
    await asyncio.gather(
        relay(client_reader, bridge_writer, math.inf, args.delay),
        relay(bridge_reader, client_writer, args.rate, args.delay),
    )
    print(f"[{peer}] disconnected")


def host_port(text):
    parts = urlsplit("//" + text)
    if not parts.hostname or parts.port is None:
        raise ValueError(text)
    return parts.hostname, parts.port


def parse_args():
    parser = argparse.ArgumentParser(
        description="TCP relay that makes the link between a client and pj_bridge slow.",
    )
    parser.add_argument("--listen", type=int, default=9091, help="port the client connects to (default: 9091)")
    parser.add_argument(
        "--listen-host",
        default="127.0.0.1",
        help="address to listen on; use 0.0.0.0 for a client on another machine (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--target",
        type=host_port,
        default="127.0.0.1:9090",
        metavar="HOST:PORT",
        help="pj_bridge address (default: 127.0.0.1:9090)",
    )
    parser.add_argument(
        "--rate-kib",
        type=float,
        default=128.0,
        help="rate toward the client in KiB/s; 0 = unlimited (default: 128, about 1 Mbit/s)",
    )
    parser.add_argument(
        "--delay-ms",
        type=float,
        default=40.0,
        help="one-way latency added in each direction, in ms (default: 40)",
    )
    args = parser.parse_args()
    if not (0 <= args.rate_kib < math.inf and 0 <= args.delay_ms < math.inf):
        parser.error("--rate-kib and --delay-ms must be finite and not negative")
    args.rate = args.rate_kib * 1024.0 if args.rate_kib else math.inf
    args.delay = args.delay_ms / 1000.0
    return args


async def main():
    args = parse_args()
    connections = set()

    async def on_client(reader, writer):
        connections.add(asyncio.current_task())
        try:
            await handle_client(reader, writer, args)
        finally:
            connections.discard(asyncio.current_task())

    server = await asyncio.start_server(on_client, args.listen_host, args.listen)
    host, port = args.target
    rate = f"{args.rate_kib:g} KiB/s" if args.rate_kib else "unlimited"
    print(
        f"Listening on {args.listen_host}:{args.listen} -> {host}:{port} "
        f"({rate} toward the client, {args.delay_ms:g} ms each way)"
    )
    try:
        await asyncio.Event().wait()
    finally:
        # Server.wait_closed() (used by serve_forever and "async with") waits for the
        # open connections since Python 3.12, so Ctrl-C would not stop a relay in use.
        server.close()
        for connection in connections:
            connection.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
