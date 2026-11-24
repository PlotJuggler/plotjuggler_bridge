#!/usr/bin/env python3
"""
Slow-link relay for testing pj_bridge with a slow client.

A TCP relay that sits between a client (PlotJuggler, test_client.py, a browser)
and pj_bridge. It limits the rate of the data flowing from the bridge to the
client and adds latency in both directions, so the slow-client behaviour can be
reproduced on one machine, without root and without touching other traffic.

    client  --->  slow_link.py (--listen)  --->  pj_bridge (--target)

The relay reads from the bridge only as fast as it forwards, so the unsent data
piles up in the bridge's socket send buffer, as it does on a real slow link.
It forwards bytes without parsing them, so ws:// and wss:// both work.

Example: 1 Mbit/s toward the client and 80 ms of round-trip time, in front of
a bridge on the default port. Connect the client to ws://localhost:9091.

    python3 slow_link.py --listen 9091 --target 127.0.0.1:9090 --rate-kib 128 --delay-ms 40

No dependencies outside the standard library.
"""

import argparse
import asyncio
import time

# Bytes are forwarded in slices of this duration, so the rate limit is smooth
# at the time scale of the bridge's publish cycle.
SLICE_SECONDS = 0.02
UNLIMITED_CHUNK_BYTES = 64 * 1024


async def read_paced(reader, queue, rate, delay):
    """Read from one side, at most `rate` bytes/s, and stamp each chunk with its release time."""
    chunk_bytes = max(1, int(rate * SLICE_SECONDS)) if rate else UNLIMITED_CHUNK_BYTES
    next_read = time.monotonic()
    try:
        while True:
            data = await reader.read(chunk_bytes)
            if not data:
                break
            await queue.put((time.monotonic() + delay, data))
            if rate:
                next_read = max(next_read, time.monotonic()) + len(data) / rate
                await asyncio.sleep(max(0.0, next_read - time.monotonic()))
    except (ConnectionError, OSError):
        pass
    await queue.put(None)


async def write_delayed(queue, writer):
    """Write the chunks to the other side, in order, each one not before its release time."""
    try:
        while True:
            item = await queue.get()
            if item is None:
                break
            release_time, data = item
            await asyncio.sleep(max(0.0, release_time - time.monotonic()))
            writer.write(data)
            await writer.drain()
    except (ConnectionError, OSError):
        pass
    finally:
        writer.close()


async def relay(reader, writer, rate, delay):
    # The queue holds what is "on the wire" (rate * delay bytes at most). It is
    # bounded so that a stalled writer stops the reader instead of growing memory.
    queue = asyncio.Queue(maxsize=int(delay / SLICE_SECONDS) + 8)
    await asyncio.gather(read_paced(reader, queue, rate, delay), write_delayed(queue, writer))


async def handle_client(client_reader, client_writer, args):
    peer = client_writer.get_extra_info("peername")
    try:
        # A small stream buffer keeps the relay from hiding the backlog: what the
        # client has not received yet stays in the bridge's socket.
        bridge_reader, bridge_writer = await asyncio.open_connection(
            args.target_host, args.target_port, limit=UNLIMITED_CHUNK_BYTES
        )
    except OSError as error:
        print(f"[{peer}] cannot connect to {args.target_host}:{args.target_port}: {error}")
        client_writer.close()
        return

    print(f"[{peer}] connected")
    delay = args.delay_ms / 1000.0
    rate = args.rate_kib * 1024.0
    await asyncio.gather(
        relay(client_reader, bridge_writer, None, delay),
        relay(bridge_reader, client_writer, rate, delay),
    )
    print(f"[{peer}] disconnected")


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
    parser.add_argument("--target", default="127.0.0.1:9090", help="pj_bridge address (default: 127.0.0.1:9090)")
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
    if args.rate_kib < 0 or args.delay_ms < 0:
        parser.error("--rate-kib and --delay-ms must not be negative")
    host, _, port = args.target.rpartition(":")
    if not host or not port.isdigit():
        parser.error("--target must be HOST:PORT")
    args.target_host, args.target_port = host, int(port)
    return args


async def main():
    args = parse_args()
    server = await asyncio.start_server(
        lambda reader, writer: handle_client(reader, writer, args),
        args.listen_host,
        args.listen,
        limit=UNLIMITED_CHUNK_BYTES,
    )
    rate = f"{args.rate_kib:g} KiB/s" if args.rate_kib else "unlimited"
    print(
        f"Listening on {args.listen_host}:{args.listen} -> {args.target_host}:{args.target_port} "
        f"({rate} toward the client, {args.delay_ms:g} ms each way)"
    )
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
