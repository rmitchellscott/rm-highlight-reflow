#include "PdfText.hpp"

#include <dlfcn.h>

namespace reflow {

class Pdfium {
public:
	void *handle = nullptr;
	void (*initLibrary)() = nullptr;
	void *(*loadDocument)(const char *, const char *) = nullptr;
	void (*closeDocument)(void *) = nullptr;
	int (*getPageCount)(void *) = nullptr;
	void *(*loadPage)(void *, int) = nullptr;
	void (*closePage)(void *) = nullptr;
	double (*getPageWidth)(void *) = nullptr;
	double (*getPageHeight)(void *) = nullptr;
	void *(*loadTextPage)(void *) = nullptr;
	void (*closeTextPage)(void *) = nullptr;
	int (*countChars)(void *) = nullptr;
	unsigned (*getUnicode)(void *, int) = nullptr;
	int (*getCharBox)(void *, int, double *, double *, double *, double *) = nullptr;
	int (*hasUnicodeMapError)(void *, int) = nullptr;
};

std::shared_ptr<Pdfium> PdfText::load(const std::string &libraryPath, bool privateNamespace)
{
	auto pdfium = std::make_shared<Pdfium>();
#ifdef __linux__
	pdfium->handle = privateNamespace ? dlmopen(LM_ID_NEWLM, libraryPath.c_str(), RTLD_NOW) : dlopen(libraryPath.c_str(), RTLD_NOW);
#else
	(void)privateNamespace;
	pdfium->handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
	if (!pdfium->handle)
		return nullptr;

	auto resolve = [&](auto &function, const char *name) {
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(pdfium->handle, name));
		return function != nullptr;
	};
	const bool resolved = resolve(pdfium->initLibrary, "FPDF_InitLibrary")
		&& resolve(pdfium->loadDocument, "FPDF_LoadDocument")
		&& resolve(pdfium->closeDocument, "FPDF_CloseDocument")
		&& resolve(pdfium->getPageCount, "FPDF_GetPageCount")
		&& resolve(pdfium->loadPage, "FPDF_LoadPage")
		&& resolve(pdfium->closePage, "FPDF_ClosePage")
		&& resolve(pdfium->getPageWidth, "FPDF_GetPageWidth")
		&& resolve(pdfium->getPageHeight, "FPDF_GetPageHeight")
		&& resolve(pdfium->loadTextPage, "FPDFText_LoadPage")
		&& resolve(pdfium->closeTextPage, "FPDFText_ClosePage")
		&& resolve(pdfium->countChars, "FPDFText_CountChars")
		&& resolve(pdfium->getUnicode, "FPDFText_GetUnicode")
		&& resolve(pdfium->getCharBox, "FPDFText_GetCharBox");
	if (!resolved)
		return nullptr;
	resolve(pdfium->hasUnicodeMapError, "FPDFText_HasUnicodeMapError");
	pdfium->initLibrary();
	return pdfium;
}

PdfText::PdfText(std::shared_ptr<Pdfium> pdfium, const std::string &pdfPath) : pdfium(std::move(pdfium))
{
	if (this->pdfium)
		document = this->pdfium->loadDocument(pdfPath.c_str(), nullptr);
}

PdfText::~PdfText()
{
	if (document)
		pdfium->closeDocument(document);
}

int PdfText::pageCount() const
{
	return document ? pdfium->getPageCount(document) : 0;
}

const PageText &PdfText::page(int index)
{
	auto cached = pages.find(index);
	if (cached != pages.end())
		return cached->second;

	PageText &text = pages[index];
	void *page = pdfium->loadPage(document, index);
	if (!page)
		return text;
	text.width = pdfium->getPageWidth(page);
	text.height = pdfium->getPageHeight(page);
	void *textPage = pdfium->loadTextPage(page);
	if (textPage) {
		const int count = pdfium->countChars(textPage);
		text.characters.reserve(count);
		text.boxes.reserve(count);
		for (int i = 0; i < count; ++i) {
			const char32_t extracted = pdfium->getUnicode(textPage, i);
			const bool unmapped = pdfium->hasUnicodeMapError && pdfium->hasUnicodeMapError(textPage, i);
			const char32_t ch = unmapped || isLigatureCharacter(extracted) ? unmappedLigature : extracted;
			CharBox box;
			pdfium->getCharBox(textPage, i, &box.left, &box.right, &box.bottom, &box.top);
			text.characters.push_back(ch);
			text.boxes.push_back(box);
			if (!isSpace(ch) && box.bottom >= 0 && box.top <= text.height)
				text.visibleIndices.push_back(i);
		}
		pdfium->closeTextPage(textPage);
	}
	pdfium->closePage(page);
	return text;
}

bool isSpace(char32_t ch)
{
	switch (ch) {
	case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
	case 0x20: case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
		return true;
	default:
		return ch >= 0x2000 && ch <= 0x200A;
	}
}

bool isLigatureCharacter(char32_t ch)
{
	return ch >= 0xFB00 && ch <= 0xFB06;
}

char32_t normalizedHyphen(char32_t ch)
{
	return ch == 0xFFFE ? U'-' : ch;
}

}
