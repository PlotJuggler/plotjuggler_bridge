#!/usr/bin/env python3
"""
ROS2 Bridge Test Client (WebSocket)

This script connects to the pj_ros_bridge server over WebSocket and allows
testing of all API operations including topic discovery, subscription, and
receiving aggregated messages.

Dependencies:
    pip install websocket-client zstandard
"""

import argparse
import json
import struct
import sys
import threading
import time
from collections import defaultdict, deque
from datetime import datetime

import websocket
import zstandard as zstd


class BridgeClient:
    """Client for pj_ros_bridge server over WebSocket"""

    # Binary frame header constants
    HEADER_SIZE = 16
    MAGIC_PJRB = 0x42524A50  # "PJRB" in ASCII (little-endian)

    def __init__(self, host="localhost", port=9090):
        """
        Initialize the bridge client

        Args:
            host: Server hostname
            port: Server WebSocket port
        """
        self.url = f"ws://{host}:{port}"
        self.ws = None

        # Buffer for binary frames received while waiting for a text response
        self._pending_binary = deque()

        # Lock for send operations (recv is single-threaded from main loop)
        self._send_lock = threading.Lock()

        # Heartbeat thread
        self.heartbeat_thread = None
        self.heartbeat_running = False

        # Statistics
        self.stats = defaultdict(lambda: {"count": 0, "bytes": 0})
        self.start_time = None

        # ZSTD decompressor
        self.decompressor = zstd.ZstdDecompressor()

    def connect(self):
        """Connect to the bridge server"""
        self.ws = websocket.create_connection(self.url)
        print(f"Connected to WebSocket server: {self.url}")
        self.start_time = time.time()

    def disconnect(self):
        """Disconnect from the server"""
        self.stop_heartbeat()

        if self.ws:
            self.ws.close()

        print("Disconnected from server")

    def send_request(self, command_data):
        """
        Send a JSON request and wait for the JSON response.

        Any binary frames received while waiting are buffered for later
        retrieval via receive_messages().

        Args:
            command_data: Dictionary containing the request

        Returns:
            Dictionary containing the response
        """
        with self._send_lock:
            self.ws.send(json.dumps(command_data))

        # Read frames until we get a text response
        while True:
            opcode, data = self.ws.recv_data()
            if opcode == websocket.ABNF.OPCODE_TEXT:
                return json.loads(data.decode("utf-8"))
            elif opcode == websocket.ABNF.OPCODE_BINARY:
                # Buffer binary data for later
                self._pending_binary.append(data)

    def get_topics(self):
        """
        Get list of available topics from the server

        Returns:
            List of topic dictionaries with 'name' and 'type' keys
        """
        request = {"command": "get_topics"}
        response = self.send_request(request)

        if response.get("status") == "success":
            return response.get("topics", [])
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to get topics: {error_msg}")

    def subscribe(self, topics):
        """
        Subscribe to topics

        Args:
            topics: List of topic names to subscribe to

        Returns:
            Dictionary mapping topic names to schema objects.
            Each schema object has:
              - "encoding": Schema encoding (e.g., "ros2msg")
              - "definition": The actual schema definition string
        """
        request = {"command": "subscribe", "topics": topics}
        response = self.send_request(request)

        if response.get("status") in ("success", "partial_success"):
            return response.get("schemas", {})
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to subscribe: {error_msg}")

    def send_heartbeat(self):
        """Send a heartbeat to the server (thread-safe for send, but not recv)"""
        with self._send_lock:
            self.ws.send(json.dumps({"command": "heartbeat"}))
        # Note: response will be picked up by the main recv loop

    def start_heartbeat(self, interval=1.0):
        """
        Start heartbeat thread.

        The heartbeat thread only sends; responses are consumed by the
        main receive loop and silently discarded.

        Args:
            interval: Heartbeat interval in seconds (default: 1.0)
        """
        self.heartbeat_running = True

        def heartbeat_loop():
            while self.heartbeat_running:
                try:
                    self.send_heartbeat()
                except Exception as e:
                    print(f"Heartbeat error: {e}")
                time.sleep(interval)

        self.heartbeat_thread = threading.Thread(target=heartbeat_loop, daemon=True)
        self.heartbeat_thread.start()
        print(f"Heartbeat thread started (interval: {interval}s)")

    def stop_heartbeat(self):
        """Stop heartbeat thread"""
        if self.heartbeat_running:
            self.heartbeat_running = False
            if self.heartbeat_thread:
                self.heartbeat_thread.join(timeout=2.0)
            print("Heartbeat thread stopped")

    def deserialize_aggregated_messages(self, data):
        """
        Deserialize aggregated messages from binary format (streaming, no header).

        Format per message:
          - Topic name length (uint16_t)
          - Topic name (N bytes UTF-8)
          - Timestamp (uint64_t nanoseconds)
          - Message data length (uint32_t)
          - Message data (N bytes)

        Args:
            data: Decompressed binary data

        Returns:
            List of message dictionaries
        """
        messages = []
        offset = 0

        while offset < len(data):
            if offset + 2 > len(data):
                break

            # Topic name length
            topic_len = struct.unpack("<H", data[offset : offset + 2])[0]
            offset += 2

            # Topic name
            topic_name = data[offset : offset + topic_len].decode("utf-8")
            offset += topic_len

            # Timestamp
            timestamp_ns = struct.unpack("<Q", data[offset : offset + 8])[0]
            offset += 8

            # Message data length
            data_len = struct.unpack("<I", data[offset : offset + 4])[0]
            offset += 4

            # Message data
            msg_data = data[offset : offset + data_len]
            offset += data_len

            messages.append(
                {
                    "topic": topic_name,
                    "timestamp_ns": timestamp_ns,
                    "data": msg_data,
                }
            )

        return messages

    def _process_binary_frame(self, binary_data):
        """
        Parse header, decompress, and deserialize a single binary frame.

        Binary frame format (API v2):
          - 16-byte header (before compression):
            - Offset 0: uint32_t magic (0x42524A50 = "PJRB")
            - Offset 4: uint32_t message_count
            - Offset 8: uint32_t uncompressed_size
            - Offset 12: uint32_t flags (reserved)
          - ZSTD-compressed payload (after header)

        Args:
            binary_data: Raw binary frame data including header

        Returns:
            List of message dictionaries, or empty list on error
        """
        # Validate minimum size for header
        if len(binary_data) < self.HEADER_SIZE:
            print(f"Binary frame too small: {len(binary_data)} bytes (need {self.HEADER_SIZE})")
            return []

        # Parse 16-byte header (little-endian)
        magic, msg_count, uncompressed_size, flags = struct.unpack(
            "<IIII", binary_data[: self.HEADER_SIZE]
        )

        # Validate magic bytes
        if magic != self.MAGIC_PJRB:
            print(f"Invalid magic: 0x{magic:08X} (expected 0x{self.MAGIC_PJRB:08X})")
            return []

        # Extract compressed payload (after header)
        compressed_payload = binary_data[self.HEADER_SIZE :]

        # Decompress payload
        decompressed = self.decompressor.decompress(compressed_payload)

        # Verify uncompressed size matches header
        if len(decompressed) != uncompressed_size:
            print(
                f"Size mismatch: got {len(decompressed)} bytes, "
                f"expected {uncompressed_size} bytes"
            )

        # Deserialize messages
        messages = self.deserialize_aggregated_messages(decompressed)

        # Verify message count matches header
        if len(messages) != msg_count:
            print(
                f"Message count mismatch: got {len(messages)}, "
                f"expected {msg_count} from header"
            )

        # Update statistics
        for msg in messages:
            topic = msg["topic"]
            self.stats[topic]["count"] += 1
            self.stats[topic]["bytes"] += len(msg["data"])

        return messages

    def receive_messages(self, timeout=0.1):
        """
        Receive and deserialize aggregated messages.

        Checks buffered binary frames first, then reads from the socket.
        Text frames (heartbeat responses) are silently discarded.

        Args:
            timeout: Receive timeout in seconds

        Returns:
            List of message dictionaries, or None if timeout
        """
        # Check buffered binary frames first
        if self._pending_binary:
            compressed_data = self._pending_binary.popleft()
            return self._process_binary_frame(compressed_data)

        # Read from socket with timeout
        self.ws.settimeout(timeout)
        try:
            opcode, data = self.ws.recv_data()
            if opcode == websocket.ABNF.OPCODE_BINARY:
                return self._process_binary_frame(data)
            elif opcode == websocket.ABNF.OPCODE_TEXT:
                # Heartbeat response or other text - discard
                return None
        except websocket.WebSocketTimeoutException:
            return None
        except Exception as e:
            print(f"Error receiving messages: {e}")
            return None

    def print_statistics(self):
        """Print statistics about received messages"""
        if not self.stats:
            print("\nNo messages received")
            return

        elapsed = time.time() - self.start_time
        print(f"\n{'=' * 80}")
        print("Statistics")
        print(f"{'=' * 80}")
        print(f"Elapsed time: {elapsed:.2f} seconds")
        print(f"\n{'Topic':<40} {'Count':>10} {'Bytes':>12} {'Rate (Hz)':>12}")
        print(f"{'-' * 80}")

        total_count = 0
        total_bytes = 0

        for topic in sorted(self.stats.keys()):
            count = self.stats[topic]["count"]
            bytes_val = self.stats[topic]["bytes"]
            rate = count / elapsed if elapsed > 0 else 0

            print(f"{topic:<40} {count:>10} {bytes_val:>12} {rate:>12.2f}")

            total_count += count
            total_bytes += bytes_val

        print(f"{'-' * 80}")
        total_rate = total_count / elapsed if elapsed > 0 else 0
        print(f"{'TOTAL':<40} {total_count:>10} {total_bytes:>12} {total_rate:>12.2f}")
        print(f"{'=' * 80}\n")


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(description="pj_ros_bridge test client (WebSocket)")

    parser.add_argument(
        "--server",
        default="localhost",
        help="Server hostname (default: localhost)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=9090,
        help="WebSocket port (default: 9090)",
    )
    parser.add_argument(
        "--command",
        choices=["get_topics", "subscribe"],
        help="Command to execute",
    )
    parser.add_argument(
        "--subscribe",
        nargs="+",
        metavar="TOPIC",
        help="Topics to subscribe to",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=10.0,
        help="Duration to receive messages in seconds (default: 10)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print each received message",
    )

    args = parser.parse_args()

    # Create client
    client = BridgeClient(args.server, args.port)

    try:
        # Connect to server
        client.connect()

        # Execute command
        if args.command == "get_topics" or (args.command is None and args.subscribe is None):
            # Get and display topics
            print("\nQuerying available topics...")
            topics = client.get_topics()

            print(f"\nAvailable topics ({len(topics)}):")
            print(f"{'Topic Name':<50} {'Type':<50}")
            print("-" * 100)
            for topic in topics:
                print(f"{topic['name']:<50} {topic['type']:<50}")

        elif args.subscribe or args.command == "subscribe":
            # Subscribe to topics
            topics = args.subscribe if args.subscribe else []

            if not topics:
                print("Error: No topics specified for subscription")
                return 1

            print(f"\nSubscribing to {len(topics)} topic(s)...")
            schemas = client.subscribe(topics)

            print(f"Successfully subscribed to {len(schemas)} topic(s)")
            for topic, schema_obj in schemas.items():
                # Handle both old (string) and new (object) schema formats
                if isinstance(schema_obj, dict):
                    encoding = schema_obj.get("encoding", "unknown")
                    definition = schema_obj.get("definition", "")
                    def_preview = definition[:50] + "..." if len(definition) > 50 else definition
                    def_preview = def_preview.replace("\n", " ")
                    print(f"  - {topic} (encoding={encoding}, schema={def_preview})")
                else:
                    # Legacy string format
                    print(f"  - {topic}")

            # Start heartbeat
            client.start_heartbeat()

            # Receive messages
            print(f"\nReceiving messages for {args.duration} seconds...")
            print("Press Ctrl+C to stop\n")

            start_time = time.time()
            msg_count = 0

            try:
                while time.time() - start_time < args.duration:
                    messages = client.receive_messages(timeout=0.1)

                    if messages:
                        msg_count += len(messages)

                        if args.verbose:
                            for msg in messages:
                                now_ns = int(time.time() * 1_000_000_000)
                                latency_ns = now_ns - msg["timestamp_ns"]
                                latency_ms = latency_ns / 1_000_000

                                print(
                                    f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] "
                                    f"{msg['topic']}: {len(msg['data'])} bytes, "
                                    f"latency: {latency_ms:.2f} ms"
                                )

            except KeyboardInterrupt:
                print("\n\nInterrupted by user")

            # Print statistics
            client.print_statistics()

    except Exception as e:
        print(f"Error: {e}")
        return 1

    finally:
        client.disconnect()

    return 0


if __name__ == "__main__":
    sys.exit(main())
