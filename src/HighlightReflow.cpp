#include "HighlightReflow.hpp"

#include "core/AnchorCodec.hpp"
#include "core/Anchoring.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMetaObject>
#include <QVariantList>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

const QString documentsDirectory = QStringLiteral("/home/root/.local/share/remarkable/xochitl");
const char *const pdfiumPaths[] = {"/usr/lib/libpdfium.so", "libpdfium.so"};

constexpr int highlighterTool = 18;
constexpr int eraseSectionTool = 8;
constexpr double highlighterMaskScale = 1.1042158603668213;
constexpr LinePoint highlighterPointStyle = {0, 0, 0, 133, 0, 150};
constexpr LinePoint lassoPointStyle = {0, 0, 25, 25, 0, 255};
constexpr double highlighterPaddingX = 18;
constexpr double highlighterPaddingY = 26;
constexpr double highlighterPointSpacing = 4;
constexpr double lassoPointSpacing = 8;

struct CaptureJob {
	std::string oldPdf;
	std::string oldIndex;
	std::vector<std::pair<uint32_t, std::string>> pageFiles;
};

struct PlanJob {
	CaptureJob capture;
	std::vector<std::pair<uint32_t, std::string>> newPageFiles;
	std::string newPdf;
	std::string newIndex;
	std::string capturedAnchors;
};

struct PageSlot {
	std::string uuid;
	uint32_t pdfPage;
};

std::vector<PageSlot> liveSlots(const QString &contentPath)
{
	std::vector<PageSlot> pageSlots;
	QFile content(contentPath);
	if (!content.open(QIODevice::ReadOnly))
		return pageSlots;
	const QJsonObject root = QJsonDocument::fromJson(content.readAll()).object();
	if (!root.contains(QStringLiteral("cPages"))) {
		const QJsonArray pageIds = root.value(QStringLiteral("pages")).toArray();
		const QJsonArray redirects = root.value(QStringLiteral("redirectionPageMap")).toArray();
		for (qsizetype i = 0; i < pageIds.size() && i < redirects.size(); ++i) {
			if (redirects[i].toInt(-1) >= 0)
				pageSlots.push_back({pageIds[i].toString().toStdString(), uint32_t(redirects[i].toInt())});
		}
		return pageSlots;
	}
	const QJsonArray pages = root.value(QStringLiteral("cPages")).toObject().value(QStringLiteral("pages")).toArray();
	for (const QJsonValue &page : pages) {
		const QJsonObject entry = page.toObject();
		if (entry.value(QStringLiteral("deleted")).toObject().value(QStringLiteral("value")).toInt() != 0)
			continue;
		const QJsonValue redirect = entry.value(QStringLiteral("redir")).toObject().value(QStringLiteral("value"));
		if (!redirect.isDouble() || redirect.toInt() < 0)
			continue;
		pageSlots.push_back({entry.value(QStringLiteral("id")).toString().toStdString(), uint32_t(redirect.toInt())});
	}
	return pageSlots;
}

LinePoint pointAt(const LinePoint &style, const QPointF &position)
{
	LinePoint point = style;
	point.x = float(position.x());
	point.y = float(position.y());
	return point;
}

std::shared_ptr<reflow::Pdfium> loadPrivatePdfium()
{
	for (const char *path : pdfiumPaths) {
		if (auto pdfium = reflow::PdfText::load(path, true))
			return pdfium;
	}
	return nullptr;
}

bool indexMatchesPdf(const reflow::EpubIndex &index, int pdfPageCount)
{
	uint32_t pages = 0;
	for (const reflow::SpineEntry &entry : index.spine)
		pages += entry.pageCount;
	return std::abs(int(pages) - pdfPageCount) <= 1;
}

std::vector<reflow::PageHighlights> readOldHighlights(const CaptureJob &job, int oldPageCount, uint32_t &lastHighlightedPage)
{
	std::vector<reflow::PageHighlights> pages;
	lastHighlightedPage = 0;
	for (const auto &[pageIndex, path] : job.pageFiles) {
		if (int(pageIndex) >= oldPageCount)
			continue;
		reflow::HighlightReadResult read = reflow::readHighlights(path);
		if (!read.ok() || read.highlights.empty())
			continue;
		pages.push_back({pageIndex, std::move(read.highlights)});
		lastHighlightedPage = std::max(lastHighlightedPage, pageIndex);
	}
	return pages;
}

