#pragma once

#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

class QThreadPool;

namespace qs::windows::sys {

class Brightness: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Brightness(QObject* parent = nullptr);
	~Brightness() override;
	Q_DISABLE_COPY_MOVE(Brightness);

	Q_INVOKABLE void query(const QString& screenName);

	Q_INVOKABLE void probe(const QString& screenName);

	Q_INVOKABLE void setBrightness(const QString& screenName, bool isDdc, qreal value);

signals:
	void queried(const QString& screenName, bool available, bool isDdc, qreal brightness);
	void brightnessSetFinished(const QString& screenName, bool ok);

private:
	QThreadPool* pool = nullptr;
	QHash<QString, int> screenRoute;
	int nextWmiInstanceIndex = 0;
	QHash<QString, bool> software;
};

} // namespace qs::windows::sys
