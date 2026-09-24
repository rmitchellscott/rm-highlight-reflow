#include "HighlightReader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace reflow {

namespace {

const char fileHeader[] = "reMarkable .lines file, version=6          ";
constexpr size_t fileHeaderLength = sizeof(fileHeader) - 1;

constexpr uint8_t glyphItemBlock = 0x03;
constexpr uint8_t glyphItemType = 0x01;

enum TagType : uint8_t {
	Byte1 = 0x1,
	Byte4 = 0x4,
	Byte8 = 0x8,
	Length4 = 0xC,
	Id = 0xF,
};

struct ParseError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class Cursor {
public:
	Cursor(const std::vector<uint8_t> &bytes, size_t begin, size_t end) : bytes(bytes), position(begin), end(end) {}

	size_t offset() const { return position; }
	size_t remaining() const { return end - position; }
	void seek(size_t offset) { position = offset; }

	uint8_t u8()
	{
		require(1);
		return bytes[position++];
	}

	uint32_t u32()
	{
		require(4);
		uint32_t value;
		std::memcpy(&value, &bytes[position], 4);
		position += 4;
		return value;
	}

	double f64()
	{
		require(8);
		double value;
		std::memcpy(&value, &bytes[position], 8);
		position += 8;
		return value;
	}

	uint64_t varuint()
	{
		uint64_t value = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			const uint8_t byte = u8();
			value |= uint64_t(byte & 0x7F) << shift;
			if (!(byte & 0x80))
				return value;
		}
		throw ParseError("varuint too long");
	}

	std::string bytesAsString(size_t length)
	{
		require(length);
		std::string text(reinterpret_cast<const char *>(&bytes[position]), length);
		position += length;
		return text;
	}

	bool nextTagIs(unsigned index, TagType type)
	{
		if (remaining() == 0)
			return false;
		const size_t saved = position;
		try {
			const uint64_t tag = varuint();
			position = saved;
			return (tag >> 4) == index && (tag & 0xF) == type;
		} catch (const ParseError &) {
			position = saved;
			return false;
		}
	}

	void expectTag(unsigned index, TagType type)
	{
		const uint64_t tag = varuint();
		if ((tag >> 4) != index || (tag & 0xF) != type)
			throw ParseError("unexpected tag " + std::to_string(tag) + ", wanted index " + std::to_string(index));
	}

	CrdtId taggedId(unsigned index)
	{
		expectTag(index, Id);
		CrdtId id;
		id.author = u8();
		id.counter = varuint();
		return id;
	}

	uint32_t taggedU32(unsigned index)
	{
		expectTag(index, Byte4);
		return u32();
	}

	std::optional<CrdtId> optionalTaggedId(unsigned index)
	{
		if (!nextTagIs(index, Id))
			return std::nullopt;
		return taggedId(index);
	}

	std::optional<bool> optionalTaggedBool(unsigned index)
	{
		if (!nextTagIs(index, Byte1))
			return std::nullopt;
		expectTag(index, Byte1);
		return u8() != 0;
	}

	std::optional<uint32_t> optionalTaggedU32(unsigned index)
	{
		if (!nextTagIs(index, Byte4))
			return std::nullopt;
		return taggedU32(index);
	}

	Cursor subBlock(unsigned index)
	{
		expectTag(index, Length4);
		const uint32_t length = u32();
		require(length);
		Cursor inner(bytes, position, position + length);
		position += length;
		return inner;
	}

private:
	void require(size_t count) const
	{
		if (count > end - position)
			throw ParseError("read past end of block");
	}

	const std::vector<uint8_t> &bytes;
	size_t position;
	size_t end;
};

std::string taggedString(Cursor &cursor, unsigned index)
{
	Cursor block = cursor.subBlock(index);
	const uint64_t length = block.varuint();
	if (block.u8() != 1)
		throw ParseError("string is not flagged as UTF-8");
	return block.bytesAsString(length);
}

Highlight readGlyphRange(Cursor &value, const CrdtId &id)
{
	Highlight highlight;
	highlight.id = id;
	highlight.start = value.optionalTaggedU32(2);
	highlight.length = value.optionalTaggedU32(3);
	highlight.color = value.taggedU32(4);
	highlight.text = taggedString(value, 5);

	Cursor rects = value.subBlock(6);
	const uint64_t count = rects.varuint();
	for (uint64_t i = 0; i < count; ++i) {
		SceneRect rect;
		rect.x = rects.f64();
		rect.y = rects.f64();
		rect.width = rects.f64();
		rect.height = rects.f64();
		highlight.rects.push_back(rect);
	}

	value.optionalTaggedId(7);
	value.optionalTaggedId(8);
	value.optionalTaggedBool(9);
	highlight.argb = value.optionalTaggedU32(10);
	return highlight;
}

}

HighlightReadResult readHighlightsFromBytes(const std::vector<uint8_t> &bytes)
{
	HighlightReadResult result;
	if (bytes.size() < fileHeaderLength || std::memcmp(bytes.data(), fileHeader, fileHeaderLength) != 0) {
		result.error = "not a v6 .rm file";
		return result;
	}

	std::map<CrdtId, std::optional<Highlight>> latestById;
	std::vector<CrdtId> order;
	try {
		Cursor file(bytes, fileHeaderLength, bytes.size());
		while (file.remaining() > 0) {
			const uint32_t length = file.u32();
			file.u8();
			file.u8();
			file.u8();
			const uint8_t type = file.u8();
			if (length > file.remaining())
				throw ParseError("block longer than file");
			const size_t blockEnd = file.offset() + length;

			if (type == glyphItemBlock) {
				Cursor block(bytes, file.offset(), blockEnd);
				block.taggedId(1);
				const CrdtId itemId = block.taggedId(2);
				block.taggedId(3);
				block.taggedId(4);
				block.taggedU32(5);

				std::optional<Highlight> value;
				if (block.nextTagIs(6, Length4)) {
					Cursor item = block.subBlock(6);
					if (item.u8() != glyphItemType)
						throw ParseError("glyph block with a non-glyph item");
					value = readGlyphRange(item, itemId);
				}
				if (!latestById.count(itemId))
					order.push_back(itemId);
				latestById[itemId] = value;
			}
			file.seek(blockEnd);
		}
	} catch (const ParseError &error) {
		result.error = error.what();
		return result;
	}

	for (const CrdtId &id : order) {
		if (latestById[id])
			result.highlights.push_back(*latestById[id]);
	}
	return result;
}

HighlightReadResult readHighlights(const std::string &rmPath)
{
	std::ifstream file(rmPath, std::ios::binary);
	if (!file) {
		HighlightReadResult result;
		result.error = "cannot open " + rmPath;
		return result;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return readHighlightsFromBytes(bytes);
}

}