std::string encodeCapture(const std::vector<reflow::Anchor> &anchors, uint64_t notInOldLayout)
{
	std::string encoded(reinterpret_cast<const char *>(&notInOldLayout), sizeof(notInOldLayout));
	return encoded + reflow::encodeAnchors(anchors);
}

bool decodeCapture(const std::string &encoded, std::vector<reflow::Anchor> &anchors, uint64_t &notInOldLayout)
{
	if (encoded.size() < sizeof(notInOldLayout))
		return false;
	std::memcpy(&notInOldLayout, encoded.data(), sizeof(notInOldLayout));
	return reflow::decodeAnchors(encoded.substr(sizeof(notInOldLayout)), anchors);
}

std::string runCaptureJob(const CaptureJob &job, std::string &error)
{
	auto pdfium = loadPrivatePdfium();
	if (!pdfium) {
		error = "cannot load pdfium";
		return {};
	}
	reflow::PdfText oldText(pdfium, job.oldPdf);
	const reflow::EpubIndex oldIndex = reflow::readEpubIndex(job.oldIndex);
	if (!oldText.ok()) {
		error = "cannot open old pdf";
		return {};
	}
	if (!oldIndex.ok() || !indexMatchesPdf(oldIndex, oldText.pageCount())) {
		error = "old index does not describe the old pdf";
		return {};
	}
	uint32_t lastHighlightedPage;
	const std::vector<reflow::PageHighlights> pages = readOldHighlights(job, oldText.pageCount(), lastHighlightedPage);
	size_t notInOldLayout = 0;
	const std::vector<reflow::Anchor> anchors = reflow::captureAnchors(oldText, oldIndex.spine, pages, &notInOldLayout);
	return encodeCapture(anchors, notInOldLayout);
}

std::string runPlanJob(const PlanJob &job, std::string &error)
{
	auto pdfium = loadPrivatePdfium();
	if (!pdfium) {
		error = "cannot load pdfium";
		return {};
	}
	reflow::PdfText newText(pdfium, job.newPdf);
	const reflow::EpubIndex newIndex = reflow::readEpubIndex(job.newIndex);
	if (!newText.ok()) {
		error = "cannot open new pdf";
		return {};
	}
	if (!newIndex.ok()) {
		error = "cannot read new index: " + newIndex.error;
		return {};
	}

	std::vector<reflow::Anchor> anchors;
	uint64_t notInOldLayout = 0;
	if (!job.capturedAnchors.empty()) {
		if (!decodeCapture(job.capturedAnchors, anchors, notInOldLayout)) {
			error = "cannot decode captured anchors";
			return {};
		}
	} else {
		reflow::PdfText oldText(pdfium, job.capture.oldPdf);
		const reflow::EpubIndex oldIndex = reflow::readEpubIndex(job.capture.oldIndex);
		if (!oldText.ok()) {
			error = "cannot open old pdf";
			return {};
		}
		uint32_t lastHighlightedPage;
		const std::vector<reflow::PageHighlights> pages = readOldHighlights(job.capture, oldText.pageCount(), lastHighlightedPage);
		if (pages.empty())
			return reflow::encodePlan(reflow::Plan());
		const std::vector<reflow::SpineEntry> oldSpine = oldIndex.ok() && indexMatchesPdf(oldIndex, oldText.pageCount())
			? oldIndex.spine
			: reflow::rebuildSpine(newIndex, newText, oldText, lastHighlightedPage);
		size_t skipped = 0;
		anchors = reflow::captureAnchors(oldText, oldSpine, pages, &skipped);
		notInOldLayout = skipped;
	}
	std::vector<reflow::PageHighlights> onNewPages;
	for (const auto &[pageIndex, path] : job.newPageFiles) {
		reflow::HighlightReadResult read = reflow::readHighlights(path);
		if (read.ok() && !read.highlights.empty())
			onNewPages.push_back({pageIndex, std::move(read.highlights)});
	}
	reflow::Plan plan = reflow::planReflow(newText, newIndex, anchors, onNewPages);
	plan.notInOldLayout = notInOldLayout;
	return reflow::encodePlan(plan);
}

bool writeAll(int fd, const std::string &data)
{
	size_t written = 0;
	while (written < data.size()) {
		const ssize_t n = write(fd, data.data() + written, data.size() - written);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		written += size_t(n);
	}
	return true;
}

