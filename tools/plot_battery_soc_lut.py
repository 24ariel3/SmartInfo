"""Plot the battery voltage-to-SOC lookup used by components/battery/src/battery.c."""

from pathlib import Path

import matplotlib.pyplot as plt


# Keep these points in sync with battery_soc_lut in components/battery/src/battery.c.
# The table's "<1%" point at 3050 mV is represented by its midpoint, 0.5%.
BATTERY_SOC_LUT = [
    (3000, 0.0), (3050, 0.5), (3100, 1.0), (3150, 1.5), (3200, 2.0),
    (3250, 4.0), (3300, 9.0), (3350, 13.0), (3400, 17.0), (3450, 21.0),
    (3500, 27.0), (3550, 35.0), (3600, 44.0), (3650, 52.0), (3700, 57.0),
    (3750, 62.0), (3800, 67.0), (3850, 73.0), (3900, 79.0), (3950, 84.0),
    (4000, 88.0), (4050, 92.0), (4100, 97.0), (4150, 100.0),
]


def voltage_to_percent(millivolts: int) -> int:
    if millivolts <= BATTERY_SOC_LUT[0][0]:
        return int(BATTERY_SOC_LUT[0][1] + 0.5)

    for index in range(1, len(BATTERY_SOC_LUT)):
        high_mv, high_percent = BATTERY_SOC_LUT[index]
        if millivolts <= high_mv:
            low_mv, low_percent = BATTERY_SOC_LUT[index - 1]
            mv_range = high_mv - low_mv
            percent = low_percent + (millivolts - low_mv) / mv_range * (high_percent - low_percent)
            return int(percent + 0.5)

    return int(BATTERY_SOC_LUT[-1][1] + 0.5)


def main() -> None:
    voltages_mv = range(2800, 4401)
    percents = [voltage_to_percent(mv) for mv in voltages_mv]

    fig, ax = plt.subplots(figsize=(10, 5.7), dpi=150)
    ax.plot(
        [mv / 1000 for mv in voltages_mv],
        percents,
        color="#1769aa",
        linewidth=2.5,
        label="Firmware output (rounded to integer)",
    )
    ax.scatter(
        [mv / 1000 for mv, _ in BATTERY_SOC_LUT],
        [percent for _, percent in BATTERY_SOC_LUT],
        color="#e4572e",
        s=30,
        zorder=3,
        label="LUT entries (before rounding)",
    )
    ax.axvline(3.0, color="#555", linestyle="--", linewidth=1)
    ax.axvline(4.2, color="#555", linestyle="--", linewidth=1)
    ax.annotate(
        "0% at 3.0 V",
        (3.0, 0),
        xytext=(3.08, 12),
        arrowprops={"arrowstyle": "->", "color": "#555"},
        fontsize=9,
    )
    ax.annotate(
        "100% at 4.2 V",
        (4.2, 100),
        xytext=(3.82, 86),
        arrowprops={"arrowstyle": "->", "color": "#555"},
        fontsize=9,
    )
    ax.set(
        title="Battery voltage-to-SOC lookup",
        xlabel="Battery voltage (V)",
        ylabel="Reported charge (%)",
    )
    ax.set_xlim(2.8, 4.4)
    ax.set_ylim(-3, 103)
    ax.set_xticks([2.8, 3.0, 3.2, 3.4, 3.6, 3.8, 4.0, 4.2, 4.4])
    ax.set_yticks(range(0, 101, 10))
    ax.grid(True, alpha=0.28)
    ax.legend(loc="upper left", frameon=True)
    fig.text(
        0.5,
        0.01,
        "This shows the programmed estimate; it is not a measured discharge curve.",
        ha="center",
        fontsize=9,
        color="#555",
    )
    fig.tight_layout(rect=(0, 0.04, 1, 1))

    output_path = Path("build") / "battery_soc_lut.png"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, bbox_inches="tight")
    print(output_path)


if __name__ == "__main__":
    main()
