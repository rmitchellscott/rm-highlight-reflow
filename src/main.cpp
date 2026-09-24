#include "HighlightReflow.hpp"

#include <QQmlApplicationEngine>

extern "C" void registerHighlightReflow()
{
	qmlRegisterSingletonInstance<HighlightReflow>("net.rmitchellscott.HighlightReflow", 1, 0, "HighlightReflow",
		new HighlightReflow(QStringLiteral("/home/root/.cache/highlight-reflow")));
}
