#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class NightLight: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit NightLight(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE void enable(int colorTemperatureKelvin);
	Q_INVOKABLE void disable();
};

} // namespace qs::windows::sys
