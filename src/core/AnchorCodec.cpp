#include "AnchorCodec.hpp"

#include <cstring>

namespace reflow {

namespace {

class Writer {
public:
	void u32(uint32_t value) { raw(&value, sizeof(value)); }
	void u64(uint64_t value) { raw(&value, sizeof(value)); }
	void f64(double value) { raw(&value, sizeof(value)); }

	void string(const std::string &text)
	{
		u64(text.size());
		out.append(text);
	}

	void u32string(const std::u32string &text)
	{
		u64(text.size());
		raw(text.data(), text.size() * sizeof(char32_t));
	}

	void rect(const SceneRect &rect)
	{
		f64(rect.x);
		f64(rect.y);
		f64(rect.width);
		f64(rect.height);
	}

	std::string out;

private:
	void raw(const void *data, size_t size) { out.append(static_cast<const char *>(data), size); }
};

class Reader {
public:
	explicit Reader(const std::string &in) : in(in) {}

	bool u32(uint32_t &value) { return raw(&value, sizeof(value)); }
	bool u64(uint64_t &value) { return raw(&value, sizeof(value)); }
	bool f64(double &value) { return raw(&value, sizeof(value)); }

	bool string(std::string &text)
	{
		uint64_t size;
		if (!u64(size) || size > in.size() - position)
			return false;
		text = in.substr(position, size);
		position += size;
		return true;
	}

	bool u32string(std::u32string &text)
	{
		uint64_t size;
		if (!u64(size) || size > (in.size() - position) / sizeof(char32_t))
			return false;
		text.resize(size);
		return raw(text.data(), size * sizeof(char32_t));
	}

	bool rect(SceneRect &rect) { return f64(rect.x) && f64(rect.y) && f64(rect.width) && f64(rect.height); }

	bool atEnd() const { return position == in.size(); }

private:
	bool raw(void *data, size_t size)
	{
		if (size > in.size() - position)
			return false;
		std::memcpy(data, in.data() + position, size);
		position += size;
		return true;
	}

