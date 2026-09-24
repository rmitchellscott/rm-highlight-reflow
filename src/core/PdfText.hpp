#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace reflow {

struct CharBox {
	double left = 0;
	double bottom = 0;
	double right = 0;
	double top = 0;
};

struct PageText {
	double width = 0;
	double height = 0;
	std::u32string characters;
	std::vector<CharBox> boxes;
	std::vector<int> visibleIndices;
};

class Pdfium;

class PdfText {
public:
	static std::shared_ptr<Pdfium> load(const std::string &libraryPath, bool privateNamespace);

	PdfText(std::shared_ptr<Pdfium> pdfium, const std::string &pdfPath);
	~PdfText();
	PdfText(const PdfText &) = delete;
	PdfText &operator=(const PdfText &) = delete;

	bool ok() const { return document != nullptr; }
	int pageCount() const;
	const PageText &page(int index);

private:
	std::shared_ptr<Pdfium> pdfium;
	void *document = nullptr;
	std::map<int, PageText> pages;
};

constexpr char32_t unmappedLigature = 0xFFFD;

bool isSpace(char32_t ch);
bool isLigatureCharacter(char32_t ch);
char32_t normalizedHyphen(char32_t ch);

}
