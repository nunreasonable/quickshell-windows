#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::windows::sys {

class Autostart: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged);

public:
	explicit Autostart(QObject* parent = nullptr): QObject(parent) { this->refresh(); }

	[[nodiscard]] QVariantList entries() const { return this->mEntries; }

	Q_INVOKABLE void refresh();
	Q_INVOKABLE void setEnabled(const QString& id, bool enabled);
	Q_INVOKABLE bool
	addFromPath(const QString& filePath, const QString& name, const QStringList& arguments);
	Q_INVOKABLE bool removeEntry(const QString& id);

signals:
	void entriesChanged();

private:
	QVariantList mEntries;
};

} // namespace qs::windows::sys
