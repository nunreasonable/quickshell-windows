#include "window_manager.hpp"

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qtimer.h>

#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_decorations.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

constexpr qint64 WARP_TIMEOUT_MS = 2000;
constexpr int WARP_FALLBACK_MS = 500;

bool isNullGuid(const GUID& id) { return IsEqualGUID(id, GUID {}) != FALSE; }

} // namespace

WorkspaceBehavior* WorkspaceBehavior::instance() {
	static QPointer<WorkspaceBehavior> behavior; // NOLINT

	if (behavior.isNull()) {
		behavior = new WorkspaceBehavior(QCoreApplication::instance());
	}

	return behavior.data();
}

WorkspaceBehavior::WorkspaceBehavior(QObject* parent)
    : QObject(parent)
    , desktops(VirtualDesktops::instance()) {
	this->currentIndex = this->desktops->currentIndex();
	const auto& list = this->desktops->desktops();
	if (this->currentIndex >= 0 && this->currentIndex < list.length()) {
		this->currentId = list[this->currentIndex].id;
	}

	QObject::connect(
	    this->desktops,
	    &VirtualDesktops::currentChanged,
	    this,
	    &WorkspaceBehavior::onCurrentChanged
	);

	this->fallbackTimer.setSingleShot(true);
	this->fallbackTimer.setInterval(WARP_FALLBACK_MS);
	QObject::connect(&this->fallbackTimer, &QTimer::timeout, this, &WorkspaceBehavior::warpToTopmost);
}

void WorkspaceBehavior::setBackAndForth(bool enabled) {
	if (enabled == this->mBackAndForth) return;
	this->mBackAndForth = enabled;
	emit this->backAndForthChanged();
}

void WorkspaceBehavior::setWarpOnChange(qint32 mode) {
	mode = qBound(0, mode, 2);
	if (mode == this->mWarpOnChange) return;
	this->mWarpOnChange = mode;
	if (mode == 0) this->cancelWarp();
	emit this->warpOnChangeChanged();
}

void WorkspaceBehavior::onCurrentChanged() {
	auto index = this->desktops->currentIndex();
	const auto& list = this->desktops->desktops();
	auto id = index >= 0 && index < list.length() ? list[index].id : GUID {};

	auto same = isNullGuid(id) || isNullGuid(this->currentId) ? index == this->currentIndex
	                                                           : IsEqualGUID(id, this->currentId);

	if (!same && this->currentIndex >= 0) {
		this->previousId = this->currentId;
		this->previousIndex = this->currentIndex;
	}

	this->currentId = id;
	this->currentIndex = index;
	this->tryWarp();
}

qsizetype WorkspaceBehavior::previousDesktop() const {
	if (this->previousIndex < 0) return -1;
	if (!isNullGuid(this->previousId)) return this->desktops->indexOf(this->previousId);
	return this->previousIndex < this->desktops->count() ? this->previousIndex : -1;
}

void WorkspaceBehavior::watchTracker() {
	if (this->tracker != nullptr) return;
	this->tracker = WindowTracker::instance();

	auto retry = &WorkspaceBehavior::tryWarp;
	QObject::connect(this->tracker, &WindowTracker::activeWindowChanged, this, retry);
	QObject::connect(this->tracker, &WindowTracker::flushed, this, retry);
}

void WorkspaceBehavior::prepareWarp(qsizetype desktop, bool fromActive) {
	this->cancelWarp();
	if (this->mWarpOnChange == 0 || WorkspaceBehavior::cursorOverShell()) return;

	this->watchTracker();
	this->warpFrom = fromActive ? nullptr : this->tracker->activeWindow();
	this->warpDesktop = desktop;
	this->warpDeadline = QDateTime::currentMSecsSinceEpoch() + WARP_TIMEOUT_MS;
}

void WorkspaceBehavior::cancelWarp() {
	this->warpDesktop = -1;
	this->warpFrom = nullptr;
	this->fallbackTimer.stop();
}

void WorkspaceBehavior::tryWarp() {
	if (this->warpDesktop < 0 || this->tracker == nullptr) return;

	if (QDateTime::currentMSecsSinceEpoch() > this->warpDeadline) {
		this->cancelWarp();
		return;
	}

	if (this->desktops->currentIndex() != this->warpDesktop) return;

	auto* window = this->tracker->activeWindow();
	auto qualifies = window != nullptr && window != this->warpFrom.data() && !window->minimized()
	              && (window->desktop() == this->warpDesktop || window->desktop() == -1);

	if (!qualifies) {
		if (!this->fallbackTimer.isActive()) this->fallbackTimer.start();
		return;
	}

	this->cancelWarp();
	WorkspaceBehavior::warpCursor(window->hwnd());
}

