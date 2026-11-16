# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from .random import RandomWorkload
from .trace import TraceWorkload

__all__ = ["RandomWorkload", "TraceWorkload"]
