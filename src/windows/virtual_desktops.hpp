#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qthread.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

struct IVirtualDesktopManager;

namespace qs::windows {

class RegistryWatcher: public QThread {
	Q_OBJECT;

public:
	explicit RegistryWatcher(QList<QString> subkeys, QObject* parent = nullptr);
	~RegistryWatcher() override;
	Q_DISABLE_COPY_MOVE(RegistryWatcher);

	void stop();

signals:
	void changed();

protected:
	void run() override;

private:
	QList<QString> subkeys;
	HANDLE stopEvent = nullptr;
};

class VirtualDesktops: public QObject {
	Q_OBJECT;

public:
	struct Desktop {
		GUID id {};
		QString name;
	};

	static VirtualDesktops* instance();

	[[nodiscard]] const QList<Desktop>& desktops() const { return this->mDesktops; }
	[[nodiscard]] qsizetype count() const { return this->mDesktops.length(); }
	[[nodiscard]] qsizetype currentIndex() const { return this->mCurrent; }
	[[nodiscard]] qsizetype indexOf(const GUID& id) const;
	[[nodiscard]] bool accessorLoaded() const { return this->accessor.loaded; }

	[[nodiscard]] GUID windowDesktopId(HWND hwnd) const;
	[[nodiscard]] qsizetype windowDesktopIndex(HWND hwnd) const;
	[[nodiscard]] bool isWindowOnCurrent(HWND hwnd) const;

	bool switchTo(qsizetype index);
	bool ensureCount(qsizetype count);
	bool removeDesktop(qsizetype index, qsizetype fallback);
	bool moveWindow(HWND hwnd, qsizetype index);
	bool pinWindow(HWND hwnd, bool pinned);
	[[nodiscard]] bool isWindowPinned(HWND hwnd) const;

	void refresh(bool rereadNames = false);
	void noteCloakChange();

	static QString guidToString(const GUID& guid);

signals:
	void desktopsChanged();
	void currentChanged();
	void accessorReady();

private:
	explicit VirtualDesktops();
	~VirtualDesktops() override;
	Q_DISABLE_COPY_MOVE(VirtualDesktops);

	[[nodiscard]] IVirtualDesktopManager* manager() const;
	void loadAccessor();
	void installListener();
	bool readRegistry(QList<GUID>& ids, GUID& current) const;
	[[nodiscard]] QString readDesktopName(const GUID& id) const;
	bool waitForCount(qsizetype count);
	void sendShortcut(WORD key, int times) const;

	template <typename R, typename... P, typename... A>
	R call(R fallback, R (*fn)(P...), A... args) const;
	void accessorFailed() const;

	static LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Desktop> mDesktops;
	qsizetype mCurrent = -1;
	GUID mCurrentId {};
	DWORD sessionId = 0;

	mutable IVirtualDesktopManager* mManager = nullptr;
	mutable bool managerTried = false;
	RegistryWatcher* watcher = nullptr;
	HWND listener = nullptr;
	QTimer cloakTimer;

	mutable ULONGLONG accessorRestartedAt = 0;

	mutable struct Accessor {
		HMODULE module = nullptr;
		bool loaded = false;
		int (*getCurrentDesktopNumber)() = nullptr;
		int (*getDesktopCount)() = nullptr;
		GUID (*getDesktopIdByNumber)(int) = nullptr;
		int (*getWindowDesktopNumber)(HWND) = nullptr;
		int (*goToDesktopNumber)(int) = nullptr;
		int (*moveWindowToDesktopNumber)(HWND, int) = nullptr;
		int (*isWindowOnCurrentVirtualDesktop)(HWND) = nullptr;
		int (*createDesktop)() = nullptr;
		int (*removeDesktop)(int, int) = nullptr;
		int (*registerPostMessageHook)(HWND, UINT) = nullptr;
		int (*unregisterPostMessageHook)(HWND) = nullptr;
		int (*isPinnedWindow)(HWND) = nullptr;
		int (*pinWindow)(HWND) = nullptr;
		int (*unPinWindow)(HWND) = nullptr;
		void (*restart)() = nullptr;
	} accessor;
};

} // namespace qs::windows