void WorkspaceBehavior::warpToTopmost() {
	if (this->warpDesktop < 0 || this->tracker == nullptr) return;

	auto desktop = this->warpDesktop;
	this->cancelWarp();
	if (this->desktops->currentIndex() != desktop) return;

	struct Search {
		WindowTracker* tracker = nullptr;
		qsizetype desktop = -1;
		HWND found = nullptr;
	} search {.tracker = this->tracker, .desktop = desktop};

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param); // NOLINT(performance-no-int-to-ptr)
		    auto* window = search->tracker->windowFor(hwnd);
		    if (window == nullptr || window->minimized() || window->desktop() != search->desktop) {
			    return TRUE;
		    }

		    search->found = hwnd;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	if (search.found != nullptr) WorkspaceBehavior::warpCursor(search.found);
}

void WorkspaceBehavior::warpToActive() {
	if (this->mWarpOnChange == 0 || WorkspaceBehavior::cursorOverShell()) return;

	this->watchTracker();
	auto* window = this->tracker->activeWindow();
	if (window == nullptr || window->minimized()) return;

	auto desktop = window->desktop();
	if (desktop != -1 && desktop != this->desktops->currentIndex()) return;

	WorkspaceBehavior::warpCursor(window->hwnd());
}

bool WorkspaceBehavior::cursorOverShell() {
	POINT cursor {};
	if (!GetCursorPos(&cursor)) return false;

	auto* hwnd = WindowFromPoint(cursor);
	if (hwnd == nullptr) return false;

	auto* root = GetAncestor(hwnd, GA_ROOT);
	return isOwnProcessWindow(root != nullptr ? root : hwnd);
}

void WorkspaceBehavior::warpCursor(HWND hwnd) {
	RECT frame {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		if (!GetWindowRect(hwnd, &frame)) return;
	}

	POINT cursor {};
	if (GetCursorPos(&cursor) && PtInRect(&frame, cursor)) return;

	SetCursorPos((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
}

WindowManager::WindowManager(QObject* parent): QObject(parent) {
	auto* decorations = DecorationManager::instance();
	auto* behavior = WorkspaceBehavior::instance();

	// clang-format off
	QObject::connect(decorations, &DecorationManager::bordersChanged, this, &WindowManager::bordersChanged);
	QObject::connect(decorations, &DecorationManager::activeColorChanged, this, &WindowManager::activeBorderColorChanged);
	QObject::connect(decorations, &DecorationManager::inactiveColorChanged, this, &WindowManager::inactiveBorderColorChanged);
	QObject::connect(decorations, &DecorationManager::cornersChanged, this, &WindowManager::cornersChanged);
	QObject::connect(behavior, &WorkspaceBehavior::backAndForthChanged, this, &WindowManager::workspaceBackAndForthChanged);
	QObject::connect(behavior, &WorkspaceBehavior::warpOnChangeChanged, this, &WindowManager::warpOnChangeWorkspaceChanged);
	// clang-format on
}

bool WindowManager::decorationsSupported() { return DecorationManager::supported(); }

QString WindowManager::borders() const { return DecorationManager::instance()->borders(); }
void WindowManager::setBorders(const QString& borders) {
	DecorationManager::instance()->setBorders(borders);
}

QColor WindowManager::activeBorderColor() const { return DecorationManager::instance()->activeColor(); }
void WindowManager::setActiveBorderColor(const QColor& color) {
	DecorationManager::instance()->setActiveColor(color);
}

QColor WindowManager::inactiveBorderColor() const {
	return DecorationManager::instance()->inactiveColor();
}
void WindowManager::setInactiveBorderColor(const QColor& color) {
	DecorationManager::instance()->setInactiveColor(color);
}

QString WindowManager::corners() const { return DecorationManager::instance()->corners(); }
void WindowManager::setCorners(const QString& corners) {
	DecorationManager::instance()->setCorners(corners);
}

bool WindowManager::workspaceBackAndForth() const {
	return WorkspaceBehavior::instance()->backAndForth();
}
void WindowManager::setWorkspaceBackAndForth(bool enabled) {
	WorkspaceBehavior::instance()->setBackAndForth(enabled);
}

qint32 WindowManager::warpOnChangeWorkspace() const {
	return WorkspaceBehavior::instance()->warpOnChange();
}
void WindowManager::setWarpOnChangeWorkspace(qint32 mode) {
	WorkspaceBehavior::instance()->setWarpOnChange(mode);
}

} // namespace qs::windows
