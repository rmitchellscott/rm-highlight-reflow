#pragma once

#include "EpubIndex.hpp"
#include "HighlightReader.hpp"
#include "PdfText.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace reflow {

struct PageHighlights {
	uint32_t pageIndex = 0;
	std::vector<Highlight> highlights;
};

struct PageRect {
	uint32_t pageIndex = 0;
	SceneRect rect;
};

struct Anchor {
	std::string href;
	size_t first = 0;
	size_t end = 0;
	std::u32string visibleText;
	uint32_t color = 0;
	uint32_t argb = 0;
	std::vector<std::string> pieces;
	std::vector<PageRect> displaced;
};

struct Stroke {
	double fromX = 0;
	double toX = 0;
	double y = 0;
	uint32_t color = 0;
	uint32_t argb = 0;
	int firstIndex = 0;
	int lastIndex = 0;
	SceneRect drawnArea;
	uint32_t highlight = 0;
};

struct Placement {
	std::string text;
	int shift = 0;
	bool exact = false;
};

struct Plan {
	std::map<uint32_t, std::vector<SceneRect>> erase;
	std::map<uint32_t, std::vector<uint32_t>> eraseHighlights;
	std::map<uint32_t, std::vector<SceneRect>> replace;
	std::map<uint32_t, std::vector<Stroke>> create;
	std::vector<Placement> placed;
	std::vector<std::string> unresolved;
	size_t notInOldLayout = 0;
};

std::vector<SpineEntry> rebuildSpine(const EpubIndex &referenceIndex, PdfText &referenceText, PdfText &targetText,
	uint32_t lastNeededPage);

std::vector<Anchor> captureAnchors(PdfText &oldText, const std::vector<SpineEntry> &oldSpine,
	const std::vector<PageHighlights> &pages, size_t *notInOldLayout = nullptr);

Plan planReflow(PdfText &newText, const EpubIndex &newIndex, const std::vector<Anchor> &anchors,
	const std::vector<PageHighlights> &onNewPages = {});

std::u32string utf32FromUtf8(const std::string &text);

}
