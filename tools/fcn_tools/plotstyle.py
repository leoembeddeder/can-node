"""One look for every plot in results/.

Colours carry identity only: series take the categorical slots below in this fixed order and
are never cycled; text, axes and grid stay in ink and neutral tones. Series are also labelled
in a legend, so no plot depends on colour alone. Plots are drawn on a light surface with an
opaque background so they read the same on a light or dark page.
"""
from __future__ import annotations

import matplotlib

matplotlib.use("Agg")  # no display needed: plots are written to files
import matplotlib.pyplot as plt  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"

# Categorical series colours, assigned in this order.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]

# One-hue ramp for ordered values (light to dark).
SEQUENTIAL = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b"]

# Reserved for state, never used for a series.
STATUS = {"good": "#0ca30c", "warning": "#fab219", "serious": "#ec835a", "critical": "#d03b3b"}


def apply() -> None:
    """Set the matplotlib defaults shared by all plots."""
    plt.rcParams.update({
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "font.family": "sans-serif",
        "font.size": 9,
        "text.color": INK,
        "axes.titlesize": 10,
        "axes.titleweight": "bold",
        "axes.titlelocation": "left",
        "axes.titlepad": 10,
        "axes.labelcolor": INK_SECONDARY,
        "axes.labelsize": 9,
        "axes.edgecolor": AXIS,
        "axes.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.spines.left": False,
        "axes.grid": True,
        "axes.grid.axis": "y",
        "axes.axisbelow": True,
        "grid.color": GRID,
        "grid.linewidth": 0.6,
        "xtick.color": INK_MUTED,
        "ytick.color": INK_MUTED,
        "xtick.labelcolor": INK_SECONDARY,
        "ytick.labelcolor": INK_SECONDARY,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "ytick.major.size": 0,
        "lines.linewidth": 2.0,
        "lines.solid_capstyle": "round",
        "legend.frameon": False,
        "legend.fontsize": 8,
        "legend.labelcolor": INK_SECONDARY,
    })


def reference_line(ax, x: float, label: str) -> None:
    """A vertical reference (a nominal value, a limit) in neutral ink, labelled at its foot."""
    ax.axvline(x, color=INK_MUTED, linewidth=0.8, linestyle=(0, (4, 3)), zorder=1)
    ax.annotate(label, xy=(x, 0.0), xycoords=("data", "axes fraction"), xytext=(-3, 4),
                textcoords="offset points", ha="right", va="bottom", fontsize=7.5,
                color=INK_MUTED, rotation=90)


def legend_above(ax) -> None:
    """Stack the legend between the title and the plot, where it cannot cover data."""
    handles, labels = ax.get_legend_handles_labels()
    rows = max(len(labels), 1)
    ax.set_title(ax.get_title(loc="left"), loc="left", pad=10 + 13 * rows)
    ax.legend(handles, labels, loc="lower left", bbox_to_anchor=(-0.01, 1.0), ncol=1,
              borderaxespad=0.2, handlelength=1.6, labelspacing=0.3)


def footnote(fig, text: str) -> None:
    """Small print under a figure: where the data came from."""
    fig.text(0.01, 0.01, text, ha="left", va="bottom", fontsize=7, color=INK_MUTED)


def save(fig, path) -> None:
    fig.savefig(path, dpi=200)
    plt.close(fig)
