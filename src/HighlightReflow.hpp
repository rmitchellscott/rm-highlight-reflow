#pragma once

#include "SceneLine.hpp"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVariantMap>

#include <string>
#include <utility>
#include <vector>

class HighlightReflow : public QObject
{
	Q_OBJECT
public:
	explicit HighlightReflow(const QString &stateDirectory, QObject *parent = nullptr);

	Q_INVOKABLE bool keepOldLayout(const QString &documentId, bool captureHighlightsNow);
	Q_INVOKABLE bool hasOldLayout(const QString &documentId) const;
	Q_INVOKABLE void discardOldLayout(const QString &documentId);
	Q_INVOKABLE bool isLivePage(const QString &documentId, const QString &pageId) const;
	Q_INVOKABLE void markApplyStarted(const QString &documentId);
	Q_INVOKABLE void planReflow(const QString &documentId);

	Q_INVOKABLE Line highlighterStroke(const QPointF &from, const QPointF &to, int color, quint32 rgba) const;
	Q_INVOKABLE Line eraseLasso(const QRectF &sceneRect) const;

	Q_INVOKABLE void log(const QString &message) const;
	Q_INVOKABLE void reportProblem(const QString &documentId, const QString &message);

signals:
	void planReady(const QString &documentId, const QVariantMap &plan);
	void planFailed(const QString &documentId, const QString &reason);
	void problemReported(const QString &documentId, const QString &message);

private:
	struct CapturedLayout {
		bool capturing = false;
		bool planWanted = false;
		bool hasAnchors = false;
		std::string anchors;
		quint64 generation = 0;
		QDateTime startedAt;
	};

	QString documentBase(const QString &documentId) const;
	QString oldLayoutDirectory(const QString &documentId) const;
	std::vector<std::pair<uint32_t, std::string>> highlightFiles(const QString &documentId, const QString &contentPath) const;
	void captureOldLayout(const QString &documentId);
	void startPlan(const QString &documentId);

	QString stateDirectory;
	QHash<QString, CapturedLayout> captured;
	quint64 nextCaptureGeneration = 1;
};