bool runForked(const std::function<std::string(std::string &)> &work, std::string &result, std::string &error)
{
	int pipeFds[2];
	if (pipe(pipeFds) != 0) {
		error = "pipe failed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		close(pipeFds[0]);
		close(pipeFds[1]);
		error = "fork failed";
		return false;
	}
	if (child == 0) {
		close(pipeFds[0]);
		std::string childError;
		const std::string output = work(childError);
		const std::string message = childError.empty() ? "R" + output : "E" + childError;
		_exit(writeAll(pipeFds[1], message) ? 0 : 1);
	}

	close(pipeFds[1]);
	std::string message;
	char buffer[65536];
	for (;;) {
		const ssize_t n = read(pipeFds[0], buffer, sizeof(buffer));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		message.append(buffer, size_t(n));
	}
	close(pipeFds[0]);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || message.empty()) {
		error = "worker process failed";
		return false;
	}
	if (message[0] == 'E') {
		error = message.substr(1);
		return false;
	}
	result = message.substr(1);
	return true;
}

qint64 millisecondsSince(const std::chrono::steady_clock::time_point &start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

QVariantMap planToVariant(const reflow::Plan &plan, const std::vector<PageSlot> &oldSlots, const std::vector<PageSlot> &newSlots)
{
	std::map<uint32_t, std::string> oldSlotForPage, newSlotForPage;
	std::map<std::string, uint32_t> newPageForSlot;
	for (const PageSlot &slot : oldSlots)
		oldSlotForPage.emplace(slot.pdfPage, slot.uuid);
	for (const PageSlot &slot : newSlots) {
		newSlotForPage.emplace(slot.pdfPage, slot.uuid);
		newPageForSlot.emplace(slot.uuid, slot.pdfPage);
	}

	QVariantMap erase;
	QVariantMap replace;
	QVariantMap create;
	QVariantMap pageNumbers;
	std::set<uint32_t> highlightsWithoutPage;
	int erasesOnDeletedPages = 0;
	int createsWithoutPage = 0;
	for (const auto &[oldPage, rects] : plan.erase) {
		const auto slot = oldSlotForPage.find(oldPage);
		if (slot == oldSlotForPage.end() || !newPageForSlot.count(slot->second)) {
			erasesOnDeletedPages += int(rects.size());
			continue;
		}
		const QString uuid = QString::fromStdString(slot->second);
		const std::vector<uint32_t> &owners = plan.eraseHighlights.at(oldPage);
		QVariantList list = erase.value(uuid).toList();
		for (size_t i = 0; i < rects.size(); ++i) {
			const reflow::SceneRect &rect = rects[i];
			list.append(QVariantMap{
				{"rect", QVariantList{rect.x, rect.y, rect.width, rect.height}},
				{"highlight", int(owners[i])},
			});
		}
		erase.insert(uuid, list);
		pageNumbers.insert(uuid, int(newPageForSlot[slot->second]) + 1);
	}
	int replaced = 0;
	for (const auto &[newPage, rects] : plan.replace) {
		const auto slot = newSlotForPage.find(newPage);
		if (slot == newSlotForPage.end())
			continue;
		const QString uuid = QString::fromStdString(slot->second);
		QVariantList list = replace.value(uuid).toList();
		for (const reflow::SceneRect &rect : rects)
			list.append(QVariant(QVariantList{rect.x, rect.y, rect.width, rect.height}));
		replace.insert(uuid, list);
		replaced += int(rects.size());
	}
	for (const auto &[newPage, strokes] : plan.create) {
		const auto slot = newSlotForPage.find(newPage);
		if (slot == newSlotForPage.end()) {
			createsWithoutPage += int(strokes.size());
			for (const reflow::Stroke &stroke : strokes)
				highlightsWithoutPage.insert(stroke.highlight);
			continue;
		}
		const QString uuid = QString::fromStdString(slot->second);
		QVariantList list = create.value(uuid).toList();
		for (const reflow::Stroke &stroke : strokes) {
			list.append(QVariantMap{
				{"from", QVariantList{stroke.fromX, stroke.y}},
				{"to", QVariantList{stroke.toX, stroke.y}},
				{"color", stroke.color},
				{"rgba", stroke.argb},
				{"drawnArea", QVariantList{stroke.drawnArea.x, stroke.drawnArea.y, stroke.drawnArea.width, stroke.drawnArea.height}},
				{"highlight", int(stroke.highlight)},
			});
		}
		create.insert(uuid, list);
		pageNumbers.insert(uuid, int(newPage) + 1);
	}
	int exact = 0;
	for (const reflow::Placement &placed : plan.placed)
		exact += placed.exact ? 1 : 0;
	QVariantList unresolved;
	for (const std::string &text : plan.unresolved)
		unresolved.append(QString::fromStdString(text));
	QVariantList withoutPage;
	for (uint32_t highlight : highlightsWithoutPage)
		withoutPage.append(int(highlight));
	return {
		{"erase", erase},
		{"replace", replace},
		{"create", create},
		{"pageNumbers", pageNumbers},
		{"placed", int(plan.placed.size())},
		{"exact", exact},
		{"unresolved", unresolved},
		{"notInOldLayout", int(plan.notInOldLayout)},
		{"erasesOnDeletedPages", erasesOnDeletedPages},
		{"replaced", replaced},
		{"createsWithoutPage", createsWithoutPage},
		{"highlightsWithoutPage", withoutPage},
	};
}


}

