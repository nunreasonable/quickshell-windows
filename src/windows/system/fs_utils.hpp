#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class FsUtils: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit FsUtils(QObject* parent = nullptr): QObject(parent) {}

	Q_INVOKABLE static QString classify(const QString& path);

	Q_INVOKABLE static bool isAccessibleDir(const QString& path);

	Q_INVOKABLE static QStringList listDir(const QString& path);

	Q_INVOKABLE static QString findExecutable(const QString& name);

	Q_INVOKABLE static bool makePath(const QString& path);

	Q_INVOKABLE static bool removeFile(const QString& path);

	Q_INVOKABLE static QString canonicalPath(const QString& path);
};

} // namespace qs::windows::sys
