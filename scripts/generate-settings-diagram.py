"""Regenerate the documentation figure (requires matplotlib, not needed by users)."""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle

OUT = Path(__file__).resolve().parents[1] / "docs" / "images"
OUT.mkdir(parents=True, exist_ok=True)
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11})
fig = plt.figure(figsize=(13, 7.4), facecolor="#0c1321")
fig.text(.055, .925, "BeamNG-VRS settings", color="white", fontsize=26, weight="bold")
fig.text(.055, .876, "Edit BeamNG-VRS.ini, then restart the game", color="#aabbd0", fontsize=13)

def panel(bounds, title):
    ax = fig.add_axes(bounds)
    ax.set(xlim=(0, 900), ylim=(0, 600), aspect="equal")
    ax.axis("off")
    ax.set_title(title, color="white", fontsize=15, loc="left", pad=14)
    return ax

ax = panel([.055, .31, .43, .47], "1. Foveation regions")
ax.add_patch(Rectangle((0, 0), 900, 600, facecolor="#283d53", edgecolor="#75869c", lw=1.5))
ax.add_patch(Circle((450, 300), 180, facecolor="#a97835", edgecolor="#ffae42", lw=3))
ax.add_patch(Circle((450, 300), 75, facecolor="#74cadd", edgecolor="#ff555c", lw=3))
ax.text(450, 330, "1×1", ha="center", va="center", color="#10202b", fontsize=18, weight="bold")
ax.text(450, 440, "2×2", ha="center", color="white", fontsize=17, weight="bold")
ax.text(755, 525, "4×4", ha="center", color="white", fontsize=17, weight="bold")
ax.annotate("", (375, 300), (525, 300), arrowprops={"arrowstyle": "<->", "color": "white", "lw": 1.6})
ax.annotate("Size1x1 = 0.25", (450, 230), (470, 90), color="white", fontsize=12,
            ha="center", arrowprops={"arrowstyle": "-", "color": "white"})
ax.annotate("", (270, 300), (630, 300), arrowprops={"arrowstyle": "<->", "color": "white", "lw": 1.4})
ax.text(35, 38, "Size2x2 = 0.60", color="white", fontsize=12)
ax.annotate("", (270, 300), (155, 64), arrowprops={"arrowstyle": "-", "color": "white"})

ax = panel([.525, .31, .43, .47], "2. Optional scissor crop")
ax.add_patch(Rectangle((0, 0), 900, 600, facecolor="#244659", edgecolor="#75869c", lw=1.5))
for y in (0, 480):
    ax.add_patch(Rectangle((0, y), 900, 120, facecolor="#593641", edgecolor="#b67880", hatch="///", lw=0))
    ax.text(450, y + 60, "CROPPED", ha="center", va="center", color="#f1cad0", fontsize=14)
ax.hlines([120, 480], 0, 900, colors="#91d9e3", linestyles="--", lw=2)
ax.text(450, 310, "Enabled = true", ha="center", color="white", fontsize=16, weight="bold")
ax.text(450, 265, "Top = 0.20  ·  Bottom = 0.20", ha="center", color="white", fontsize=12)
for y0, y1, label in ((480, 600, "20%"), (0, 120, "20%")):
    ax.annotate("", (850, y0), (850, y1), arrowprops={"arrowstyle": "<->", "color": "white", "lw": 1.7})
    ax.text(800, (y0 + y1) / 2, label, color="white", ha="right", va="center")
ax.text(25, 145, "Disabled by default", color="#c7dee8", fontsize=11)

fig.text(.055, .255, "Size1x1 and Size2x2 are diameters relative to eye-image height.", color="white", fontsize=12)
fig.text(.055, .22, "Size2x2 must be at least Size1x1. Larger values preserve more detail.", color="#aabbd0", fontsize=11)
fig.text(.055, .165, "DrawOutline = true", color="white", fontsize=12, weight="bold")
fig.text(.255, .165, "Red: 1×1 boundary   ·   Orange: 2×2 → 4×4 boundary", color="#aabbd0", fontsize=11)
fig.text(.055, .11, "GracePeriodMs = 30", color="white", fontsize=12, weight="bold")
fig.text(.255, .11, "Briefly holds the last gaze when tracking is lost; then centers.", color="#aabbd0", fontsize=11)
fig.text(.055, .063, "GazeRefreshMs = 8", color="white", fontsize=12, weight="bold")
fig.text(.255, .063, "CPU gaze polling interval. This is not eye-to-display latency.", color="#aabbd0", fontsize=11)
fig.text(.955, .016, "Illustration · Shape=circle · not a game screenshot", color="#7f91a8", fontsize=9, ha="right")
fig.savefig(OUT / "vrs-settings.png", dpi=160, facecolor=fig.get_facecolor())
fig.savefig(OUT / "vrs-settings.svg", facecolor=fig.get_facecolor())
svg = OUT / "vrs-settings.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
plt.close(fig)
print("Generated docs/images/vrs-settings.png and .svg")
