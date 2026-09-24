#include "EpubIndex.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace reflow {

namespace {

constexpr uint32_t nullQString = 0xFFFFFFFF;
const char16_t indexMagic[] = u"rM epub index";

class BigEndianCursor {
public:
	explicit BigEndianCursor(const std::vector<uint8_t> &bytes) : bytes(bytes) {}

	size_t remaining() const { return bytes.size() - position; }

	uint32_t u32()
	{
		require(4);
		const uint32_t value = uint32_t(bytes[position]) << 24 | uint32_t(bytes[position + 1]) << 16
			| uint32_t(bytes[position + 2]) << 8 | uint32_t(bytes[position + 3]);
		position += 4;
		return value;
	}

	void skip(size_t count)
	{
		require(count);
		position += count;
	}

	std::u16string qstring()
	{
		const uint32_t byteLength = u32();
		if (byteLength == nullQString)
			return {};
		if (byteLength % 2)
			throw std::runtime_error("odd QString length");
		require(byteLength);
		std::u16string text;
		for (uint32_t i = 0; i < byteLength; i += 2)
			text.push_back(char16_t(bytes[position + i] << 8 | bytes[position + i + 1]));
		position += byteLength;
		return text;
	}

private:
	void require(size_t count) const
	{
		if (count > remaining())
			throw std::runtime_error("index truncated");
	}

	const std::vector<uint8_t> &bytes;
	size_t position = 0;
};

std::string utf8FromUtf16(const std::u16string &text)
{
	std::string out;
	for (char16_t ch : text) {
		if (ch < 0x80) {
			out.push_back(char(ch));
		} else if (ch < 0x800) {
			out.push_back(char(0xC0 | ch >> 6));
			out.push_back(char(0x80 | (ch & 0x3F)));
		} else {
			out.push_back(char(0xE0 | ch >> 12));
			out.push_back(char(0x80 | (ch >> 6 & 0x3F)));
			out.push_back(char(0x80 | (ch & 0x3F)));
		}
	}
	return out;
}

}

const SpineEntry *EpubIndex::entryForPage(uint32_t page) const
{
	for (const SpineEntry &entry : spine) {
		if (entry.containsPage(page))
			return &entry;
	}
	return nullptr;
}

const SpineEntry *EpubIndex::entryForHref(const std::string &href) const
{
	for (const SpineEntry &entry : spine) {
		if (entry.href == href)
			return &entry;
	}
	return nullptr;
}

EpubIndex readEpubIndex(const std::string &path)
{
	EpubIndex index;
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		index.error = "cannot open " + path;
		return index;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	try {
		BigEndianCursor cursor(bytes);
		if (cursor.qstring() != indexMagic)
			throw std::runtime_error("not an epub index");
		const uint32_t version = cursor.u32();
		if (version >= 2)
			cursor.skip(16);
		cursor.skip(3 * 8 + 4);
		cursor.qstring();

		const uint32_t tocCount = cursor.u32();
		for (uint32_t i = 0; i < tocCount; ++i) {
			cursor.qstring();
			cursor.qstring();
			cursor.u32();
			cursor.u32();
		}

		const uint32_t spineCount = cursor.u32();
		for (uint32_t i = 0; i < spineCount; ++i) {
			SpineEntry entry;
			entry.href = utf8FromUtf16(cursor.qstring());
			cursor.u32();
			cursor.u32();
			entry.pageStart = cursor.u32();
			entry.pageCount = cursor.u32();
			index.spine.push_back(entry);
		}
	} catch (const std::exception &error) {
		index.spine.clear();
		index.error = error.what();
	}
	return index;
}

}
