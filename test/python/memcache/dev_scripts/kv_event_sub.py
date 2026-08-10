#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""
ZMQ SUB subscriber for MemCache KV events.

Listens to the ZMQ PUB endpoint and decodes msgpack-encoded event batches
into human-readable JSON.

Wire format (per ZMQ multipart message):
  Frame 1: topic  (utf-8 string, e.g. "kv" or "kv@model_name")
  Frame 2: seq    (8-byte big-endian uint64 sequence number)
  Frame 3: payload (msgpack array: [event_map, ...])

Usage:
  python3 kv_event_sub.py [--endpoint tcp://127.0.0.1:5557] [--topic kv]
"""

import argparse
import json
import struct
import sys

import msgpack
import zmq
from zmq.utils import z85

# ZMQ CURVE server public key (Z85, 40 chars). Set to the MemCache server's
# public key to enable CURVE encryption; leave empty for plaintext.
SERVER_PUB_KEY = ""


def setup_curve(sock, server_pub_key):
    """Enable ZMQ CURVE (server-only auth) if server_pub_key is valid. Returns True if encrypted."""
    if not server_pub_key:
        return False
    try:
        if len(z85.decode(server_pub_key)) != 32:
            return False
    except Exception as exc:
        print(f"WARNING: invalid CURVE server key, fallback to plaintext: {exc!r}", file=sys.stderr)
        return False
    sock.curve_publickey, sock.curve_secretkey = zmq.curve_keypair()
    sock.curve_serverkey = server_pub_key.encode("ascii")
    return True


def decode_value(v):
    if isinstance(v, bytes):
        try:
            return v.decode("utf-8")
        except UnicodeDecodeError:
            return v.hex()
    if isinstance(v, list):
        return [decode_value(item) for item in v]
    if isinstance(v, dict):
        return {decode_value(k): decode_value(val) for k, val in v.items()}
    return v


def decode_event(event_obj):
    if not isinstance(event_obj, dict):
        return repr(event_obj)
    return {decode_value(k): decode_value(v) for k, v in event_obj.items()}


def decode_batch(payload: bytes):
    obj = msgpack.unpackb(payload, raw=False)
    if isinstance(obj, dict) and "events" in obj:
        events_array = obj["events"]
        decoded_events = [decode_event(e) for e in events_array]
        return {"events": decoded_events}
    if isinstance(obj, (list, tuple)):
        decoded_events = [decode_event(e) for e in obj]
        return {"events": decoded_events}
    return {"raw": repr(obj)}


def main():
    parser = argparse.ArgumentParser(description="MemCache KV event subscriber")
    parser.add_argument(
        "--endpoint",
        default="tcp://127.0.0.1:5557",
        help="ZMQ PUB endpoint to connect to (default: tcp://127.0.0.1:5557)",
    )
    parser.add_argument(
        "--topic",
        default="kv",
        help="ZMQ topic prefix to subscribe to (default: kv, empty string = all)",
    )
    args = parser.parse_args()

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.SUB)
    encrypted = setup_curve(sock, SERVER_PUB_KEY)
    sock.connect(args.endpoint)
    if args.topic:
        sock.setsockopt_string(zmq.SUBSCRIBE, args.topic)
    else:
        sock.setsockopt_string(zmq.SUBSCRIBE, "")

    print(
        f"Subscribed to {args.endpoint} (topic={args.topic!r}, curve={'on' if encrypted else 'off'})",
        file=sys.stderr,
    )
    print("Waiting for events...", file=sys.stderr)

    try:
        while True:
            frames = sock.recv_multipart()
            if len(frames) != 3:
                print(
                    json.dumps({"error": f"expected 3 frames, got {len(frames)}"}),
                    flush=True,
                )
                continue

            topic_frame, seq_frame, payload_frame = frames
            try:
                batch = decode_batch(payload_frame)
            except Exception as exc:
                batch = {"decode_error": str(exc), "payload_hex": payload_frame.hex()}

            output = batch
            print(json.dumps(output, ensure_ascii=False, indent=2), flush=True)
            print(flush=True)
    except KeyboardInterrupt:
        print("\nInterrupted, exiting.", file=sys.stderr)
    finally:
        sock.close(linger=0)


if __name__ == "__main__":
    main()
