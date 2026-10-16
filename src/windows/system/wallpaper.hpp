#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class Wallpaper: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(bool lockScreenImageSupported READ lockScreenImageSupported NOTIFY lockScreenImageSupportedChanged);
	Q_PROPERTY(bool systemDarkMode READ systemDarkMode NOTIFY systemDarkModeChanged);

public:
	explicit Wallpaper(QObject* parent = nullptr);

	Q_INVOKABLE static QString currentWallpaper();

	Q_INVOKABLE static bool setWallpaper(const QString& path, bool animate = true);

	Q_INVOKABLE static bool isDarkMode();

	Q_INVOKABLE static void setDarkMode(bool dark);

	Q_INVOKABLE static bool setAccentColor(const QString& hex);

	Q_INVOKABLE static QString matugenPath();

	[[nodiscard]] bool lockScreenImageSupported() const { return this->mLockScreenImageSupported; }

	[[nodiscard]] bool systemDarkMode() const { return this->mSystemDarkMode; }

	Q_INVOKABLE void setLockScreenImage(const QString& path);

	Q_INVOKABLE static void openLockScreenPersonalizationSettings();

	Q_INVOKABLE static QString lockScreenMode();

signals:
	void lockScreenImageSupportedChanged();
	void lockScreenImageApplied(bool ok);
	void systemDarkModeChanged();

private:
	void applyLockScreenResult(bool ok, const QString& errorMessage);
	void refreshSystemDarkMode();

	bool mLockScreenImageSupported = true;
	bool mLockScreenAttemptRunning = false;
	bool mSystemDarkMode = false;
};

} // namespace qs::windows::sys
