#pragma once

#include <qt_windows.h>

#include <qcolor.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class TrackedWindow;
class WindowTracker;

class DecorationManager: public QObject {
	Q_OBJECT;

public:
	static DecorationManager* instance();
	~DecorationManager() override;
	Q_DISABLE_COPY_MOVE(DecorationManager);

	[[nodiscard]] static bool supported();
	static void restoreForCrash();

	[[nodiscard]] QString borders() const { return this->mBorders; }
	void setBorders(const QString& borders);
	[[nodiscard]] QColor activeColor() const { return this->mActiveColor; }
	void setActiveColor(const QColor& color);
	[[nodiscard]] QColor inactiveColor() const { return this->mInactiveColor; }
	void setInactiveColor(const QColor& color);
	[[nodiscard]] QString corners() const { return this->mCorners; }
	void setCorners(const QString& corners);

signals:
	void bordersChanged();
	void activeColorChanged();
	void inactiveColorChanged();
	void cornersChanged();

private:
	explicit DecorationManager(QObject* parent);

	enum class BorderMode : quint8 { System, Colors, None };

	struct Applied {
		bool border = false;
		bool corner = false;
		COLORREF color = 0;
		DWORD preference = 0;
	};

	[[nodiscard]] BorderMode borderMode() const;
	[[nodiscard]] DWORD cornerPreference() const;
	[[nodiscard]] bool overriding() const;

	void update();
	void start();
	void stop();
	void applyAll();
	void apply(TrackedWindow* window);
	void forget(TrackedWindow* window);
	void restoreAll();
	void onActiveWindowChanged();

	WindowTracker* tracker = nullptr;
	QList<QMetaObject::Connection> connections;
	QHash<HWND, Applied> applied;
	HWND lastActive = nullptr;
	bool running = false;

	QString mBorders = QStringLiteral("system");
	QColor mActiveColor;
	QColor mInactiveColor;
	QString mCorners = QStringLiteral("default");
};

} // namespace qs::windows
