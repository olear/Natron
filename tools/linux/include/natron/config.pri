boost: LIBS += -lboost_serialization
INCLUDEPATH+=/opt/Natron-CY2015/include
LIBS+="-L/opt/Natron-CY2015/lib"

LIBS += -lfontconfig -lfreetype -lexpat -lz

pyside {
PKGCONFIG -= pyside
INCLUDEPATH += $$system(pkg-config --variable=includedir pyside)
INCLUDEPATH += $$system(pkg-config --variable=includedir pyside)/QtCore
INCLUDEPATH += $$system(pkg-config --variable=includedir pyside)/QtGui
INCLUDEPATH += $$system(pkg-config --variable=includedir QtGui)
LIBS += -lpyside-python2.7
}
shiboken {
PKGCONFIG -= shiboken
INCLUDEPATH += $$system(pkg-config --variable=includedir shiboken)
LIBS += -lshiboken-python2.7
}

QMAKE_LFLAGS += -Wl,-rpath,\\\$\$ORIGIN/../lib
