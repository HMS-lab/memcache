# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from pathlib import Path

import matplotlib.pyplot as plt
from pandas import DataFrame


def plot_memory(samples: DataFrame, filepath: Path) -> None:
    figure, (ax1, ax2) = plt.subplots(2, 1, sharex=True, layout="constrained")
    figure.suptitle("MetaManager memory")

    try:
        # Native RAM usage at each sampled read count
        ax1.scatter(samples["reads"], samples["rss_bytes"] / (1 << 20), s=0.1)
        ax1.set_ylabel("Native RSS (MiB)")

        # Number of cached keys at each sampled read count
        ax2.scatter(samples["reads"], samples["cached_keys"], s=0.1)
        ax2.set(xlabel="Completed reads", ylabel="Cached keys")

        filepath.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(filepath, dpi=300)
    finally:
        plt.close(figure)
