#include "window_frame.hpp"

#include <algorithm>

#include <qmargins.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qvariant.h>
#include <qwindow.h>
#include <qpa/qplatformwindow_p.h>

namespace qs::windows::sys {

namespace {

constexpr auto HIDDEN_PROPERTY = "_qs_hiddenTitleBar";
constexpr auto WATCHED_PROPERTY = "_qs_titleBarWatched";

QMargins hiddenMargins(QWindow* window) {
	auto* hwnd = reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
	auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
	auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
	RECT frame {};
	if (!AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, GetDpiForWindow(hwnd))) return {};

	auto top = -frame.top;
	if (IsZoomed(hwnd)) top -= frame.bottom;
	return {0, -static_cast<int>(top), 0, 0};
}

bool applyMargins(QWindow* window, bool keepFrame = false) {
	auto* native = window->nativeInterface<QNativeInterface::Private::QWindowsWindow>();
	if (native == nullptr) return false;

	auto margins = window->property(HIDDEN_PROPERTY).toBool() ? hiddenMargins(window) : QMargins();
	if (native->customMargins() == margins) return true;

	auto* hwnd = reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
	RECT frame {};
	keepFrame = keepFrame && !IsZoomed(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &frame);
	native->setCustomMargins(margins);
	if (keepFrame) {
		SetWindowPos(
		    hwnd,
		    nullptr,
		    frame.left,
		    frame.top,
		    frame.right - frame.left,
		    frame.bottom - frame.top,
		    SWP_NOZORDER | SWP_NOACTIVATE
		);
	}
	return true;
}

void keepOnScreen(QWindow* window) {
	auto* hwnd = reinterpret_cast<HWND>(window->winId()); // NOLINT(performance-no-int-to-ptr)
	if (IsZoomed(hwnd) || IsIconic(hwnd)) return;

	MONITORINFO monitor {.cbSize = sizeof(MONITORINFO)};
	RECT frame {};
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
	if (!GetWindowRect(hwnd, &frame)) return;

	const auto& work = monitor.rcWork;
	auto width = std::min(frame.right - frame.left, work.right - work.left);
	auto height = std::min(frame.bottom - frame.top, work.bottom - work.top);
	auto left = std::clamp(frame.left, work.left, work.right - width);
	auto top = std::clamp(frame.top, work.top, work.bottom - height);
	if (left == frame.left && top == frame.top && width == frame.right - frame.left
	    && height == frame.bottom - frame.top)
	{
		return;
	}

	SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

WindowFrame::WindowFrame(QObject* parent): QObject(parent) {}

bool WindowFrame::hideTitleBar(QObject* object) {
	auto* window = qobject_cast<QWindow*>(object);
	if (window == nullptr) return false;

	window->setProperty(HIDDEN_PROPERTY, true);
	if (!window->property(WATCHED_PROPERTY).toBool()) {
		window->setProperty(WATCHED_PROPERTY, true);
		QObject::connect(window, &QWindow::windowStateChanged, window, [window] { applyMargins(window, true); });
		QObject::connect(window, &QWindow::screenChanged, window, [window] { applyMargins(window); });
	}

	window->winId();
	if (!applyMargins(window)) return false;
	keepOnScreen(window);
	return true;
}

bool WindowFrame::showTitleBar(QObject* object) {
	auto* window = qobject_cast<QWindow*>(object);
	if (window == nullptr) return false;

	window->setProperty(HIDDEN_PROPERTY, false);
	return applyMargins(window);
}

} // namespace qs::windows::sys
