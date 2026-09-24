#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace reflow {

struct CrdtId {
	uint8_t author = 0;
	uint64_t counter = 0;

	bool operator==(const CrdtId &other) const { return author == other.author && counter == other.counter; }
	bool operator<(const CrdtId &other) const { return author != other.author ? author < other.author : counter < other.counter; }
};

struct SceneRect {
	double x = 0;
	double y = 0;
	double width = 0;
	double height = 0;
};

struct Highlight {
	CrdtId id;
	std::optional<uint32_t> start;
	std::optional<uint32_t> length;
	uint32_t color = 0;
	std::optional<uint32_t> argb;
	std::string text;
	std::vector<SceneRect> rects;
};

struct HighlightReadResult {
	std::vector<Highlight> highlights;
	std::string error;

	bool ok() const { return error.empty(); }
};

HighlightReadResult readHighlights(const std::string &rmPath);
HighlightReadResult readHighlightsFromBytes(const std::vector<uint8_t> &bytes);

}
