#pragma once

#include <qt_windows.h>

#include <qcolor.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class TrackedWindow;
class WindowTracker;
class VirtualDesktops;

class WorkspaceBehavior: public QObject {
	Q_OBJECT;

public:
	static WorkspaceBehavior* instance();
	Q_DISABLE_COPY_MOVE(WorkspaceBehavior);

	[[nodiscard]] bool backAndForth() const { return this->mBackAndForth; }
	void setBackAndForth(bool enabled);
	[[nodiscard]] qint32 warpOnChange() const { return this->mWarpOnChange; }
	void setWarpOnChange(qint32 mode);

	[[nodiscard]] qsizetype previousDesktop() const;
	void prepareWarp(qsizetype desktop, bool fromActive = false);
	void warpToActive();

signals:
	void backAndForthChanged();
	void warpOnChangeChanged();

private:
	explicit WorkspaceBehavior(QObject* parent);
	~WorkspaceBehavior() override = default;

	void onCurrentChanged();
	void tryWarp();
	void cancelWarp();
	void warpToTopmost();
	void watchTracker();
	[[nodiscard]] static bool cursorOverShell();
	static void warpCursor(HWND hwnd);

	VirtualDesktops* desktops = nullptr;
	WindowTracker* tracker = nullptr;

	bool mBackAndForth = false;
	qint32 mWarpOnChange = 0;

	GUID currentId {};
	qsizetype currentIndex = -1;
	GUID previousId {};
	qsizetype previousIndex = -1;

	qsizetype warpDesktop = -1;
	qint64 warpDeadline = 0;
	QPointer<TrackedWindow> warpFrom;
	QTimer fallbackTimer;
};

class WindowManager: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool decorationsSupported READ decorationsSupported CONSTANT);
	Q_PROPERTY(QString borders READ borders WRITE setBorders NOTIFY bordersChanged);
	Q_PROPERTY(QColor activeBorderColor READ activeBorderColor WRITE setActiveBorderColor NOTIFY activeBorderColorChanged);
	Q_PROPERTY(QColor inactiveBorderColor READ inactiveBorderColor WRITE setInactiveBorderColor NOTIFY inactiveBorderColorChanged);
	Q_PROPERTY(QString corners READ corners WRITE setCorners NOTIFY cornersChanged);
	Q_PROPERTY(bool workspaceBackAndForth READ workspaceBackAndForth WRITE setWorkspaceBackAndForth NOTIFY workspaceBackAndForthChanged);
	Q_PROPERTY(qint32 warpOnChangeWorkspace READ warpOnChangeWorkspace WRITE setWarpOnChangeWorkspace NOTIFY warpOnChangeWorkspaceChanged);
	// clang-format on

public:
	explicit WindowManager(QObject* parent = nullptr);

	[[nodiscard]] static bool decorationsSupported();
	[[nodiscard]] QString borders() const;
	void setBorders(const QString& borders);
	[[nodiscard]] QColor activeBorderColor() const;
	void setActiveBorderColor(const QColor& color);
	[[nodiscard]] QColor inactiveBorderColor() const;
	void setInactiveBorderColor(const QColor& color);
	[[nodiscard]] QString corners() const;
	void setCorners(const QString& corners);
	[[nodiscard]] bool workspaceBackAndForth() const;
	void setWorkspaceBackAndForth(bool enabled);
	[[nodiscard]] qint32 warpOnChangeWorkspace() const;
	void setWarpOnChangeWorkspace(qint32 mode);

signals:
	void bordersChanged();
	void activeBorderColorChanged();
	void inactiveBorderColorChanged();
	void cornersChanged();
	void workspaceBackAndForthChanged();
	void warpOnChangeWorkspaceChanged();
};

} // namespace qs::windows
