"""Regenerate the documentation figure (requires matplotlib, not needed by users)."""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Ellipse, Rectangle

OUT = Path(__file__).resolve().parents[1] / "docs" / "images"
OUT.mkdir(parents=True, exist_ok=True)
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11})
fig = plt.figure(figsize=(13, 7.4), facecolor="#0c1321")
fig.text(.055, .925, "BeamNG-VRS settings", color="white", fontsize=26, weight="bold")
fig.text(.055, .876, "Edit BeamNG-VRS.ini beside the launch CMD, then restart the game", color="#aabbd0", fontsize=13)

def panel(bounds, title):
    ax = fig.add_axes(bounds)
    ax.set(xlim=(0, 900), ylim=(0, 600), aspect="equal")
    ax.axis("off")
    ax.set_title(title, color="white", fontsize=15, loc="left", pad=14)
    return ax

ax = panel([.055, .31, .43, .47], "1. Foveation regions")
ax.add_patch(Rectangle((0, 0), 900, 600, facecolor="#283d53", edgecolor="#75869c", lw=1.5))
ax.add_patch(Ellipse((450, 300), 540, 360, facecolor="#a97835", edgecolor="#ffae42", lw=3))
ax.add_patch(Ellipse((450, 300), 225, 150, facecolor="#74cadd", edgecolor="#ff555c", lw=3))
ax.text(450, 330, "1×1", ha="center", va="center", color="#10202b", fontsize=18, weight="bold")
ax.text(450, 440, "2×2", ha="center", color="white", fontsize=17, weight="bold")
ax.text(755, 525, "4×4", ha="center", color="white", fontsize=17, weight="bold")
ax.annotate("", (337.5, 300), (562.5, 300), arrowprops={"arrowstyle": "<->", "color": "white", "lw": 1.6})
ax.annotate("Size1x1 = 0.25", (450, 230), (470, 90), color="white", fontsize=12,
            ha="center", arrowprops={"arrowstyle": "-", "color": "white"})
ax.annotate("", (180, 300), (720, 300), arrowprops={"arrowstyle": "<->", "color": "white", "lw": 1.4})
ax.text(35, 38, "Size2x2 = 0.60", color="white", fontsize=12)
ax.annotate("", (180, 300), (155, 64), arrowprops={"arrowstyle": "-", "color": "white"})

ax = panel([.525, .31, .43, .47], "2. Scissor: tune each eye separately")
for x, eye, outside_left in ((20, "LEFT EYE", True), (480, "RIGHT EYE", False)):
    ax.text(x + 200, 560, eye, ha="center", color="white", fontsize=13, weight="bold")
    ax.add_patch(Rectangle((x, 100), 400, 400, facecolor="#244659", edgecolor="#75869c", lw=1.5))
    for y in (100, 380):
        ax.add_patch(Rectangle((x, y), 400, 120, facecolor="#593641", edgecolor="#b67880", hatch="///", lw=0))
        ax.text(x + 200, y + 60, "30% CROPPED", ha="center", va="center", color="#f1cad0", fontsize=10)
    outer_x = x if outside_left else x + 360
    ax.add_patch(Rectangle((outer_x, 220), 40, 160, facecolor="#593641", edgecolor="#b67880", hatch="///", lw=0))
    ax.text(outer_x + 20, 300, "10%", ha="center", va="center", color="#f1cad0", fontsize=9, rotation=90)
    ax.hlines([220, 380], x, x + 400, colors="#91d9e3", linestyles="--", lw=2)
    ax.vlines(x + 40 if outside_left else x + 360, 220, 380, colors="#91d9e3", linestyles="--", lw=2)
    ax.text(x + 200, 325, "Outside-only example", ha="center", color="white", fontsize=10)
    ax.text(x + 200, 278, "Nose-side edge untouched", ha="center", color="#c7dee8", fontsize=9)
    prefix = "LeftEye" if outside_left else "RightEye"
    ax.text(x + 200, 65, prefix + "Left = " + ("0.10 (outer)" if outside_left else "0 (nose)"), ha="center", color="white", fontsize=10)
    ax.text(x + 200, 30, prefix + "Right = " + ("0 (nose)" if outside_left else "0.10 (outer)"), ha="center", color="white", fontsize=10)
ax.text(450, 520, "Top = Bottom = 0.30 · Enabled = true", ha="center", color="#c7dee8", fontsize=11)
ax.text(450, -12, "Defaults: scissor off · all four horizontal edges = 0", ha="center", color="#aabbd0", fontsize=10)

fig.text(.055, .255, "Ellipse diameters scale with eye-image width horizontally and height vertically.", color="white", fontsize=12)
fig.text(.055, .22, "Size2x2 must be at least Size1x1. Larger values preserve more detail.", color="#aabbd0", fontsize=11)
fig.text(.055, .165, "DrawOutline = true", color="white", fontsize=12, weight="bold")
fig.text(.255, .165, "Red: 1×1 boundary   ·   Orange: 2×2 → 4×4 boundary", color="#aabbd0", fontsize=11)
fig.text(.055, .11, "GracePeriodMs = 30", color="white", fontsize=12, weight="bold")
fig.text(.255, .11, "Briefly holds the last gaze when tracking is lost; then centers.", color="#aabbd0", fontsize=11)
fig.text(.055, .063, "GazeRefreshMs = 8", color="white", fontsize=12, weight="bold")
fig.text(.255, .063, "CPU gaze polling interval. This is not eye-to-display latency.", color="#aabbd0", fontsize=11)
fig.text(.955, .016, "Illustration · Shape=ellipse · not a game screenshot", color="#7f91a8", fontsize=9, ha="right")
fig.savefig(OUT / "vrs-settings.png", dpi=160, facecolor=fig.get_facecolor())
fig.savefig(OUT / "vrs-settings.svg", facecolor=fig.get_facecolor())
svg = OUT / "vrs-settings.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
plt.close(fig)
print("Generated docs/images/vrs-settings.png and .svg")
