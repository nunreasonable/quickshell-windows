#include "window_decorations.hpp"
#include <array>
#include <atomic>
#include <cstddef>

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qhash.h>
#include <qobject.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>

#include "util.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

constexpr DWORD WINDOWS_11_BUILD = 22000;
constexpr std::size_t CRASH_SLOTS = 512;

std::array<std::atomic<HWND>, CRASH_SLOTS> gSlots {}; // NOLINT
std::atomic<bool> gBordersOverridden = false;          // NOLINT
std::atomic<bool> gCornersOverridden = false;          // NOLINT

void rememberForCrash(HWND hwnd) {
	std::atomic<HWND>* free = nullptr;

	for (auto& slot: gSlots) {
		auto* value = slot.load();
		if (value == hwnd) return;
		if (value == nullptr && free == nullptr) free = &slot;
	}

	if (free != nullptr) free->store(hwnd);
}

void forgetForCrash(HWND hwnd) {
	for (auto& slot: gSlots) {
		if (slot.load() == hwnd) slot.store(nullptr);
	}
}

COLORREF colorRef(const QColor& color) {
	auto rgb = color.toRgb();
	return RGB(rgb.red(), rgb.green(), rgb.blue());
}

bool setBorder(HWND hwnd, COLORREF color) {
	return SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color)));
}

bool setCorner(HWND hwnd, DWORD preference) {
	auto value = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(preference);
	return SUCCEEDED(
	    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof(value))
	);
}

bool reachable(HWND hwnd) { return IsWindow(hwnd) && !IsHungAppWindow(hwnd); }

} // namespace

DecorationManager* DecorationManager::instance() {
	static QPointer<DecorationManager> manager; // NOLINT

	if (manager.isNull()) {
		manager = new DecorationManager(QCoreApplication::instance());
	}

	return manager.data();
}

DecorationManager::DecorationManager(QObject* parent): QObject(parent) {
	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
		this->restoreAll();
		this->stop();
	});
}

DecorationManager::~DecorationManager() { this->restoreAll(); }

bool DecorationManager::supported() { return windowsBuild() >= WINDOWS_11_BUILD; }

void DecorationManager::restoreForCrash() {
	auto borders = gBordersOverridden.load();
	auto corners = gCornersOverridden.load();
	if (!borders && !corners) return;

	for (auto& slot: gSlots) {
		auto* hwnd = slot.load();
		if (hwnd == nullptr || !reachable(hwnd)) continue;
		if (borders) setBorder(hwnd, DWMWA_COLOR_DEFAULT);
		if (corners) setCorner(hwnd, DWMWCP_DEFAULT);
	}
}

void DecorationManager::setBorders(const QString& borders) {
	auto value = borders.trimmed().toLower();
	if (value != QStringLiteral("colors") && value != QStringLiteral("none")) {
		value = QStringLiteral("system");
	}

	if (value == this->mBorders) return;
	this->mBorders = value;
	this->update();
	emit this->bordersChanged();
}

void DecorationManager::setActiveColor(const QColor& color) {
	if (color == this->mActiveColor) return;
	this->mActiveColor = color;
	if (this->borderMode() == BorderMode::Colors) this->update();
	emit this->activeColorChanged();
}

void DecorationManager::setInactiveColor(const QColor& color) {
	if (color == this->mInactiveColor) return;
	this->mInactiveColor = color;
	if (this->borderMode() == BorderMode::Colors) this->update();
	emit this->inactiveColorChanged();
}

void DecorationManager::setCorners(const QString& corners) {
	auto value = corners.trimmed();
	if (value != QStringLiteral("round") && value != QStringLiteral("roundSmall")
	    && value != QStringLiteral("square"))
	{
		value = QStringLiteral("default");
	}

	if (value == this->mCorners) return;
	this->mCorners = value;
	this->update();
	emit this->cornersChanged();
}

DecorationManager::BorderMode DecorationManager::borderMode() const {
	if (this->mBorders == QStringLiteral("colors")) return BorderMode::Colors;
	if (this->mBorders == QStringLiteral("none")) return BorderMode::None;
	return BorderMode::System;
}

DWORD DecorationManager::cornerPreference() const {
	if (this->mCorners == QStringLiteral("round")) return DWMWCP_ROUND;
	if (this->mCorners == QStringLiteral("roundSmall")) return DWMWCP_ROUNDSMALL;
	if (this->mCorners == QStringLiteral("square")) return DWMWCP_DONOTROUND;
	return DWMWCP_DEFAULT;
}

bool DecorationManager::overriding() const {
	if (!DecorationManager::supported()) return false;
	return this->borderMode() != BorderMode::System || this->cornerPreference() != DWMWCP_DEFAULT;
}