HighlightReflow::HighlightReflow(const QString &stateDirectory, QObject *parent)
	: QObject(parent), stateDirectory(stateDirectory)
{
}

QString HighlightReflow::documentBase(const QString &documentId) const
{
	return documentsDirectory + QLatin1Char('/') + documentId;
}

QString HighlightReflow::oldLayoutDirectory(const QString &documentId) const
{
	return stateDirectory + QLatin1Char('/') + documentId;
}

bool HighlightReflow::keepOldLayout(const QString &documentId, bool captureHighlightsNow)
{
	const QString base = documentBase(documentId);
	const QString directory = oldLayoutDirectory(documentId);
	QDir().mkpath(directory);
	const QString keptPdf = directory + QStringLiteral("/old.pdf");
	const QString keptIndex = directory + QStringLiteral("/old.epubindex");
	QFile::remove(keptPdf);
	QFile::remove(keptIndex);

	const QString backup = base + QStringLiteral(".pdf.backup");
	const bool pdfKept = link(QFile::encodeName(backup).constData(), QFile::encodeName(keptPdf).constData()) == 0
		|| QFile::copy(backup, keptPdf);
	const bool indexKept = QFile::copy(base + QStringLiteral(".epubindex"), keptIndex);
	const QString keptContent = directory + QStringLiteral("/old.content");
	QFile::remove(keptContent);
	QFile::remove(directory + QStringLiteral("/anchors.bin"));
	QFile::remove(directory + QStringLiteral("/applied"));
	const bool contentKept = QFile::copy(base + QStringLiteral(".content"), keptContent);
	log(QStringLiteral("kept old layout of %1: pdf %2, index %3, content %4").arg(documentId).arg(pdfKept).arg(indexKept).arg(contentKept));
	if (!pdfKept || !indexKept || !contentKept) {
		discardOldLayout(documentId);
		return false;
	}
	if (captureHighlightsNow)
		captureOldLayout(documentId);
	return true;
}

std::vector<std::pair<uint32_t, std::string>> HighlightReflow::highlightFiles(const QString &documentId, const QString &contentPath) const
{
	const QString base = documentBase(documentId);
	std::vector<std::pair<uint32_t, std::string>> files;
	for (const PageSlot &slot : liveSlots(contentPath)) {
		const QString pageFile = base + QLatin1Char('/') + QString::fromStdString(slot.uuid) + QStringLiteral(".rm");
		if (QFile::exists(pageFile))
			files.emplace_back(slot.pdfPage, QFile::encodeName(pageFile).toStdString());
	}
	return files;
}

