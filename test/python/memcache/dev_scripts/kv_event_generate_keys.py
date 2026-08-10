#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Generate a ZMQ CURVE keypair (Z85 encoded) for kv_events encryption.

Usage: python generate_key.py

- Server (MemCache kv_events publisher): keep secret_key locally, configure
  curve_server_public_key + curve_server_secret_key.
- Subscriber (e.g. kv-conductor): only needs the public_key as ZMQ_CURVE_SERVERKEY.
"""

import zmq


def main() -> None:
    public_key, secret_key = zmq.curve_keypair()
    print("public_key: {}".format(public_key.decode("ascii")))
    print("secret_key: {}".format(secret_key.decode("ascii")))


if __name__ == "__main__":
    main()
