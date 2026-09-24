TEMPLATE = lib
TARGET = highlight-reflow
CONFIG += shared plugin no_plugin_name_prefix
QMAKE_LFLAGS += -Wl,--no-undefined

OBJECTS_DIR = build/obj
MOC_DIR = build/moc
XOVI_DIR = build/xovi

xoviextension.target = build/xovi/xovi.c
xoviextension.commands = mkdir -p $$XOVI_DIR && python3 $$(XOVI_REPO)/util/xovigen.py -o $$XOVI_DIR/xovi.c -H $$XOVI_DIR/xovi.h highlight-reflow.xovi
xoviextension.depends = highlight-reflow.xovi qmd/highlight-reflow.qmd

QMAKE_EXTRA_TARGETS += xoviextension
PRE_TARGETDEPS += $$XOVI_DIR/xovi.c

QT += quick qml
CONFIG += c++20

SOURCES += \
    src/entry.c $$XOVI_DIR/xovi.c src/main.cpp src/HighlightReflow.cpp \
    src/core/HighlightReader.cpp src/core/EpubIndex.cpp src/core/PdfText.cpp src/core/Anchoring.cpp src/core/AnchorCodec.cpp
HEADERS += src/HighlightReflow.hpp src/SceneLine.hpp
INCLUDEPATH += $$XOVI_DIR src

LIBS += -ldl
QMAKE_CXXFLAGS += -fPIC -Werror -Wno-invalid-offsetof
