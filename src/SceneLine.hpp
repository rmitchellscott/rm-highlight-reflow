#pragma once

#include <QList>
#include <QRectF>

struct LinePoint {
	float x;
	float y;
	unsigned short speed;
	unsigned short width;
	unsigned char direction;
	unsigned char pressure;
} __attribute__((packed));
static_assert(sizeof(LinePoint) == 0xe);

struct Line {
	int tool;
	int color;
	unsigned int rgba;
	QList<LinePoint> points;
	double maskScale;
	float thickness;
	QRectF bounds;
};
#ifdef __aarch64__
static_assert(sizeof(Line) == 0x58);
#else
static_assert(sizeof(Line) == 0x48);
#endif
