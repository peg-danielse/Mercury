"""IEEE Computer Society Transactions figure type, including Transactions on Services Computing.

Figure text is 8 pt Computer Modern Roman, via the vendored latexify (usetex).
Draw at the printed size: 3.39 in for one column (`single`), 6.90 in for two
(`double`). Height follows the content and stays at most 8.0 in. Do not draw
larger and scale the page down in LaTeX. A line that carries data is at least 1 pt.
"""

from __future__ import annotations

import os

import matplotlib.pyplot as plt

COLUMN = 3.39  # single-column width, inches
FULL = 6.90    # double-column width, inches
MAX_HEIGHT = 8.0
_STYLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "paper.mplstyle")


def snap(width: float) -> float:
    """The IEEE column this width belongs to. Values past the midpoint are the full measure."""
    return COLUMN if width < (COLUMN + FULL) / 2 else FULL


def width_of(spec) -> float:
    """Publication width. `single` is 3.39 in, `double` is 6.90 in. A number snaps to the nearer."""
    if isinstance(spec, str):
        key = spec.strip().lower()
        if key in ("single", "column", "1"):
            return COLUMN
        if key in ("double", "full", "2"):
            return FULL
        spec = float(key)
    return snap(float(spec))


def apply() -> str:
    """Load latexify, then pin 8 pt and the column-safe save. Returns the face."""
    from latexify import latexify
    plt.style.use(_STYLE)
    latexify(fig_width=COLUMN, columns=1)
    # Latexify's own save uses a tight bbox, which changes the PDF page and makes
    # LaTeX rescale the type. The canvas stays the size the figure was drawn at.
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Computer Modern Roman"],
        "text.usetex": True,
        "font.size": 8,
        "axes.labelsize": 8,
        "axes.titlesize": 8,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "legend.fontsize": 8,
        "lines.linewidth": 1.0,
        "savefig.dpi": 300,
        "savefig.bbox": "standard",
        "savefig.pad_inches": 0.02,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })
    return "Computer Modern Roman (latexify)"


def dress(fig) -> None:
    """latexify.format_axes on every axes: no top or right spine, light ticks."""
    from latexify import format_axes
    for ax in fig.axes:
        format_axes(ax)
