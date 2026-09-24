#define _GNU_SOURCE
#include <stdio.h>

#include "xovi.h"

void registerHighlightReflow();

void _xovi_construct()
{
	Environment->requireExtension("qt-resource-rebuilder", 0, 2, 0);
	registerHighlightReflow();
	qt_resource_rebuilder$qmldiff_add_external_diff(r$highlightReflow, "Highlight reflow");
}