void HighlightReflow::captureOldLayout(const QString &documentId)
{
	const QString directory = oldLayoutDirectory(documentId);
	CaptureJob job;
	job.oldPdf = QFile::encodeName(directory + QStringLiteral("/old.pdf")).toStdString();
	job.oldIndex = QFile::encodeName(directory + QStringLiteral("/old.epubindex")).toStdString();
	job.pageFiles = highlightFiles(documentId, directory + QStringLiteral("/old.content"));

	CapturedLayout &layout = captured[documentId];
	layout = CapturedLayout();
	layout.capturing = true;
	layout.generation = nextCaptureGeneration++;
	layout.startedAt = QDateTime::currentDateTimeUtc();
	const quint64 generation = layout.generation;
	const QDateTime startedAt = layout.startedAt;

	std::thread([this, documentId, job, generation, startedAt]() {
		const auto started = std::chrono::steady_clock::now();
		std::string anchors;
		std::string error;
		const bool ok = runForked([&job](std::string &childError) { return runCaptureJob(job, childError); }, anchors, error);
		const qint64 elapsed = millisecondsSince(started);
		QMetaObject::invokeMethod(this, [this, documentId, generation, startedAt, ok, anchors, error, elapsed]() {
			auto layout = captured.find(documentId);
			if (layout == captured.end() || layout->generation != generation)
				return;
			layout->capturing = false;
			if (ok) {
				layout->hasAnchors = true;
				layout->anchors = anchors;
				QFile saved(oldLayoutDirectory(documentId) + QStringLiteral("/anchors.bin"));
				if (saved.open(QIODevice::WriteOnly)) {
					saved.write(anchors.data(), qint64(anchors.size()));
					saved.setFileTime(startedAt, QFileDevice::FileModificationTime);
				}
				log(QStringLiteral("captured old layout of %1 in %2 ms").arg(documentId).arg(elapsed));
			} else {
				log(QStringLiteral("capture of %1 failed after %2 ms (%3), planning will read the old layout itself")
						.arg(documentId).arg(elapsed).arg(QString::fromStdString(error)));
			}
			if (layout->planWanted) {
				layout->planWanted = false;
				startPlan(documentId);
			}
		}, Qt::QueuedConnection);
	}).detach();
}

bool HighlightReflow::hasOldLayout(const QString &documentId) const
{
	const QString directory = oldLayoutDirectory(documentId);
	return QFile::exists(directory + QStringLiteral("/old.pdf")) && QFile::exists(directory + QStringLiteral("/old.epubindex"))
		&& QFile::exists(directory + QStringLiteral("/old.content"));
}

void HighlightReflow::reportProblem(const QString &documentId, const QString &message)
{
	log(QStringLiteral("reported for %1: %2").arg(documentId, message));
	emit problemReported(documentId, message);
}

void HighlightReflow::markApplyStarted(const QString &documentId)
{
	QFile marker(oldLayoutDirectory(documentId) + QStringLiteral("/applied"));
	if (!marker.open(QIODevice::WriteOnly))
		log(QStringLiteral("cannot mark the reflow of %1 as applied").arg(documentId));
}

bool HighlightReflow::isLivePage(const QString &documentId, const QString &pageId) const
{
	const std::string uuid = pageId.toStdString();
	const std::vector<PageSlot> pageSlots = liveSlots(documentBase(documentId) + QStringLiteral(".content"));
	return std::any_of(pageSlots.begin(), pageSlots.end(), [&](const PageSlot &slot) { return slot.uuid == uuid; });
}

void HighlightReflow::discardOldLayout(const QString &documentId)
{
	captured.remove(documentId);
	QDir(oldLayoutDirectory(documentId)).removeRecursively();
}

void HighlightReflow::planReflow(const QString &documentId)
{
	auto layout = captured.find(documentId);
	if (layout != captured.end() && layout->capturing) {
		layout->planWanted = true;
		log(QStringLiteral("plan for %1 waits for the old layout capture").arg(documentId));
		return;
	}
	startPlan(documentId);
}

