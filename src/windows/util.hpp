#pragma once

#include <qt_windows.h>

#include <qpoint.h>
#include <qrect.h>
#include <qscreen.h>
#include <qtypes.h>
#include <qwindow.h>

namespace qs::windows {

struct MonitorRects {
	QRect monitor;
	QRect work;
	bool valid = false;
};

[[nodiscard]] HMONITOR monitorForScreen(QScreen* screen);
[[nodiscard]] QScreen* screenForMonitor(HMONITOR monitor);
[[nodiscard]] QScreen* focusedScreen();
void setFocusedScreen(QScreen* screen);
void prioritizeInputThread(int priority);
void optOutOfProcessPowerThrottling();
[[nodiscard]] MonitorRects monitorRects(HMONITOR monitor);

class ScreenMapper {
public:
	ScreenMapper(const QRect& logicalScreen, const QRect& physicalMonitor, qreal dpr)
	    : logicalOrigin(logicalScreen.topLeft())
	    , physicalOrigin(physicalMonitor.topLeft())
	    , dpr(dpr) {}

	[[nodiscard]] QPoint toLogical(const QPoint& physical) const;
	[[nodiscard]] QRect toLogical(const QRect& physical) const;
	[[nodiscard]] QPoint toPhysical(const QPoint& logical) const;
	[[nodiscard]] QRect toPhysical(const QRect& logical) const;
	[[nodiscard]] qint32 toPhysical(qint32 logicalLength) const;

private:
	QPoint logicalOrigin;
	QPoint physicalOrigin;
	qreal dpr;
};

[[nodiscard]] HWND hwndOf(const QWindow* window);
[[nodiscard]] QRect toQRect(const RECT& rect);
[[nodiscard]] RECT toRECT(const QRect& rect);

bool forceForegroundWindow(HWND hwnd);

DWORD windowsBuild();

[[nodiscard]] bool isOwnProcessWindow(HWND hwnd);
[[nodiscard]] bool isMoreElevated(HWND hwnd);

inline constexpr const wchar_t* TRAY_HOOK_PROP = L"QuickshellTrayHook";
[[nodiscard]] bool isTrayHookWindow(HWND hwnd);

[[nodiscard]] HWND explorerTaskbarWindow();

class TrayHookYield {
public:
	TrayHookYield();
	~TrayHookYield();
	TrayHookYield(const TrayHookYield&) = delete;
	TrayHookYield& operator=(const TrayHookYield&) = delete;
	TrayHookYield(TrayHookYield&&) = delete;
	TrayHookYield& operator=(TrayHookYield&&) = delete;
};

[[nodiscard]] bool trayHookYielding();

void markNonRude(HWND hwnd);

void applyPanelDwmAttributes(HWND hwnd);

void setExStyleBits(HWND hwnd, LONG_PTR bits, bool enabled);

} // namespace qs::windows
