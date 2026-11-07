"""Plot the firmware's voltage-to-usable-energy LUT (before temporal filtering)."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = (ROOT / 'components/battery/src/battery.c').read_text(encoding='utf-8')
LUT_MIN_MV, LUT_MAX_MV, LUT_STEP_MV = (
    int(re.search(r'^#define BATT_LUT_' + name + r'_MV\s+(\d+)', FIRMWARE, re.M)[1])
    for name in ('MIN', 'MAX', 'STEP')
)
LUT_PERMILLE = [
    int(value.strip())
    for value in re.search(
        r'battery_soc_lut_permille\[\]\s*=\s*\{([^}]+)\}', FIRMWARE
    )[1].split(',')
    if value.strip()
]
if (len(LUT_PERMILLE) != (LUT_MAX_MV - LUT_MIN_MV) // LUT_STEP_MV + 1
        or LUT_PERMILLE[0] != 0 or LUT_PERMILLE[-1] != 1000
        or any(low > high for low, high in zip(LUT_PERMILLE, LUT_PERMILLE[1:]))):
    raise ValueError('Firmware battery LUT has invalid bounds, length or order')


def voltage_to_percent(millivolts: int) -> int:
    if millivolts <= LUT_MIN_MV:
        return 0
    if millivolts >= LUT_MAX_MV:
        return 100
    index, remainder = divmod(millivolts - LUT_MIN_MV, LUT_STEP_MV)
    low, high = LUT_PERMILLE[index:index + 2]
    numerator = low * LUT_STEP_MV + remainder * (high - low)
    return (numerator + 5 * LUT_STEP_MV) // (10 * LUT_STEP_MV)


def main() -> None:
    import matplotlib.pyplot as plt

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
        [mv / 1000 for mv in range(LUT_MIN_MV, LUT_MAX_MV + 1, LUT_STEP_MV)],
        [value / 10 for value in LUT_PERMILLE],
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
        "Upper clamp at 4.2 V",
        (4.2, 100),
        xytext=(3.82, 86),
        arrowprops={"arrowstyle": "->", "color": "#555"},
        fontsize=9,
    )
    ax.set(
        title="Battery voltage-to-usable-energy lookup",
        xlabel="Battery voltage (V)",
        ylabel="Estimated usable energy remaining (%)",
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
        "Bayreuth NCR18650B, 0.05C / 25 C; provisional reference, not calibrated to this device.",
        ha="center",
        fontsize=9,
        color="#555",
    )
    fig.tight_layout(rect=(0, 0.04, 1, 1))

    output_path = ROOT / "build" / "battery_soc_lut.png"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, bbox_inches="tight")
    print(output_path)


if __name__ == "__main__":
    main()
