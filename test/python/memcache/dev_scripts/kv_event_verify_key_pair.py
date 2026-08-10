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

"""Verify a ZMQ CURVE server keypair in a single process.

Spins up a PUB (server) and SUB (client) in one process and checks whether the
subscriber can receive a message over the CURVE-encrypted channel.

Usage:
  python kv_event_verify_key_pair.py

Fill PUBLIC_KEY / SECRET_KEY below with a keypair produced by
kv_event_generate_keys.py before running. Expected output:
  == Test 1: plaintext ==  -> OK
  == Test 2: CURVE + client keypair == -> OK
"""

import time

import zmq

PUBLIC_KEY = ""
SECRET_KEY = ""

print("zmq:", zmq.zmq_version(), "| pyzmq:", zmq.pyzmq_version())


def run_test(name, use_curve):
    ctx = zmq.Context()
    pub = ctx.socket(zmq.PUB)
    pub.linger = 0
    sub = ctx.socket(zmq.SUB)
    sub.linger = 0
    try:
        if use_curve:
            pub.curve_publickey = PUBLIC_KEY.encode()
            pub.curve_secretkey = SECRET_KEY.encode()
            pub.curve_server = True
        pub.bind("tcp://127.0.0.1:5999")

        if use_curve:
            cpub, csec = zmq.curve_keypair()
            sub.curve_publickey = cpub
            sub.curve_secretkey = csec
            sub.curve_serverkey = PUBLIC_KEY.encode()
        sub.setsockopt_string(zmq.SUBSCRIBE, "kv")
        sub.connect("tcp://127.0.0.1:5999")

        time.sleep(3)
        for i in range(5):
            pub.send_multipart([b"kv", b"hello-%d" % i])
            poller = zmq.Poller()
            poller.register(sub, zmq.POLLIN)
            if poller.poll(1500):
                print(f"  [{name}] RECEIVED: {sub.recv_multipart()}")
                return True
            print(f"  [{name}] attempt {i}: no data")
        return False
    finally:
        pub.close(linger=0)
        sub.close(linger=0)
        ctx.term()


print("== Test 1: plaintext ==")
r1 = run_test("plain", False)
print("  result:", "OK" if r1 else "FAIL")

print("== Test 2: CURVE + client keypair ==")
r2 = run_test("curve", True)
print("  result:", "OK" if r2 else "FAIL")
