#!/usr/bin/env python3
"""
ROS2 Bridge Test Client

This script connects to the pj_ros_bridge server and allows testing
of all API operations including topic discovery, subscription, and
receiving aggregated messages.
"""

import argparse
import json
import struct
import sys
import threading
import time
from collections import defaultdict
from datetime import datetime

import zmq
import zstandard as zstd


class BridgeClient:
    """Client for pj_ros_bridge server"""

    def __init__(self, req_host="localhost", req_port=5555, pub_port=5556):
        """
        Initialize the bridge client

        Args:
            req_host: Server hostname for REQ-REP socket
            req_port: Server port for REQ-REP socket (API)
            pub_port: Server port for PUB-SUB socket (data stream)
        """
        self.req_host = req_host
        self.req_port = req_port
        self.pub_port = pub_port

        # ZeroMQ context and sockets
        self.context = zmq.Context()
        self.req_socket = None
        self.sub_socket = None

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
        # Create REQ socket for API
        self.req_socket = self.context.socket(zmq.REQ)
        req_addr = f"tcp://{self.req_host}:{self.req_port}"
        self.req_socket.connect(req_addr)
        print(f"Connected to API socket: {req_addr}")

        # Create SUB socket for data stream
        self.sub_socket = self.context.socket(zmq.SUB)
        pub_addr = f"tcp://{self.req_host}:{self.pub_port}"
        self.sub_socket.connect(pub_addr)
        self.sub_socket.setsockopt(zmq.SUBSCRIBE, b"")  # Subscribe to all messages
        print(f"Connected to data stream socket: {pub_addr}")

        self.start_time = time.time()

    def disconnect(self):
        """Disconnect from the server"""
        self.stop_heartbeat()

        if self.req_socket:
            self.req_socket.close()
        if self.sub_socket:
            self.sub_socket.close()

        print("Disconnected from server")

    def send_request(self, command_data):
        """
        Send a request to the server

        Args:
            command_data: Dictionary containing the request

        Returns:
            Dictionary containing the response
        """
        request_json = json.dumps(command_data)
        self.req_socket.send_string(request_json)

        response_json = self.req_socket.recv_string()
        return json.loads(response_json)

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
            Dictionary mapping topic names to schemas
        """
        request = {"command": "subscribe", "topics": topics}
        response = self.send_request(request)

        if response.get("status") == "success":
            return response.get("schemas", {})
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to subscribe: {error_msg}")

    def send_heartbeat(self):
        """Send a heartbeat to the server"""
        request = {"command": "heartbeat"}
        response = self.send_request(request)

        if response.get("status") != "ok":
            error_msg = response.get("message", "Unknown error")
            print(f"Warning: Heartbeat failed: {error_msg}")

    def start_heartbeat(self, interval=1.0):
        """
        Start heartbeat thread

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

    def decompress_zstd(self, compressed_data):
        """
        Decompress ZSTD data

        Args:
            compressed_data: Compressed bytes

        Returns:
            Decompressed bytes
        """
        return self.decompressor.decompress(compressed_data)

    def deserialize_aggregated_messages(self, data):
        """
        Deserialize aggregated messages from binary format

        Format:
        - Number of messages (uint32_t)
        - For each message:
          - Topic name length (uint16_t)
          - Topic name (N bytes UTF-8)
          - Publish timestamp (uint64_t nanoseconds)
          - Receive timestamp (uint64_t nanoseconds)
          - Message data length (uint32_t)
          - Message data (N bytes)

        Args:
            data: Decompressed binary data

        Returns:
            List of message dictionaries
        """
        messages = []
        offset = 0

        # Read message count
        if len(data) < 4:
            return messages

        msg_count = struct.unpack("<I", data[offset : offset + 4])[0]
        offset += 4

        # Read each message
        for _ in range(msg_count):
            if offset >= len(data):
                break

            # Topic name length
            topic_len = struct.unpack("<H", data[offset : offset + 2])[0]
            offset += 2

            # Topic name
            topic_name = data[offset : offset + topic_len].decode("utf-8")
            offset += topic_len

            # Publish timestamp
            pub_time_ns = struct.unpack("<Q", data[offset : offset + 8])[0]
            offset += 8

            # Receive timestamp
            recv_time_ns = struct.unpack("<Q", data[offset : offset + 8])[0]
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
                    "publish_time_ns": pub_time_ns,
                    "receive_time_ns": recv_time_ns,
                    "data": msg_data,
                }
            )

        return messages

    def receive_messages(self, timeout_ms=100):
        """
        Receive and deserialize aggregated messages

        Args:
            timeout_ms: Receive timeout in milliseconds

        Returns:
            List of message dictionaries, or None if timeout
        """
        try:
            # Set receive timeout
            self.sub_socket.setsockopt(zmq.RCVTIMEO, timeout_ms)

            # Receive compressed data
            compressed_data = self.sub_socket.recv()

            # Decompress
            decompressed_data = self.decompress_zstd(compressed_data)

            # Deserialize
            messages = self.deserialize_aggregated_messages(decompressed_data)

            # Update statistics
            for msg in messages:
                topic = msg["topic"]
                self.stats[topic]["count"] += 1
                self.stats[topic]["bytes"] += len(msg["data"])

            return messages

        except zmq.Again:
            # Timeout
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
    parser = argparse.ArgumentParser(description="pj_ros_bridge test client")

    parser.add_argument(
        "--server",
        default="localhost",
        help="Server hostname (default: localhost)",
    )
    parser.add_argument(
        "--req-port",
        type=int,
        default=5555,
        help="REQ-REP port (default: 5555)",
    )
    parser.add_argument(
        "--pub-port",
        type=int,
        default=5556,
        help="PUB-SUB port (default: 5556)",
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
    client = BridgeClient(args.server, args.req_port, args.pub_port)

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
            for topic in schemas.keys():
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
                    messages = client.receive_messages(timeout_ms=100)

                    if messages:
                        msg_count += len(messages)

                        if args.verbose:
                            for msg in messages:
                                # Calculate latency
                                latency_ns = msg["receive_time_ns"] - msg["publish_time_ns"]
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
