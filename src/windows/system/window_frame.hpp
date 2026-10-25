#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class WindowFrame: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit WindowFrame(QObject* parent = nullptr);

	Q_INVOKABLE static bool hideTitleBar(QObject* window);
	Q_INVOKABLE static bool showTitleBar(QObject* window);
};

} // namespace qs::windows::sys
