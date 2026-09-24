#include "Anchoring.hpp"

#include <algorithm>
#include <cmath>
#include <clocale>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <set>
#include <utility>

namespace reflow {

namespace {

constexpr double sceneUnitsPerPoint = 227.0 / 72.0;
constexpr double erasePadding = 4;
constexpr size_t searchWindow = 150;
constexpr uint32_t argbColor = 9;
constexpr uint32_t opaqueBlack = 0xFF000000;
constexpr double drawnAreaMarginInLinePitches = 0.25;
constexpr size_t commonLigatureLength = 2;
constexpr size_t longestLigatureLength = 3;

struct VisibleString {
	std::u32string characters;
	std::vector<std::pair<int, int>> positions;
};

VisibleString visibleString(PdfText &text, const SpineEntry &entry)
{
	VisibleString visible;
	for (uint32_t page = entry.pageStart; page < entry.pageStart + entry.pageCount; ++page) {
		const PageText &pageText = text.page(int(page));
		for (int index : pageText.visibleIndices) {
			visible.characters.push_back(normalizedHyphen(pageText.characters[index]));
			visible.positions.emplace_back(int(page), index);
		}
	}
	return visible;
}

size_t visibleCharactersBefore(PdfText &text, const SpineEntry &entry, uint32_t page, uint32_t pdfiumIndex)
{
	size_t count = 0;
	for (uint32_t earlier = entry.pageStart; earlier < page; ++earlier)
		count += text.page(int(earlier)).visibleIndices.size();
	for (int index : text.page(int(page)).visibleIndices) {
		if (uint32_t(index) < pdfiumIndex)
			++count;
	}
	return count;
}

SceneRect coveringRect(const std::vector<SceneRect> &rects)
{
	double left = rects.front().x, top = rects.front().y;
	double right = left + rects.front().width, bottom = top + rects.front().height;
	for (const SceneRect &rect : rects) {
		left = std::min(left, rect.x);
		top = std::min(top, rect.y);
		right = std::max(right, rect.x + rect.width);
		bottom = std::max(bottom, rect.y + rect.height);
	}
	return {left - erasePadding, top - erasePadding, right - left + 2 * erasePadding, bottom - top + 2 * erasePadding};
}

std::u32string withoutSpaces(const std::u32string &text)
{
	std::u32string out;
	for (char32_t ch : text) {
		if (!isSpace(ch))
			out.push_back(normalizedHyphen(ch));
	}
	return out;
}

bool matchesStoredText(const std::u32string &pdfText, const std::u32string &storedText)
{
	if (pdfText.size() != storedText.size())
		return false;
	for (size_t i = 0; i < pdfText.size(); ++i) {
		if (pdfText[i] != storedText[i] && pdfText[i] != unmappedLigature)
			return false;
	}
	return true;
}

size_t visibleWeight(char32_t ch)
{
	return ch == unmappedLigature ? commonLigatureLength : 1;
}

size_t weightedPosition(const std::u32string &characters, size_t index)
{
	size_t position = 0;
	for (size_t i = 0; i < index && i < characters.size(); ++i)
		position += visibleWeight(characters[i]);
	return position;
}

size_t indexAtWeightedPosition(const std::u32string &characters, size_t position)
{
	size_t reached = 0;
	for (size_t i = 0; i < characters.size(); ++i) {
		if (reached >= position)
			return i;
		reached += visibleWeight(characters[i]);
	}
	return characters.size();
}

char32_t foldedCase(char32_t ch)
{
	static const locale_t unicodeLocale = [] {
		locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", locale_t(0));
		return locale ? locale : newlocale(LC_CTYPE_MASK, "en_US.UTF-8", locale_t(0));
	}();
	if (ch < 0x80)
		return ch >= U'A' && ch <= U'Z' ? ch + (U'a' - U'A') : ch;
	return unicodeLocale ? char32_t(towlower_l(wint_t(ch), unicodeLocale)) : ch;
}

bool sameIgnoringCase(char32_t a, char32_t b)
{
	return a == b || foldedCase(a) == foldedCase(b);
}

bool matchesAcrossLigatures(const std::u32string &characters, size_t first, const std::u32string &wanted, size_t &end)
{
	std::set<std::pair<size_t, size_t>> failed;
	std::function<bool(size_t, size_t)> matchFrom = [&](size_t i, size_t j) -> bool {
		if (i == wanted.size()) {
			end = j;
			return true;
		}
		if (j >= characters.size() || !failed.insert({i, j}).second)
			return false;
		if (sameIgnoringCase(wanted[i], characters[j]) && matchFrom(i + 1, j + 1))
			return true;
		for (size_t spanned = 1; spanned <= longestLigatureLength; ++spanned) {
			if (wanted[i] == unmappedLigature && j + spanned <= characters.size() && matchFrom(i + 1, j + spanned))
				return true;
			if (characters[j] == unmappedLigature && i + spanned <= wanted.size() && matchFrom(i + spanned, j + 1))
				return true;
		}
		return false;
	};
	return matchFrom(0, first);
}

struct Match {
	size_t first = 0;
	size_t end = 0;
	bool exact = false;
};

bool bestMatch(const std::u32string &characters, const std::u32string &wanted, size_t anchor, Match &match)
{
	const size_t windowStart = anchor > searchWindow ? anchor - searchWindow : 0;
	const size_t windowEnd = std::min(characters.size(), anchor + searchWindow + wanted.size());
	bool found = false;
	size_t best = 0;
	for (size_t at = characters.find(wanted, windowStart); at != std::u32string::npos && at + wanted.size() <= windowEnd;
		 at = characters.find(wanted, at + 1)) {
		const size_t distance = at > anchor ? at - anchor : anchor - at;
		const size_t bestDistance = best > anchor ? best - anchor : anchor - best;
		if (!found || distance < bestDistance) {
			best = at;
			found = true;
		}
	}
	if (found) {
		match = {best, best + wanted.size(), true};
		return true;
	}

	for (size_t distance = 0; distance <= searchWindow; ++distance) {
		for (int direction : {-1, 1}) {
			if (direction < 0 && distance > anchor)
				continue;
			const size_t first = direction < 0 ? anchor - distance : anchor + distance;
			size_t end = 0;
			if (first < characters.size() && matchesAcrossLigatures(characters, first, wanted, end)) {
				match = {first, end, false};
				return true;
			}
			if (distance == 0)
				break;
		}
	}
	return false;
}

double linePitch(const PageText &pageText)
{
	std::vector<double> lineCenters;
	std::vector<double> centers;
	double previousLeft = 0;
	double tallestGlyph = 0;
	for (int index : pageText.visibleIndices) {
		const CharBox &box = pageText.boxes[index];
		tallestGlyph = std::max(tallestGlyph, box.top - box.bottom);
		if (!centers.empty() && box.left < previousLeft) {
			std::nth_element(centers.begin(), centers.begin() + centers.size() / 2, centers.end());
			lineCenters.push_back(centers[centers.size() / 2]);
			centers.clear();
		}
		centers.push_back((box.bottom + box.top) / 2);
		previousLeft = box.left;
	}
	std::vector<double> gaps;
	for (size_t i = 1; i < lineCenters.size(); ++i) {
		if (lineCenters[i - 1] > lineCenters[i])
			gaps.push_back(lineCenters[i - 1] - lineCenters[i]);
	}
	if (gaps.empty())
		return tallestGlyph;
	std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
	return gaps[gaps.size() / 2];
}

std::vector<Stroke> strokesForLines(PdfText &text, int page, int firstIndex, int lastIndex, uint32_t color, uint32_t argb)
{
	const PageText &pageText = text.page(page);
	const double drawnAreaMargin = drawnAreaMarginInLinePitches * linePitch(pageText) * sceneUnitsPerPoint;
	std::vector<std::vector<std::pair<int, CharBox>>> lines;
	for (int index = firstIndex; index <= lastIndex; ++index) {
		if (isSpace(pageText.characters[index]))
			continue;
		const CharBox &box = pageText.boxes[index];
		if (lines.empty() || box.left < lines.back().back().second.left)
			lines.emplace_back();
		lines.back().emplace_back(index, box);
	}

	std::vector<Stroke> strokes;
	for (const std::vector<std::pair<int, CharBox>> &line : lines) {
		std::vector<double> centers;
		for (const auto &[index, box] : line)
			centers.push_back((box.bottom + box.top) / 2);
		std::nth_element(centers.begin(), centers.begin() + centers.size() / 2, centers.end());
		const double medianCenter = centers[centers.size() / 2];
		Stroke stroke;
		const CharBox &first = line.front().second;
		const CharBox &last = line.back().second;
		stroke.fromX = ((first.left + first.right) / 2 - pageText.width / 2) * sceneUnitsPerPoint;
		stroke.toX = ((last.left + last.right) / 2 - pageText.width / 2) * sceneUnitsPerPoint;
		stroke.firstIndex = line.front().first;
		stroke.lastIndex = line.back().first;
		stroke.y = (pageText.height - medianCenter) * sceneUnitsPerPoint;
		stroke.color = color;
		stroke.argb = argb;
		double left = first.left, right = last.right, top = first.top, bottom = first.bottom;
		for (const auto &[index, box] : line) {
			left = std::min(left, box.left);
			right = std::max(right, box.right);
			top = std::max(top, box.top);
			bottom = std::min(bottom, box.bottom);
		}
		stroke.drawnArea = {(left - pageText.width / 2) * sceneUnitsPerPoint - drawnAreaMargin,
			(pageText.height - top) * sceneUnitsPerPoint - drawnAreaMargin,
			(right - left) * sceneUnitsPerPoint + 2 * drawnAreaMargin,
			(top - bottom) * sceneUnitsPerPoint + 2 * drawnAreaMargin};
		strokes.push_back(stroke);
	}
	return strokes;
}

}

std::u32string utf32FromUtf8(const std::string &text)
{
	std::u32string out;
	for (size_t i = 0; i < text.size();) {
		const unsigned char lead = text[i];
		char32_t ch;
		size_t extra;
		if (lead < 0x80) {
			ch = lead;
			extra = 0;
		} else if ((lead >> 5) == 0x6) {
			ch = lead & 0x1F;
			extra = 1;
		} else if ((lead >> 4) == 0xE) {
			ch = lead & 0x0F;
			extra = 2;
		} else {
			ch = lead & 0x07;
			extra = 3;
		}
		for (size_t k = 1; k <= extra && i + k < text.size(); ++k)
			ch = ch << 6 | (text[i + k] & 0x3F);
		out.push_back(ch);
		i += extra + 1;
	}
	return out;
}

std::vector<SpineEntry> rebuildSpine(const EpubIndex &referenceIndex, PdfText &referenceText, PdfText &targetText,
	uint32_t lastNeededPage)
{
	std::vector<SpineEntry> rebuilt;
	uint32_t targetPage = 0;
	const uint32_t targetPageCount = uint32_t(targetText.pageCount());
	for (const SpineEntry &entry : referenceIndex.spine) {
		if (targetPage > lastNeededPage)
			break;
		size_t wanted = 0;
		for (uint32_t page = entry.pageStart; page < entry.pageStart + entry.pageCount; ++page)
			wanted += referenceText.page(int(page)).visibleIndices.size();
		SpineEntry rebuiltEntry;
		rebuiltEntry.href = entry.href;
		rebuiltEntry.pageStart = targetPage;
		size_t have = 0;
		while (have < wanted && targetPage < targetPageCount)
			have += targetText.page(int(targetPage++)).visibleIndices.size();
		rebuiltEntry.pageCount = targetPage - rebuiltEntry.pageStart;
		rebuilt.push_back(rebuiltEntry);
	}
	return rebuilt;
}

std::vector<Anchor> captureAnchors(PdfText &oldText, const std::vector<SpineEntry> &oldSpine,
	const std::vector<PageHighlights> &pages, size_t *notInOldLayout)
{
	std::vector<Anchor> pieces;
	for (const PageHighlights &page : pages) {
		const SpineEntry *entry = nullptr;
		for (const SpineEntry &candidate : oldSpine) {
			if (candidate.containsPage(page.pageIndex)) {
				entry = &candidate;
				break;
			}
		}
		if (!entry)
			continue;
		const PageText &oldPage = oldText.page(int(page.pageIndex));
		for (const Highlight &highlight : page.highlights) {
			if (!highlight.start)
				continue;
			const std::u32string storedText = withoutSpaces(utf32FromUtf8(highlight.text));
			const size_t length = highlight.length ? *highlight.length : storedText.size();
			const std::u32string oldPdfText = *highlight.start < oldPage.characters.size()
				? withoutSpaces(oldPage.characters.substr(*highlight.start, length))
				: std::u32string();
			if (!matchesStoredText(oldPdfText, storedText)) {
				if (notInOldLayout)
					++*notInOldLayout;
				continue;
			}
			Anchor piece;
			piece.href = entry->href;
			piece.first = visibleCharactersBefore(oldText, *entry, page.pageIndex, *highlight.start);
			piece.end = piece.first + storedText.size();
			piece.color = highlight.color;
			piece.argb = highlight.color == argbColor && highlight.argb ? *highlight.argb : opaqueBlack;
			piece.pieces.push_back(highlight.text);
			if (!highlight.rects.empty())
				piece.displaced.push_back({page.pageIndex, coveringRect(highlight.rects)});
			pieces.push_back(std::move(piece));
		}
	}

	std::sort(pieces.begin(), pieces.end(), [](const Anchor &a, const Anchor &b) {
		return a.href != b.href ? a.href < b.href : a.first < b.first;
	});
	std::vector<Anchor> merged;
	for (Anchor &piece : pieces) {
		if (!merged.empty()) {
			Anchor &previous = merged.back();
			if (previous.href == piece.href && previous.color == piece.color && previous.argb == piece.argb
				&& piece.first <= previous.end) {
				previous.end = std::max(previous.end, piece.end);
				previous.pieces.insert(previous.pieces.end(), piece.pieces.begin(), piece.pieces.end());
				previous.displaced.insert(previous.displaced.end(), piece.displaced.begin(), piece.displaced.end());
				continue;
			}
		}
		merged.push_back(std::move(piece));
	}

	std::map<std::string, VisibleString> chapters;
	for (Anchor &anchor : merged) {
		const SpineEntry *entry = nullptr;
		for (const SpineEntry &candidate : oldSpine) {
			if (candidate.href == anchor.href) {
				entry = &candidate;
				break;
			}
		}
		auto chapter = chapters.find(anchor.href);
		if (chapter == chapters.end())
			chapter = chapters.emplace(anchor.href, visibleString(oldText, *entry)).first;
		const std::u32string &characters = chapter->second.characters;
		const size_t end = std::min(anchor.end, characters.size());
		anchor.visibleText = anchor.first < end ? characters.substr(anchor.first, end - anchor.first) : std::u32string();
		anchor.first = weightedPosition(characters, anchor.first);
		anchor.end = anchor.first + weightedPosition(anchor.visibleText, anchor.visibleText.size());
	}
	return merged;
}

Plan planReflow(PdfText &newText, const EpubIndex &newIndex, const std::vector<Anchor> &anchors,
	const std::vector<PageHighlights> &onNewPages)
{
	Plan plan;
	std::map<std::string, VisibleString> chapters;
	for (const Anchor &anchor : anchors) {
		std::string description;
		for (const std::string &piece : anchor.pieces)
			description += (description.empty() ? "" : " | ") + piece;

		const SpineEntry *entry = newIndex.entryForHref(anchor.href);
		if (!entry || anchor.visibleText.empty()) {
			plan.unresolved.push_back(description);
			continue;
		}
		auto chapter = chapters.find(anchor.href);
		if (chapter == chapters.end())
			chapter = chapters.emplace(anchor.href, visibleString(newText, *entry)).first;
		const VisibleString &visible = chapter->second;

		Match match;
		const size_t searchFrom = indexAtWeightedPosition(visible.characters, anchor.first);
		if (!bestMatch(visible.characters, anchor.visibleText, searchFrom, match) || match.end == 0) {
			plan.unresolved.push_back(description);
			continue;
		}
		const uint32_t highlight = uint32_t(plan.placed.size());
		plan.placed.push_back({description, int(weightedPosition(visible.characters, match.first)) - int(anchor.first), match.exact});

		for (const PageRect &displaced : anchor.displaced) {
			plan.erase[displaced.pageIndex].push_back(displaced.rect);
			plan.eraseHighlights[displaced.pageIndex].push_back(highlight);
		}

		const auto [startPage, startIndex] = visible.positions[match.first];
		const auto [lastPage, lastIndex] = visible.positions[match.end - 1];
		for (int page = startPage; page <= lastPage; ++page) {
			const PageText &pageText = newText.page(page);
			if (pageText.visibleIndices.empty())
				continue;
			const int firstOnPage = page == startPage ? startIndex : pageText.visibleIndices.front();
			const int lastOnPage = page == lastPage ? lastIndex : pageText.visibleIndices.back();
			for (Stroke &stroke : strokesForLines(newText, page, firstOnPage, lastOnPage, anchor.color, anchor.argb)) {
				stroke.highlight = highlight;
				plan.create[uint32_t(page)].push_back(stroke);
			}
		}
	}
	for (const PageHighlights &page : onNewPages) {
		const auto strokes = plan.create.find(page.pageIndex);
		if (strokes == plan.create.end())
			continue;
		const PageText &pageText = newText.page(int(page.pageIndex));
		for (const Highlight &existing : page.highlights) {
			if (!existing.start || existing.rects.empty())
				continue;
			const std::u32string storedText = withoutSpaces(utf32FromUtf8(existing.text));
			const size_t length = existing.length ? *existing.length : storedText.size();
			if (*existing.start >= pageText.characters.size()
				|| !matchesStoredText(withoutSpaces(pageText.characters.substr(*existing.start, length)), storedText))
				continue;
			const uint32_t existingArgb = existing.color == argbColor && existing.argb ? *existing.argb : opaqueBlack;
			const int existingFirst = int(*existing.start);
			const int existingLast = int(*existing.start + length) - 1;
			const bool redrawn = std::any_of(strokes->second.begin(), strokes->second.end(), [&](const Stroke &stroke) {
				return stroke.color == existing.color && stroke.argb == existingArgb
					&& stroke.firstIndex <= existingLast && existingFirst <= stroke.lastIndex;
			});
			if (redrawn)
				plan.replace[page.pageIndex].push_back(coveringRect(existing.rects));
		}
	}
	return plan;
}

}