	const std::string &in;
	size_t position = 0;
};

}

std::string encodeAnchors(const std::vector<Anchor> &anchors)
{
	Writer writer;
	writer.u64(anchors.size());
	for (const Anchor &anchor : anchors) {
		writer.string(anchor.href);
		writer.u64(anchor.first);
		writer.u64(anchor.end);
		writer.u32string(anchor.visibleText);
		writer.u32(anchor.color);
		writer.u32(anchor.argb);
		writer.u64(anchor.pieces.size());
		for (const std::string &piece : anchor.pieces)
			writer.string(piece);
		writer.u64(anchor.displaced.size());
		for (const PageRect &displaced : anchor.displaced) {
			writer.u32(displaced.pageIndex);
			writer.rect(displaced.rect);
		}
	}
	return writer.out;
}

bool decodeAnchors(const std::string &encoded, std::vector<Anchor> &anchors)
{
	Reader reader(encoded);
	uint64_t count;
	if (!reader.u64(count))
		return false;
	anchors.clear();
	for (uint64_t i = 0; i < count; ++i) {
		Anchor anchor;
		uint64_t first, end, pieces, displaced;
		if (!reader.string(anchor.href) || !reader.u64(first) || !reader.u64(end) || !reader.u32string(anchor.visibleText)
			|| !reader.u32(anchor.color) || !reader.u32(anchor.argb) || !reader.u64(pieces))
			return false;
		anchor.first = first;
		anchor.end = end;
		for (uint64_t p = 0; p < pieces; ++p) {
			std::string piece;
			if (!reader.string(piece))
				return false;
			anchor.pieces.push_back(piece);
		}
		if (!reader.u64(displaced))
			return false;
		for (uint64_t d = 0; d < displaced; ++d) {
			PageRect rect;
			if (!reader.u32(rect.pageIndex) || !reader.rect(rect.rect))
				return false;
			anchor.displaced.push_back(rect);
		}
		anchors.push_back(std::move(anchor));
	}
	return reader.atEnd();
}

void encodePageRects(Writer &writer, const std::map<uint32_t, std::vector<SceneRect>> &pages)
{
	writer.u64(pages.size());
	for (const auto &[page, rects] : pages) {
		writer.u32(page);
		writer.u64(rects.size());
		for (const SceneRect &rect : rects)
			writer.rect(rect);
	}
}

bool decodePageRects(Reader &reader, std::map<uint32_t, std::vector<SceneRect>> &pages)
{
	uint64_t count;
	if (!reader.u64(count))
		return false;
	for (uint64_t i = 0; i < count; ++i) {
		uint32_t page;
		uint64_t rects;
		if (!reader.u32(page) || !reader.u64(rects))
			return false;
		for (uint64_t r = 0; r < rects; ++r) {
			SceneRect rect;
			if (!reader.rect(rect))
				return false;
			pages[page].push_back(rect);
		}
	}
	return true;
}

std::string encodePlan(const Plan &plan)
{
	Writer writer;
	encodePageRects(writer, plan.erase);
	writer.u64(plan.eraseHighlights.size());
	for (const auto &[page, highlights] : plan.eraseHighlights) {
		writer.u32(page);
		writer.u64(highlights.size());
		for (uint32_t highlight : highlights)
			writer.u32(highlight);
	}
	encodePageRects(writer, plan.replace);
	writer.u64(plan.create.size());
	for (const auto &[page, strokes] : plan.create) {
		writer.u32(page);
		writer.u64(strokes.size());
		for (const Stroke &stroke : strokes) {
			writer.f64(stroke.fromX);
			writer.f64(stroke.toX);
			writer.f64(stroke.y);
			writer.u32(stroke.color);
			writer.u32(stroke.argb);
			writer.rect(stroke.drawnArea);
			writer.u32(stroke.highlight);
		}
	}
	writer.u64(plan.placed.size());
	for (const Placement &placed : plan.placed) {
		writer.string(placed.text);
		writer.u32(uint32_t(placed.shift));
		writer.u32(placed.exact ? 1 : 0);
	}
	writer.u64(plan.unresolved.size());
	for (const std::string &text : plan.unresolved)
		writer.string(text);
	writer.u64(plan.notInOldLayout);
	return writer.out;
}

bool decodePlan(const std::string &encoded, Plan &plan)
{
	Reader reader(encoded);
	plan = Plan();
	uint64_t count;
	if (!decodePageRects(reader, plan.erase) || !reader.u64(count))
		return false;
	for (uint64_t i = 0; i < count; ++i) {
		uint32_t page;
		uint64_t highlights;
		if (!reader.u32(page) || !reader.u64(highlights))
			return false;
		for (uint64_t h = 0; h < highlights; ++h) {
			uint32_t highlight;
			if (!reader.u32(highlight))
				return false;
			plan.eraseHighlights[page].push_back(highlight);
		}
	}
	if (!decodePageRects(reader, plan.replace))
		return false;
	if (!reader.u64(count))
		return false;
	for (uint64_t i = 0; i < count; ++i) {
		uint32_t page;
		uint64_t strokes;
		if (!reader.u32(page) || !reader.u64(strokes))
			return false;
		for (uint64_t s = 0; s < strokes; ++s) {
			Stroke stroke;
			if (!reader.f64(stroke.fromX) || !reader.f64(stroke.toX) || !reader.f64(stroke.y) || !reader.u32(stroke.color)
				|| !reader.u32(stroke.argb) || !reader.rect(stroke.drawnArea)
					|| !reader.u32(stroke.highlight))
				return false;
			plan.create[page].push_back(stroke);
		}
	}
	if (!reader.u64(count))
		return false;
	for (uint64_t i = 0; i < count; ++i) {
		Placement placed;
		uint32_t shift, exact;
		if (!reader.string(placed.text) || !reader.u32(shift) || !reader.u32(exact))
			return false;
		placed.shift = int(int32_t(shift));
		placed.exact = exact != 0;
		plan.placed.push_back(placed);
	}
	if (!reader.u64(count))
		return false;
	for (uint64_t i = 0; i < count; ++i) {
		std::string text;
		if (!reader.string(text))
			return false;
		plan.unresolved.push_back(text);
	}
	uint64_t notInOldLayout;
	if (!reader.u64(notInOldLayout))
		return false;
	plan.notInOldLayout = notInOldLayout;
	return reader.atEnd();
}

}