void DecorationManager::update() {
	auto supported = DecorationManager::supported();
	auto borders = supported && this->borderMode() != BorderMode::System;
	auto corners = supported && this->cornerPreference() != DWMWCP_DEFAULT;

	if (borders) gBordersOverridden.store(true);
	if (corners) gCornersOverridden.store(true);

	if (this->overriding()) {
		if (!this->running) this->start();
		this->applyAll();
	} else if (this->running) {
		this->applyAll();
		this->stop();
	}

	gBordersOverridden.store(borders);
	gCornersOverridden.store(corners);
}

void DecorationManager::start() {
	this->tracker = WindowTracker::instance();
	this->running = true;

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &DecorationManager::apply)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &DecorationManager::forget)
	);
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &DecorationManager::onActiveWindowChanged
	));

	auto* active = this->tracker->activeWindow();
	this->lastActive = active == nullptr ? nullptr : active->hwnd();
}

void DecorationManager::stop() {
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();
	this->restoreAll();
	this->lastActive = nullptr;
	this->running = false;
}

void DecorationManager::applyAll() {
	if (this->tracker == nullptr) return;

	QSet<HWND> tracked;
	for (auto* window: this->tracker->windows()) {
		tracked.insert(window->hwnd());
		this->apply(window);
	}

	for (auto iter = this->applied.begin(); iter != this->applied.end();) {
		if (tracked.contains(iter.key())) {
			++iter;
			continue;
		}

		if (reachable(iter.key())) {
			if (iter->border) setBorder(iter.key(), DWMWA_COLOR_DEFAULT);
			if (iter->corner) setCorner(iter.key(), DWMWCP_DEFAULT);
		}

		forgetForCrash(iter.key());
		iter = this->applied.erase(iter);
	}
}

void DecorationManager::apply(TrackedWindow* window) {
	if (window == nullptr || this->tracker == nullptr) return;

	auto* hwnd = window->hwnd();
	if (isOwnProcessWindow(hwnd) || !reachable(hwnd)) return;

	auto state = this->applied.value(hwnd);

	auto mode = this->borderMode();
	auto color = window == this->tracker->activeWindow() ? this->mActiveColor : this->mInactiveColor;
	if (mode == BorderMode::Colors && !color.isValid()) mode = BorderMode::System;

	if (mode != BorderMode::System) {
		auto value = mode == BorderMode::None ? DWMWA_COLOR_NONE : colorRef(color);
		if ((!state.border || state.color != value) && setBorder(hwnd, value)) {
			state.border = true;
			state.color = value;
		}
	} else if (state.border) {
		setBorder(hwnd, DWMWA_COLOR_DEFAULT);
		state.border = false;
	}

	auto preference = this->cornerPreference();
	if (preference != DWMWCP_DEFAULT) {
		if ((!state.corner || state.preference != preference) && setCorner(hwnd, preference)) {
			state.corner = true;
			state.preference = preference;
		}
	} else if (state.corner) {
		setCorner(hwnd, DWMWCP_DEFAULT);
		state.corner = false;
	}

	if (state.border || state.corner) {
		this->applied.insert(hwnd, state);
		rememberForCrash(hwnd);
	} else {
		this->applied.remove(hwnd);
		forgetForCrash(hwnd);
	}
}

void DecorationManager::forget(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (hwnd == this->lastActive) this->lastActive = nullptr;

	auto iter = this->applied.find(hwnd);
	if (iter == this->applied.end()) return;

	if (reachable(hwnd)) {
		if (iter->border) setBorder(hwnd, DWMWA_COLOR_DEFAULT);
		if (iter->corner) setCorner(hwnd, DWMWCP_DEFAULT);
	}

	forgetForCrash(hwnd);
	this->applied.erase(iter);
}

void DecorationManager::restoreAll() {
	for (auto iter = this->applied.cbegin(); iter != this->applied.cend(); ++iter) {
		if (reachable(iter.key())) {
			if (iter->border) setBorder(iter.key(), DWMWA_COLOR_DEFAULT);
			if (iter->corner) setCorner(iter.key(), DWMWCP_DEFAULT);
		}

		forgetForCrash(iter.key());
	}

	this->applied.clear();
}

void DecorationManager::onActiveWindowChanged() {
	auto* active = this->tracker->activeWindow();
	auto* hwnd = active == nullptr ? nullptr : active->hwnd();
	if (hwnd == this->lastActive) return;

	if (auto* previous = this->tracker->windowFor(this->lastActive)) this->apply(previous);
	this->lastActive = hwnd;
	if (active != nullptr) this->apply(active);
}

} // namespace qs::windows
