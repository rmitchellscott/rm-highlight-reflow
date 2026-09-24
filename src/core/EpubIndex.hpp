#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reflow {

struct SpineEntry {
	std::string href;
	uint32_t pageStart = 0;
	uint32_t pageCount = 0;

	bool containsPage(uint32_t page) const { return page >= pageStart && page < pageStart + pageCount; }
};

struct EpubIndex {
	std::vector<SpineEntry> spine;
	std::string error;

	bool ok() const { return error.empty(); }
	const SpineEntry *entryForPage(uint32_t page) const;
	const SpineEntry *entryForHref(const std::string &href) const;
};

EpubIndex readEpubIndex(const std::string &path);

}
