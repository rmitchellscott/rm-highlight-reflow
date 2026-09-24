# highlight-reflow

[![rm1](https://img.shields.io/badge/rM1-supported-green)](https://remarkable.com/store/remarkable)
[![rm2](https://img.shields.io/badge/rM2-supported-green)](https://remarkable.com/store/remarkable-2)
[![rmpp](https://img.shields.io/badge/rMPP-supported-green)](https://remarkable.com/store/overview/remarkable-paper-pro)
[![rmppmove](https://img.shields.io/badge/rMPPMove-supported-green)](https://remarkable.com/products/remarkable-paper/pro-move)
[![rmppure](https://img.shields.io/badge/rMPPure-supported-green)](https://remarkable.com/products/remarkable-paper/pure)

A xovi extension that keeps EPUB highlights on their text when the book reflows.

Changing the margins, text size, font or line spacing of an EPUB makes xochitl render the book to a new PDF. Highlights are stored by page and position, so after the render they sit on whatever text now occupies that spot, or on a page that no longer exists.

This extension moves each highlight back onto its text. It keeps the old layout while xochitl renders the new one, anchors every highlight to its place in the book's text, and after the render erases the displaced highlights and draws them again on the pages where their text now is. Colors are kept, and a highlight that now crosses a page break is split across both pages.

The edits go through xochitl's own highlighter, so the new highlights are ordinary highlights: they sync, and they can be erased like any other. The extension writes nothing into the document folder and shows no UI. While xochitl finishes saving the re-anchored pages, a page turn may result in a loading indicator.

## Dependencies

- [xovi](https://github.com/asivery/rm-xovi-extensions) - Extension framework
    - qt-resource-rebuilder - Required to apply the QML patch

## Installation

### Vellum

```
vellum add highlight-reflow
```

### Manual

1. Ensure dependencies are installed
2. Download the `.so` file for your architecture from the [latest release](https://github.com/rmitchellscott/rm-highlight-reflow/releases/latest) and place it in `/home/root/xovi/extensions.d/` on your reMarkable tablet
    - **reMarkable 1 & 2**: `highlight-reflow-armv7.so`
    - **reMarkable Paper Pro, Paper Pro Move and Paper Pure**: `highlight-reflow-aarch64.so`
3. Restart xovi

## Usage

Open an EPUB and change a setting under the text format menu. When the render finishes, the highlights move onto their text.

If the book is closed or xochitl restarts before that happens, the extension finishes the move the next time the book is opened.

## Limitations

- Only EPUBs are handled. Highlights in PDFs and notebooks do not move, because their pages do not change.
- Pen strokes on EPUB pages are not moved.
- A highlight whose text cannot be found in the new layout is left where it was. The log line reports how many.

## How it works

1. When a render starts, xochitl renames the book's PDF to `<uuid>.pdf.backup`. The extension hard-links it and copies the `.epubindex` and `.content` into `/home/root/.cache/highlight-reflow/<uuid>/`, then reads every highlight in a background process.
2. Each highlight is anchored to its chapter plus the number of visible non-whitespace characters before it. That number does not change with the layout. A font change can move it by a character or two, and a search for the highlight's text around the anchor corrects that. Some fonts (EB Garamond, reMarkable Serif) write ligatures such as "fi" into the PDF as an unreadable character. pdfium flags those, and the search lets each one stand for the letters it replaced.
3. When the new table of contents arrives, the extension finds each anchor in the new PDF and plans one erase per displaced highlight and one highlighter stroke per line of text.
4. It opens hidden page views, erases the old highlights with an EraseSection lasso, and draws the new ones through `SceneController.highlightWithLine`. Highlights already on a target page in the same color are erased first, so a second pass does not stack duplicates. It waits on the document worker before closing each view, so no edit is lost.

## License

Copyright (C) 2026 Mitchell Scott

Licensed under the GNU General Public License v3.0.