void HighlightReflow::startPlan(const QString &documentId)
{
	const QString base = documentBase(documentId);
	const QString directory = oldLayoutDirectory(documentId);
	PlanJob job;
	job.capture.oldPdf = QFile::encodeName(directory + QStringLiteral("/old.pdf")).toStdString();
	job.capture.oldIndex = QFile::encodeName(directory + QStringLiteral("/old.epubindex")).toStdString();
	job.newPdf = QFile::encodeName(base + QStringLiteral(".pdf")).toStdString();
	job.newIndex = QFile::encodeName(base + QStringLiteral(".epubindex")).toStdString();
	const QString oldContent = directory + QStringLiteral("/old.content");
	const std::vector<PageSlot> oldSlots = liveSlots(oldContent);
	const std::vector<PageSlot> newSlots = liveSlots(base + QStringLiteral(".content"));
	auto layout = captured.find(documentId);
	QFile savedAnchors(directory + QStringLiteral("/anchors.bin"));
	QDateTime captureStartedAt;
	if (layout != captured.end() && layout->hasAnchors) {
		job.capturedAnchors = layout->anchors;
		captureStartedAt = layout->startedAt;
	} else if (savedAnchors.open(QIODevice::ReadOnly)) {
		job.capturedAnchors = savedAnchors.readAll().toStdString();
		captureStartedAt = QFileInfo(savedAnchors).lastModified().toUTC();
	}
	if (!job.capturedAnchors.empty() && !QFile::exists(directory + QStringLiteral("/applied"))) {
		int changedSinceCapture = 0;
		for (const auto &[pageIndex, path] : highlightFiles(documentId, oldContent)) {
			if (QFileInfo(QFile::decodeName(path.c_str())).lastModified().toUTC() > captureStartedAt)
				++changedSinceCapture;
		}
		if (changedSinceCapture > 0) {
			log(QStringLiteral("%1 pages of %2 were saved after the capture, reading the old layout again").arg(changedSinceCapture).arg(documentId));
			job.capturedAnchors.clear();
		}
	}
	if (job.capturedAnchors.empty())
		job.capture.pageFiles = highlightFiles(documentId, oldContent);
	job.newPageFiles = highlightFiles(documentId, base + QStringLiteral(".content"));

	std::thread([this, documentId, job, oldSlots, newSlots]() {
		const auto started = std::chrono::steady_clock::now();
		std::string encoded;
		std::string error;
		reflow::Plan plan;
		const bool ok = runForked([&job](std::string &childError) { return runPlanJob(job, childError); }, encoded, error)
			&& reflow::decodePlan(encoded, plan);
		const qint64 elapsed = millisecondsSince(started);
		const bool usedCapture = !job.capturedAnchors.empty();
		if (ok) {
			const QVariantMap variant = planToVariant(plan, oldSlots, newSlots);
			QMetaObject::invokeMethod(this, [this, documentId, variant, elapsed, usedCapture]() {
				log(QStringLiteral("planned %1 in %2 ms (%3)").arg(documentId).arg(elapsed)
						.arg(usedCapture ? QStringLiteral("old layout captured during render") : QStringLiteral("both layouts read now")));
				emit planReady(documentId, variant);
			}, Qt::QueuedConnection);
		} else {
			const QString reason = QString::fromStdString(error.empty() ? "cannot decode plan" : error);
			QMetaObject::invokeMethod(this, [this, documentId, reason]() { emit planFailed(documentId, reason); }, Qt::QueuedConnection);
		}
	}).detach();
}

Line HighlightReflow::highlighterStroke(const QPointF &from, const QPointF &to, int color, quint32 rgba) const
{
	Line stroke = {};
	stroke.tool = highlighterTool;
	stroke.color = color;
	stroke.rgba = rgba;
	stroke.maskScale = highlighterMaskScale;
	const QLineF path(from, to);
	const int pointCount = std::max(2, int(path.length() / highlighterPointSpacing));
	for (int i = 0; i < pointCount; ++i)
		stroke.points.append(pointAt(highlighterPointStyle, path.pointAt(qreal(i) / (pointCount - 1))));
	stroke.bounds = QRectF(from, to).normalized().adjusted(-highlighterPaddingX, -highlighterPaddingY, highlighterPaddingX, highlighterPaddingY);
	return stroke;
}

Line HighlightReflow::eraseLasso(const QRectF &sceneRect) const
{
	Line lasso = {};
	lasso.tool = eraseSectionTool;
	lasso.rgba = 0xff000000;
	lasso.maskScale = 1.0;
	lasso.bounds = sceneRect;
	const QPointF corners[] = {sceneRect.topLeft(), sceneRect.topRight(), sceneRect.bottomRight(), sceneRect.bottomLeft(), sceneRect.topLeft()};
	for (int side = 0; side < 4; ++side) {
		const QLineF edge(corners[side], corners[side + 1]);
		const int steps = std::max(1, int(edge.length() / lassoPointSpacing));
		for (int i = 0; i < steps; ++i)
			lasso.points.append(pointAt(lassoPointStyle, edge.pointAt(qreal(i) / steps)));
	}
	lasso.points.append(pointAt(lassoPointStyle, sceneRect.topLeft()));
	return lasso;
}

void HighlightReflow::log(const QString &message) const
{
	std::fprintf(stderr, "[highlight-reflow] %s\n", message.toUtf8().constData());
}
