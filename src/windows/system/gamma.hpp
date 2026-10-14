#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qstring.h>
#include <qt_windows.h>

namespace qs::windows::sys {

class GammaController: public QObject {
	Q_OBJECT;

public:
	static GammaController* instance();

	void setTemperature(int kelvin);
	void setBrightness(const QString& screenName, qreal brightness);

	void restoreAll();

private:
	explicit GammaController(QObject* parent);

	struct Ramp {
		WORD values[3][256]; // NOLINT
	};

	bool apply(const QString& screenName, qreal brightness);
	qreal floorFor(const QString& screenName);

	int mKelvin = 0;
	QHash<QString, qreal> brightness;
	QHash<QString, qreal> floors;
	QHash<QString, Ramp> originals;
};

} // namespace qs::windows::sys
