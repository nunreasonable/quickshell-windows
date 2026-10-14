#include "tiling.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <qdatetime.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>

#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logTiling, "quickshell.windows.tiling", QtWarningMsg);

using tiling::Edge;

constexpr int SYNC_DELAY_MS = 30;
constexpr int VERIFY_DELAY_MS = 150;
constexpr int DEADLINE_MS = 2000;
constexpr qint32 MAX_CORRECTIONS = 2;
constexpr qint64 CORRECTION_RESET_MS = 1500;
constexpr int EDGE_TOLERANCE = 2;

TilingManager* gManager = nullptr; // NOLINT

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

QString windowClass(HWND hwnd) {
	wchar_t buffer[128] {};
	auto length = GetClassNameW(hwnd, buffer, 128);
	return QString::fromWCharArray(buffer, length);
}

bool readFrame(HWND hwnd, QRect& raw, QRect& frame) {
	RECT rawRect {};
	if (!GetWindowRect(hwnd, &rawRect)) return false;

	RECT frameRect {};
	auto hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
	if (FAILED(hr)) frameRect = rawRect;

	raw = toQRect(rawRect);
	frame = toQRect(frameRect);
	return true;
}

QRect currentFrame(HWND hwnd) {
	QRect raw;
	QRect frame;
	readFrame(hwnd, raw, frame);
	return frame;
}

void setFrame(HWND hwnd, const QRect& target) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	auto left = frame.left() - raw.left();
	auto top = frame.top() - raw.top();
	auto right = rightOf(raw) - rightOf(frame);
	auto bottom = bottomOf(raw) - bottomOf(frame);

	SetWindowPos(
	    hwnd,
	    nullptr,
	    target.left() - left,
	    target.top() - top,
	    target.width() + left + right,
	    target.height() + top + bottom,
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

void setRawRect(HWND hwnd, const QRect& rect) {
	SetWindowPos(
	    hwnd,
	    nullptr,
	    rect.left(),
	    rect.top(),
	    rect.width(),
	    rect.height(),
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

QPoint cursorPos() {
	POINT point {};
	GetCursorPos(&point);
	return {point.x, point.y};
}

QRect physicalMonitor(QScreen* screen) { return monitorRects(monitorForScreen(screen)).monitor; }

qint32 toPhysical(qint32 logical, qreal dpr) {
	return static_cast<qint32>(std::lround(logical * dpr));
}

} // namespace

TilingManager* TilingManager::instance() {
	static auto* instance = new TilingManager(); // NOLINT
	return instance;
}

TilingManager* TilingManager::active() {
	return gManager != nullptr && gManager->mEnabled ? gManager : nullptr;
}

TilingManager::TilingManager() {
	gManager = this;

	this->syncTimer.setSingleShot(true);
	this->syncTimer.setInterval(SYNC_DELAY_MS);
	QObject::connect(&this->syncTimer, &QTimer::timeout, this, &TilingManager::sync);

	this->verifyTimer.setSingleShot(true);
	this->verifyTimer.setInterval(VERIFY_DELAY_MS);
	QObject::connect(&this->verifyTimer, &QTimer::timeout, this, &TilingManager::verify);

	this->deadlineTimer.setSingleShot(true);
	this->deadlineTimer.setInterval(DEADLINE_MS);
	QObject::connect(&this->deadlineTimer, &QTimer::timeout, this, &TilingManager::onDeadline);
}

void TilingManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	if (enabled) {
		if (!this->start()) {
			this->mEnabled = false;
			return;
		}
	} else {
		this->stop();
	}

	emit this->enabledChanged();
}

void TilingManager::setGapsIn(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsIn) return;
	this->mGapsIn = gaps;
	this->markAll();
	emit this->gapsInChanged();
}

void TilingManager::setGapsOut(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsOut) return;
	this->mGapsOut = gaps;
	this->markAll();
	emit this->gapsOutChanged();
}

void TilingManager::setPreserveSplit(bool preserve) {
	if (preserve == this->mPreserveSplit) return;
	this->mPreserveSplit = preserve;
	this->markAll();
	emit this->preserveSplitChanged();
}

void TilingManager::setExcluded(const QStringList& excluded) {
	if (excluded == this->mExcluded) return;
	this->mExcluded = excluded;

	this->excludedPatterns.clear();
	this->excludedTitles.clear();

	for (const auto& entry: excluded) {
		auto pattern = entry.trimmed();
		auto byTitle = pattern.startsWith("title:", Qt::CaseInsensitive);
		if (byTitle) pattern = pattern.mid(6).trimmed();
		if (pattern.isEmpty()) continue;

		auto expression = QRegularExpression(
		    QRegularExpression::wildcardToRegularExpression(
		        pattern,
		        QRegularExpression::NonPathWildcardConversion
		    ),
		    QRegularExpression::CaseInsensitiveOption
		);

		if (byTitle) this->excludedTitles.append(expression);
		else this->excludedPatterns.append(expression);
	}

	this->markAll();
	emit this->excludedChanged();
}

void TilingManager::setLayout(const QString& layout) {
	auto master = layout.trimmed().compare(QStringLiteral("master"), Qt::CaseInsensitive) == 0;
	auto normalized = master ? QStringLiteral("master") : QStringLiteral("dwindle");
	if (normalized == this->mLayout) return;
	this->mLayout = normalized;
	if (this->mEnabled) this->rebuildLayouts();
	emit this->layoutChanged();
}

void TilingManager::setSmartSplit(bool smart) {
	if (smart == this->mSmartSplit) return;
	this->mSmartSplit = smart;
	this->markAll();
	emit this->smartSplitChanged();
}

void TilingManager::setForceSplit(qint32 force) {
	force = qBound(0, force, 2);
	if (force == this->mForceSplit) return;
	this->mForceSplit = force;
	emit this->forceSplitChanged();
}

void TilingManager::setDefaultSplitRatio(double ratio) {
	ratio = qBound(0.1, ratio, 1.9);
	if (qFuzzyCompare(ratio, this->mDefaultSplitRatio)) return;
	this->mDefaultSplitRatio = ratio;
	emit this->defaultSplitRatioChanged();
}

void TilingManager::setMasterNewStatus(const QString& status) {
	auto value = status.trimmed().toLower();
	if (value != QStringLiteral("master") && value != QStringLiteral("inherit")) {
		value = QStringLiteral("slave");
	}

	if (value == this->mMasterNewStatus) return;
	this->mMasterNewStatus = value;
	this->markAll();
	emit this->masterNewStatusChanged();
}

void TilingManager::setMasterNewOnTop(bool onTop) {
	if (onTop == this->mMasterNewOnTop) return;
	this->mMasterNewOnTop = onTop;
	this->markAll();
	emit this->masterNewOnTopChanged();
}

void TilingManager::setMasterOrientation(const QString& orientation) {
	static const auto known = QStringList {
	    QStringLiteral("left"),
	    QStringLiteral("right"),
	    QStringLiteral("top"),
	    QStringLiteral("bottom"),
	    QStringLiteral("center"),
	};

	auto value = orientation.trimmed().toLower();
	if (!known.contains(value)) value = QStringLiteral("left");
	if (value == this->mMasterOrientation) return;
	this->mMasterOrientation = value;
	this->markAll();
	emit this->masterOrientationChanged();
}

void TilingManager::setMasterFactor(double factor) {
	factor = qBound(0.05, factor, 0.95);
	if (qFuzzyCompare(factor, this->mMasterFactor)) return;
	this->mMasterFactor = factor;
	this->markAll();
	emit this->masterFactorChanged();
}

tiling::Kind TilingManager::layoutKind() const {
	return this->mLayout == QStringLiteral("master") ? tiling::Kind::Master : tiling::Kind::Dwindle;
}

tiling::Options TilingManager::options() const {
	auto options = tiling::Options();
	options.preserveSplit = this->mPreserveSplit;
	options.smartSplit = this->mSmartSplit;
	options.forceSplit = this->mForceSplit;
	options.defaultSplitRatio = this->mDefaultSplitRatio;
	options.newOnTop = this->mMasterNewOnTop;
	options.mfact = this->mMasterFactor;

	if (this->mMasterNewStatus == QStringLiteral("master")) {
		options.newStatus = tiling::NewStatus::Master;
	} else if (this->mMasterNewStatus == QStringLiteral("inherit")) {
		options.newStatus = tiling::NewStatus::Inherit;
	}

	const auto& o = this->mMasterOrientation;
	if (o == QStringLiteral("right")) options.orientation = tiling::Orientation::Right;
	else if (o == QStringLiteral("top")) options.orientation = tiling::Orientation::Top;
	else if (o == QStringLiteral("bottom")) options.orientation = tiling::Orientation::Bottom;
	else if (o == QStringLiteral("center")) options.orientation = tiling::Orientation::Center;

	return options;
}

void TilingManager::rebuildLayouts() {
	auto kind = this->layoutKind();
	auto options = this->options();

	auto seed = options;
	seed.smartSplit = false;
	seed.forceSplit = 0;
	seed.newStatus = tiling::NewStatus::Slave;
	seed.newOnTop = false;

	for (auto& [key, layout]: this->layouts) {
		if (layout->tree->kind() == kind) continue;

		auto fresh = tiling::LayoutEngine::create(kind);
		fresh->configure(seed);
		auto area = layout->tree->area();
		if (area.isValid()) fresh->compute(area);

		auto previous = tiling::LayoutEngine::Id(0);
		for (auto id: layout->tree->ids()) {
			fresh->insert(id, previous, nullptr);
			previous = id;
		}

		fresh->configure(options);
		layout->tree = std::move(fresh);
	}

	this->edgeDragState = EdgeDrag();
	this->relayout();
}

void TilingManager::relayout() {
	if (!this->mEnabled) return;

	for (auto& [window, m]: this->managed) {
		m.target = QRect();
		m.accepted = QRect();
		m.corrections = 0;
	}

	this->markAll();
}

void TilingManager::markAll() {
	if (!this->mEnabled) return;
	for (auto& [window, m]: this->managed) this->dirtyWindows.insert(window);
	for (auto& [key, layout]: this->layouts) this->dirtyLayouts.insert(layout.get());
	this->schedule();
}

bool TilingManager::isTiled(TrackedWindow* window) const {
	auto iter = this->managed.find(window);
	return iter != this->managed.end() && iter->second.layout != nullptr;
}

bool TilingManager::start() {
	this->ownerMutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickshellTiling");
	if (this->ownerMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
		qCWarning(logTiling) << "Another Quickshell process already tiles windows;"
		                     << "tiling stays off in this one.";
		if (this->ownerMutex != nullptr) CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return false;
	}

	this->tracker = WindowTracker::instance();
	this->desktops = this->tracker->desktops();

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &TilingManager::manage)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &TilingManager::unmanage)
	);
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &TilingManager::onActiveWindowChanged
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeStarted,
	    this,
	    &TilingManager::onMoveSizeStarted
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeEnded,
	    this,
	    &TilingManager::onMoveSizeEnded
	));
	c.append(QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, [this]() {
		this->markAll();
	}));

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		auto changed = &TilingManager::onScreensChanged;
		c.append(QObject::connect(app, &QGuiApplication::screenAdded, this, changed));
		c.append(QObject::connect(app, &QGuiApplication::screenRemoved, this, changed));
	}

	this->onScreensChanged();

	for (auto* window: this->tracker->windows()) this->manage(window);

	auto windows = this->tracker->windows();
	std::stable_sort(windows.begin(), windows.end(), [](TrackedWindow* a, TrackedWindow* b) {
		auto ca = a->rect().center();
		auto cb = b->rect().center();
		return ca.x() != cb.x() ? ca.x() < cb.x() : ca.y() < cb.y();
	});

	QHash<Layout*, TrackedWindow*> previous;
	for (auto* window: windows) {
		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto* layout = this->layoutFor(window, true);
		if (layout == nullptr) continue;

		this->insertInto(m, layout, previous.value(layout), nullptr);
		previous.insert(layout, window);
	}

	this->onActiveWindowChanged();
	this->schedule();
	return true;
}

void TilingManager::stop() {
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();

	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
	}

	QList<TrackedWindow*> tiled;
	for (auto& [window, m]: this->managed) {
		QObject::disconnect(window, nullptr, this, nullptr);
		if (m.layout == nullptr) continue;

		tiled.append(window);
		this->restoreFloating(m);
	}

	this->managed.clear();
	this->layouts.clear();
	this->dirtyWindows.clear();
	this->dirtyLayouts.clear();
	this->unverified.clear();
	this->dragging = nullptr;
	this->edgeDragState = EdgeDrag();
	this->syncTimer.stop();
	this->verifyTimer.stop();
	this->deadlineTimer.stop();

	if (this->ownerMutex != nullptr) {
		ReleaseMutex(this->ownerMutex);
		CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
	}

	for (auto* window: tiled) emit this->windowTilingChanged(window);
}

void TilingManager::manage(TrackedWindow* window) {
	if (this->managed.contains(window)) return;

	auto& m = this->managed[window];
	m.window = window;
	m.elevated = isMoreElevated(window->hwnd());

	if (m.elevated) {
		qCDebug(logTiling) << "Window" << window->addressHex() << window->appId()
		                   << "belongs to a more privileged process; it floats.";
	}

	auto mark = [this, window]() { this->markWindow(window); };
	QObject::connect(window, &TrackedWindow::minimizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::maximizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::fullscreenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::desktopChanged, this, mark);
	QObject::connect(window, &TrackedWindow::screenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window]() {
		if (!this->excludedTitles.isEmpty()) this->markWindow(window);
	});
	QObject::connect(window, &TrackedWindow::rectChanged, this, [this, window]() {
		this->onRectChanged(window);
	});

	this->markWindow(window);
}

void TilingManager::unmanage(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	QObject::disconnect(window, nullptr, this, nullptr);
	this->takeOut(iter->second);
	this->managed.erase(iter);

	this->dirtyWindows.remove(window);
	this->unverified.remove(window);
	if (this->dragging == window) this->dragging = nullptr;

	for (auto& [key, layout]: this->layouts) {
		if (layout->lastFocused == window) layout->lastFocused = nullptr;
	}
}

TilingManager::State TilingManager::stateOf(const Managed& m) const {
	auto* window = m.window;

	if (window->desktop() < 0 || window->screen() == nullptr) return State::Out;
	if (!QGuiApplication::screens().contains(window->screen())) return State::Out;
	if (m.elevated) return State::Out;
	if (m.override == Override::Float) return State::Out;
	if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;

	if (window->minimized()) return State::Hidden;
	if (window->maximized()) return State::Suspended;

	if (window->fullscreen()) {
		if (m.layout == nullptr || m.target.isNull() || currentFrame(window->hwnd()) != m.target) {
			return State::Suspended;
		}
	}

	return State::Tiled;
}

bool TilingManager::floatsByRule(const Managed& m) const {
	auto* hwnd = m.window->hwnd();

	auto* owner = GetWindow(hwnd, GW_OWNER);
	if (owner != nullptr && IsWindowVisible(owner)) return true;

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	constexpr auto FLOATING_EX =
	    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_DLGMODALFRAME;
	if ((exStyle & FLOATING_EX) != 0) return true;

	if ((style & WS_THICKFRAME) == 0) return true;

	if (windowClass(hwnd) == QStringLiteral("#32770")) return true;

	return this->isExcluded(m.window);
}

bool TilingManager::isExcluded(TrackedWindow* window) const {
	for (const auto& pattern: this->excludedTitles) {
		if (pattern.match(window->title()).hasMatch()) return true;
	}

	if (this->excludedPatterns.isEmpty()) return false;

	auto exe = QFileInfo(window->exePath()).fileName();
	auto base = exe;
	if (base.endsWith(".exe", Qt::CaseInsensitive)) base.chop(4);

	auto candidates = QStringList {window->appId(), exe, base, windowClass(window->hwnd())};

	for (const auto& pattern: this->excludedPatterns) {
		for (const auto& candidate: candidates) {
			if (!candidate.isEmpty() && pattern.match(candidate).hasMatch()) return true;
		}
	}

	return false;
}

tiling::LayoutEngine::Id TilingManager::idOf(TrackedWindow* window) {
	return reinterpret_cast<tiling::LayoutEngine::Id>(window);
}

TrackedWindow* TilingManager::windowOf(tiling::LayoutEngine::Id id) const {
	auto* window = reinterpret_cast<TrackedWindow*>(id); // NOLINT(performance-no-int-to-ptr)
	return this->managed.contains(window) ? window : nullptr;
}

TilingManager::Layout* TilingManager::layoutFor(TrackedWindow* window, bool create) {
	auto index = window->desktop();
	if (index < 0 || index >= this->desktops->count()) return nullptr;
	return this->layoutAt(this->desktops->desktops().at(index).id, window->screen(), create);
}

TilingManager::Layout* TilingManager::layoutAt(const GUID& desktop, QScreen* screen, bool create) {
	if (screen == nullptr || !QGuiApplication::screens().contains(screen)) return nullptr;

	auto key = VirtualDesktops::guidToString(desktop) + '|' + screen->name();
	auto iter = this->layouts.find(key);
	if (iter != this->layouts.end()) return iter->second.get();
	if (!create) return nullptr;

	auto layout = std::make_unique<Layout>();
	layout->desktop = desktop;
	layout->screen = screen;
	layout->tree = tiling::LayoutEngine::create(this->layoutKind());
	layout->tree->configure(this->options());
	auto rects = monitorRects(monitorForScreen(screen));
	if (rects.valid) layout->tree->compute(rects.work);

	auto* raw = layout.get();
	this->layouts.emplace(key, std::move(layout));
	return raw;
}

bool TilingManager::layoutAlive(const Layout* layout) const {
	if (layout->screen == nullptr) return false;
	if (!QGuiApplication::screens().contains(layout->screen.data())) return false;
	return this->desktops->indexOf(layout->desktop) != -1;
}

void TilingManager::markWindow(TrackedWindow* window) {
	if (!this->mEnabled) return;
	this->dirtyWindows.insert(window);
	this->schedule();
}

void TilingManager::markLayout(Layout* layout) {
	if (!this->mEnabled || layout == nullptr) return;
	this->dirtyLayouts.insert(layout);
	this->schedule();
}

void TilingManager::schedule() {
	if (!this->syncTimer.isActive()) this->syncTimer.start();
}

void TilingManager::sync() {
	if (!this->mEnabled) return;

	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		auto* layout = iter->second.get();
		if (this->layoutAlive(layout)) {
			++iter;
			continue;
		}

		for (auto id: layout->tree->ids()) {
			if (auto* window = this->windowOf(id)) {
				auto& m = this->managed.at(window);
				m.layout = nullptr;
				m.target = QRect();
				this->dirtyWindows.insert(window);
			}
		}

		this->dirtyLayouts.remove(layout);
		iter = this->layouts.erase(iter);
	}

	auto windows = std::move(this->dirtyWindows);
	this->dirtyWindows.clear();

	for (auto* window: this->tracker->windows()) {
		if (windows.contains(window)) this->syncWindow(window);
	}

	this->dropEmptyLayouts();

	auto layouts = std::move(this->dirtyLayouts);
	this->dirtyLayouts.clear();

	for (auto& [key, layout]: this->layouts) {
		if (layouts.contains(layout.get())) this->apply(layout.get());
	}
}

void TilingManager::syncWindow(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	if (window == this->dragging) return;

	auto state = this->stateOf(m);

	if (state == State::Out) {
		if (m.layout == nullptr) return;

		auto floated = !window->minimized() && window->desktop() >= 0 && !m.elevated;
		this->takeOut(m);
		if (floated) this->restoreFloating(m);
		return;
	}

	auto index = window->desktop();
	if (index >= this->desktops->count()) return;
	auto desktop = this->desktops->desktops().at(index).id;

	Layout* want = nullptr;
	auto* current = m.layout;
	if (current != nullptr && this->layoutAlive(current) && IsEqualGUID(current->desktop, desktop)) {
		want = current;

		if (state == State::Tiled && !m.verifying && window->screen() != current->screen) {
			this->moveToLayout(m, this->layoutFor(window, true), nullptr);
			return;
		}
	} else if (state == State::Tiled) {
		want = this->layoutFor(window, true);
	}

	if (want == m.layout) {
		if (m.parked && state == State::Tiled) m.target = QRect();
		m.parked = state != State::Tiled;
		this->markLayout(m.layout);
		return;
	}

	if (m.layout != nullptr) this->takeOut(m);

	if (want != nullptr) {
		auto cursor = cursorPos();
		this->insertInto(m, want, nullptr, &cursor);
	}
}

void TilingManager::insertInto(
    Managed& m,
    Layout* layout,
    TrackedWindow* target,
    const QPoint* cursor
) {
	auto* window = m.window;

	if (m.preTiling.isNull()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) m.preTiling = raw;
	}

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	auto targetId = target == nullptr ? tiling::LayoutEngine::Id(0) : idOf(target);

	layout->tree->configure(this->options());
	layout->tree->insert(idOf(window), targetId, cursor);
	m.layout = layout;
	m.parked = false;
	m.target = QRect();
	m.accepted = QRect();
	m.corrections = 0;

	qCDebug(logTiling) << "Tiling" << window->addressHex() << window->appId() << "on"
	                   << VirtualDesktops::guidToString(layout->desktop) << layout->screen->name();

	this->markLayout(layout);
	emit this->windowTilingChanged(window);
}

void TilingManager::takeOut(Managed& m) {
	auto* layout = m.layout;
	if (layout == nullptr) return;

	layout->tree->remove(idOf(m.window));
	if (layout->lastFocused == m.window) layout->lastFocused = nullptr;

	m.layout = nullptr;
	m.target = QRect();
	m.accepted = QRect();
	m.verifying = false;
	this->unverified.remove(m.window);

	this->markLayout(layout);
	emit this->windowTilingChanged(m.window);
}

void TilingManager::dropEmptyLayouts() {
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		if (iter->second->tree->isEmpty()) {
			this->dirtyLayouts.remove(iter->second.get());
			iter = this->layouts.erase(iter);
		} else {
			++iter;
		}
	}
}

void TilingManager::apply(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (!rects.valid) return;

	layout->tree->configure(this->options());
	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		auto hidden = window != nullptr && this->stateOf(this->managed.at(window)) == State::Hidden;
		layout->tree->setHidden(id, hidden);
	}

	layout->tree->compute(rects.work);

	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr || window == this->dragging) continue;

		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, this->tileRect(layout, window));
	}
}

QRect TilingManager::tileRect(const Layout* layout, TrackedWindow* window) const {
	auto box = layout->tree->box(idOf(window));
	auto area = layout->tree->area();
	auto dpr = layout->screen->devicePixelRatio();
	auto in = toPhysical(this->mGapsIn, dpr);
	auto out = toPhysical(this->mGapsOut, dpr);

	auto left = box.left() == area.left() ? out : in;
	auto top = box.top() == area.top() ? out : in;
	auto right = rightOf(box) == rightOf(area) ? out : in;
	auto bottom = bottomOf(box) == bottomOf(area) ? out : in;

	return QRect(
	    box.left() + left,
	    box.top() + top,
	    qMax(1, box.width() - left - right),
	    qMax(1, box.height() - top - bottom)
	);
}

void TilingManager::place(Managed& m, const QRect& tile) {
	auto frame = currentFrame(m.window->hwnd());

	if (tile != m.target) {
		m.target = tile;
		m.accepted = QRect();
		m.corrections = 0;

		if (frame == tile) m.verifying = false;
		else this->send(m, frame);
		return;
	}

	if (frame == tile) {
		m.verifying = false;
		return;
	}

	if (!m.accepted.isNull() && frame == m.accepted) return;
	if (m.verifying && frame == m.before) return;

	auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - m.lastCorrection > CORRECTION_RESET_MS) m.corrections = 0;

	if (m.corrections >= MAX_CORRECTIONS) {
		this->giveUp(m, frame);
		return;
	}

	m.corrections++;
	m.lastCorrection = now;
	this->send(m, frame);
}

void TilingManager::send(Managed& m, const QRect& frame) {
	m.before = frame;
	m.verifying = true;
	m.deadline = QDateTime::currentMSecsSinceEpoch() + DEADLINE_MS;
	setFrame(m.window->hwnd(), m.target);
	this->armDeadline();
}

void TilingManager::armDeadline() {
	auto next = std::numeric_limits<qint64>::max();
	for (auto& [window, m]: this->managed) {
		if (m.verifying) next = qMin(next, m.deadline);
	}

	if (next == std::numeric_limits<qint64>::max()) {
		this->deadlineTimer.stop();
		return;
	}

	auto remaining = next - QDateTime::currentMSecsSinceEpoch();
	remaining = qBound(qint64(0), remaining, qint64(DEADLINE_MS));
	this->deadlineTimer.start(static_cast<int>(remaining));
}

void TilingManager::restoreFloating(Managed& m) {
	auto* hwnd = m.window->hwnd();
	auto previous = m.preTiling;
	m.preTiling = QRect();

	if (previous.isNull()) return;

	if (IsIconic(hwnd) || IsZoomed(hwnd)) {
		WINDOWPLACEMENT placement {};
		placement.length = sizeof(placement);
		if (GetWindowPlacement(hwnd, &placement)) {
			placement.rcNormalPosition = toRECT(previous);
			SetWindowPlacement(hwnd, &placement);
		}
		return;
	}

	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	auto rect = previous;
	auto previousRect = toRECT(previous);
	auto* then = MonitorFromRect(&previousRect, MONITOR_DEFAULTTONEAREST);
	auto* now = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

	if (then != now) {
		auto work = monitorRects(now).work;
		rect.moveCenter(raw.center());
		if (work.isValid()) {
			rect.setSize(rect.size().boundedTo(work.size()));
			rect.moveLeft(qBound(work.left(), rect.left(), rightOf(work) - rect.width()));
			rect.moveTop(qBound(work.top(), rect.top(), bottomOf(work) - rect.height()));
		}
	}

	setRawRect(hwnd, rect);
}

void TilingManager::onScreensChanged() {
	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
		auto relayoutAll = [this]() {
			for (auto& [key, layout]: this->layouts) this->markLayout(layout.get());
		};
		QObject::connect(screen, &QScreen::availableGeometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::geometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, relayoutAll);
	}

	this->markAll();
}

void TilingManager::onActiveWindowChanged() {
	auto* window = this->tracker->activeWindow();
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto& m = iter->second;
	m.focusStamp = ++this->focusCounter;
	if (m.layout != nullptr) m.layout->lastFocused = window;
}

void TilingManager::onRectChanged(TrackedWindow* window) {
	if (window == this->dragging) return;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	const auto& m = iter->second;
	if (m.layout == nullptr || m.target.isNull()) return;

	this->unverified.insert(window);
	if (!this->verifyTimer.isActive()) this->verifyTimer.start();
}

void TilingManager::verify() {
	auto pending = std::move(this->unverified);
	this->unverified.clear();

	for (auto* window: pending) {
		auto iter = this->managed.find(window);
		if (iter == this->managed.end() || window == this->dragging) continue;

		auto& m = iter->second;
		if (m.layout == nullptr || m.target.isNull()) continue;
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, m.target);
	}
}

void TilingManager::giveUp(Managed& m, const QRect& frame) {
	m.verifying = false;

	auto bigger = frame.width() > m.target.width() + EDGE_TOLERANCE
	           || frame.height() > m.target.height() + EDGE_TOLERANCE;
	auto away = !m.target.contains(frame.center());

	if (bigger || away) {
		qCInfo(logTiling) << "Window" << m.window->addressHex() << m.window->appId()
		                  << "doesn't take its tile" << m.target << "(is" << frame << "); it floats.";
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(m.window);
	} else {
		m.accepted = frame;
	}
}

void TilingManager::onDeadline() {
	auto now = QDateTime::currentMSecsSinceEpoch();

	for (auto& [window, m]: this->managed) {
		if (!m.verifying || m.layout == nullptr || m.target.isNull() || window == this->dragging) {
			continue;
		}
		if (now < m.deadline) continue;

		auto* hwnd = window->hwnd();
		auto frame = currentFrame(hwnd);

		if (frame == m.target) {
			m.verifying = false;
			continue;
		}

		if (frame != m.before) {
			this->unverified.insert(window);
			if (!this->verifyTimer.isActive()) this->verifyTimer.start();
			continue;
		}

		if (IsHungAppWindow(hwnd)) continue;

		qCInfo(logTiling) << "Window" << window->addressHex() << window->appId()
		                  << "ignores being moved; it floats.";
		m.verifying = false;
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(window);
	}

	this->armDeadline();
}

void TilingManager::onMoveSizeStarted(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return;

	auto state = this->stateOf(iter->second);
	if (state != State::Tiled && !(state == State::Suspended && window->maximized())) return;

	this->dragging = window;
	this->unverified.remove(window);
}

void TilingManager::onMoveSizeEnded(TrackedWindow* window) {
	if (window != this->dragging) return;
	this->dragging = nullptr;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	auto* hwnd = window->hwnd();
	if (m.layout == nullptr || m.target.isNull() || IsIconic(hwnd) || IsZoomed(hwnd)) {
		this->markWindow(window);
		return;
	}

	auto frame = currentFrame(hwnd);
	const auto target = m.accepted.isNull() ? m.target : m.accepted;

	auto movedLeft = qAbs(frame.left() - target.left()) > EDGE_TOLERANCE;
	auto movedTop = qAbs(frame.top() - target.top()) > EDGE_TOLERANCE;
	auto movedRight = qAbs(rightOf(frame) - rightOf(target)) > EDGE_TOLERANCE;
	auto movedBottom = qAbs(bottomOf(frame) - bottomOf(target)) > EDGE_TOLERANCE;
	auto moved = int(movedLeft) + int(movedTop) + int(movedRight) + int(movedBottom);
	auto resized = qAbs(frame.width() - target.width()) > EDGE_TOLERANCE
	            || qAbs(frame.height() - target.height()) > EDGE_TOLERANCE;

	m.target = QRect();

	if (moved == 0) {
		this->markLayout(m.layout);
		return;
	}

	if (!resized || moved == 4) {
		this->drop(m, cursorPos());
		return;
	}

	auto* layout = m.layout;
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());
	auto id = idOf(window);

	if (movedLeft) layout->tree->moveEdge(id, Edge::Left, frame.left() - in);
	if (movedRight) layout->tree->moveEdge(id, Edge::Right, rightOf(frame) + in);
	if (movedTop) layout->tree->moveEdge(id, Edge::Top, frame.top() - in);
	if (movedBottom) layout->tree->moveEdge(id, Edge::Bottom, bottomOf(frame) + in);

	this->markLayout(layout);
}

void TilingManager::drop(Managed& m, const QPoint& cursor) {
	auto* from = m.layout;
	auto self = idOf(m.window);

	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree->ids()) {
			if (id == self) continue;
			auto* other = this->windowOf(id);
			if (other == nullptr) continue;

			auto& o = this->managed.at(other);
			if (this->stateOf(o) != State::Tiled) continue;

			if (layout->tree->box(id).contains(cursor)) {
				this->swapWindows(m, o);
				return;
			}
		}
	}

	auto point = POINT {cursor.x(), cursor.y()};
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONULL));

	if (screen != nullptr && screen != from->screen) {
		this->moveToLayout(m, this->layoutAt(from->desktop, screen, true), nullptr);
		return;
	}

	this->markLayout(from);
}

TilingManager::Managed* TilingManager::tiledActive() {
	if (this->tracker == nullptr) return nullptr;

	auto iter = this->managed.find(this->tracker->activeWindow());
	if (iter == this->managed.end()) return nullptr;

	auto& m = iter->second;
	if (m.layout == nullptr || this->stateOf(m) != State::Tiled) return nullptr;
	return &m;
}

TilingManager::Managed* TilingManager::neighbour(const Managed& from, Edge direction) {
	if (from.layout == nullptr) return nullptr;

	auto source = from.layout->tree->box(idOf(from.window));
	Managed* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from.layout->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree->ids()) {
			auto* window = this->windowOf(id);
			if (window == nullptr || window == from.window) continue;

			auto& m = this->managed.at(window);
			if (this->stateOf(m) != State::Tiled) continue;

			auto box = layout->tree->box(id);
			auto distance = 0;
			auto overlap = 0;

			switch (direction) {
			case Edge::Left:
				distance = source.left() - rightOf(box);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Right:
				distance = box.left() - rightOf(source);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Top:
				distance = source.top() - bottomOf(box);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			case Edge::Bottom:
				distance = box.top() - bottomOf(source);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			}

			if (distance < -EDGE_TOLERANCE || overlap <= 0) continue;

			if (best == nullptr || distance < bestDistance
			    || (distance == bestDistance && m.focusStamp > best->focusStamp))
			{
				best = &m;
				bestDistance = distance;
			}
		}
	}

	return best;
}

QScreen* TilingManager::screenInDirection(QScreen* from, Edge direction) const {
	auto source = physicalMonitor(from);
	QScreen* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto* screen: QGuiApplication::screens()) {
		if (screen == from) continue;
		auto rect = physicalMonitor(screen);
		auto distance = 0;
		auto overlap = 0;

		switch (direction) {
		case Edge::Left:
			distance = source.left() - rightOf(rect);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Right:
			distance = rect.left() - rightOf(source);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Top:
			distance = source.top() - bottomOf(rect);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		case Edge::Bottom:
			distance = rect.top() - bottomOf(source);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		}

		if (distance < 0 || overlap <= 0) continue;
		if (distance < bestDistance) {
			best = screen;
			bestDistance = distance;
		}
	}

	return best;
}

void TilingManager::swapWindows(Managed& a, Managed& b) {
	auto* layoutA = a.layout;
	auto* layoutB = b.layout;
	if (layoutA == nullptr || layoutB == nullptr) return;

	if (layoutA == layoutB) {
		layoutA->tree->swap(idOf(a.window), idOf(b.window));
	} else {
		layoutA->tree->replace(idOf(a.window), idOf(b.window));
		layoutB->tree->replace(idOf(b.window), idOf(a.window));
		a.layout = layoutB;
		b.layout = layoutA;
		if (layoutA->lastFocused == a.window) layoutA->lastFocused = b.window;
		if (layoutB->lastFocused == b.window) layoutB->lastFocused = a.window;
	}

	this->markLayout(layoutA);
	this->markLayout(layoutB);
}

void TilingManager::moveToLayout(Managed& m, Layout* layout, TrackedWindow* target) {
	auto* from = m.layout;
	if (from == nullptr || layout == nullptr || from == layout) return;

	from->tree->remove(idOf(m.window));
	if (from->lastFocused == m.window) from->lastFocused = nullptr;
	this->markLayout(from);

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	layout->tree->configure(this->options());
	layout->tree->insert(idOf(m.window), target == nullptr ? 0 : idOf(target), nullptr);
	layout->lastFocused = m.window;
	m.layout = layout;
	m.target = QRect();
	this->markLayout(layout);
}

bool TilingManager::focusDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	auto* next = this->neighbour(*m, direction);
	if (next == nullptr) return false;

	next->window->activate();
	return true;
}

bool TilingManager::moveDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	auto* next = this->neighbour(*m, direction);

	if (next != nullptr && next->layout == m->layout) {
		this->swapWindows(*m, *next);
	} else if (next != nullptr) {
		this->moveToLayout(*m, next->layout, next->window);
	} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
		this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
	}

	return true;
}

bool TilingManager::swapDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (auto* next = this->neighbour(*m, direction)) this->swapWindows(*m, *next);
	return true;
}

bool TilingManager::toggleSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->toggleSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::swapSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->swapSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::splitRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->splitRatio(idOf(window), value, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::masterRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->kind() != tiling::Kind::Master) return false;
	if (layout->tree->splitRatio(idOf(window), value * 2.0, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::swapWithMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* other = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (other != nullptr && other != window) this->swapWindows(m, this->managed.at(other));
	return true;
}

bool TilingManager::focusMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* target = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (target != nullptr && target != window) target->activate();
	return true;
}

bool TilingManager::resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	auto dpr = layout->screen->devicePixelRatio();
	if (layout->tree->resize(idOf(window), toPhysical(dx, dpr), toPhysical(dy, dpr))) {
		this->markLayout(layout);
	}

	return true;
}

bool TilingManager::setFloating(TrackedWindow* window, bool floating, bool toggle) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;

	auto& m = iter->second;
	if (toggle) floating = m.layout != nullptr;

	if (!floating && m.elevated) {
		qCInfo(logTiling) << "Window" << window->addressHex()
		                  << "belongs to a more privileged process and can't be tiled.";
		return false;
	}

	m.override = floating ? Override::Float : Override::Tile;
	if (!floating) m.autoFloat = false;

	this->markWindow(window);
	return true;
}

bool TilingManager::beginEdgeDrag(TrackedWindow* window, bool left, bool top) {
	this->edgeDragState = EdgeDrag();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (this->stateOf(m) != State::Tiled) return false;

	auto* layout = m.layout;
	auto id = idOf(window);
	auto pick = [&](Edge preferred, Edge other) -> std::optional<Edge> {
		if (layout->tree->hasEdge(id, preferred)) return preferred;
		if (layout->tree->hasEdge(id, other)) return other;
		return std::nullopt;
	};

	auto& drag = this->edgeDragState;
	drag.window = window;
	drag.layout = layout;
	drag.box = layout->tree->box(id);
	drag.horizontal = left ? pick(Edge::Left, Edge::Right) : pick(Edge::Right, Edge::Left);
	drag.vertical = top ? pick(Edge::Top, Edge::Bottom) : pick(Edge::Bottom, Edge::Top);
	return true;
}

void TilingManager::edgeDrag(const QPoint& delta) {
	auto& drag = this->edgeDragState;
	if (drag.window == nullptr) return;

	auto iter = this->managed.find(drag.window);
	if (iter == this->managed.end() || iter->second.layout != drag.layout
	    || !this->layoutAlive(drag.layout))
	{
		drag = EdgeDrag();
		return;
	}

	auto edgeOf = [&drag](Edge edge) {
		switch (edge) {
		case Edge::Left: return drag.box.left();
		case Edge::Right: return rightOf(drag.box);
		case Edge::Top: return drag.box.top();
		case Edge::Bottom: return bottomOf(drag.box);
		}
		return 0;
	};

	auto& tree = *drag.layout->tree;
	auto id = idOf(drag.window);
	auto changed = false;

	if (drag.horizontal) {
		changed |= tree.moveEdge(id, *drag.horizontal, edgeOf(*drag.horizontal) + delta.x());
	}

	if (drag.vertical) {
		changed |= tree.moveEdge(id, *drag.vertical, edgeOf(*drag.vertical) + delta.y());
	}

	if (changed) this->markLayout(drag.layout);
}

void TilingManager::endEdgeDrag() { this->edgeDragState = EdgeDrag(); }

void TilingManager::resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact) {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto dpr = window->screen()->devicePixelRatio();
	auto frame = currentFrame(hwnd);
	auto width = toPhysical(dx, dpr);
	auto height = toPhysical(dy, dpr);
	if (!exact) {
		width += frame.width();
		height += frame.height();
	}

	setFrame(hwnd, QRect(frame.topLeft(), QSize(qMax(1, width), qMax(1, height))));
}

void TilingManager::center(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto frame = currentFrame(hwnd);
	frame.moveTo(
	    work.left() + (work.width() - frame.width()) / 2,
	    work.top() + (work.height() - frame.height()) / 2
	);
	setFrame(hwnd, frame);
}

void TilingManager::moveFloating(TrackedWindow* window, Edge direction) const {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto gap = toPhysical(this->mGapsOut, window->screen()->devicePixelRatio());
	work.adjust(gap, gap, -gap, -gap);

	auto frame = currentFrame(hwnd);
	switch (direction) {
	case Edge::Left: frame.moveLeft(work.left()); break;
	case Edge::Right: frame.moveRight(work.right()); break;
	case Edge::Top: frame.moveTop(work.top()); break;
	case Edge::Bottom: frame.moveBottom(work.bottom()); break;
	}

	setFrame(hwnd, frame);
}

Tiling::Tiling(QObject* parent): QObject(parent) {
	auto* manager = TilingManager::instance();
	QObject::connect(manager, &TilingManager::enabledChanged, this, &Tiling::enabledChanged);
	QObject::connect(manager, &TilingManager::gapsInChanged, this, &Tiling::gapsInChanged);
	QObject::connect(manager, &TilingManager::gapsOutChanged, this, &Tiling::gapsOutChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::preserveSplitChanged, this, &Tiling::preserveSplitChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::excludedChanged, this, &Tiling::excludedChanged);
	QObject::connect(manager, &TilingManager::layoutChanged, this, &Tiling::layoutChanged);
	QObject::connect(manager, &TilingManager::smartSplitChanged, this, &Tiling::smartSplitChanged);
	QObject::connect(manager, &TilingManager::forceSplitChanged, this, &Tiling::forceSplitChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::defaultSplitRatioChanged, this, &Tiling::defaultSplitRatioChanged);
	QObject::connect(manager, &TilingManager::masterNewStatusChanged, this, &Tiling::masterNewStatusChanged);
	QObject::connect(manager, &TilingManager::masterNewOnTopChanged, this, &Tiling::masterNewOnTopChanged);
	QObject::connect(manager, &TilingManager::masterOrientationChanged, this, &Tiling::masterOrientationChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::masterFactorChanged, this, &Tiling::masterFactorChanged);
}

void Tiling::relayout() { TilingManager::instance()->relayout(); }

bool Tiling::enabled() const { return TilingManager::instance()->enabled(); }
void Tiling::setEnabled(bool enabled) { TilingManager::instance()->setEnabled(enabled); }
qint32 Tiling::gapsIn() const { return TilingManager::instance()->gapsIn(); }
void Tiling::setGapsIn(qint32 gaps) { TilingManager::instance()->setGapsIn(gaps); }
qint32 Tiling::gapsOut() const { return TilingManager::instance()->gapsOut(); }
void Tiling::setGapsOut(qint32 gaps) { TilingManager::instance()->setGapsOut(gaps); }
bool Tiling::preserveSplit() const { return TilingManager::instance()->preserveSplit(); }
void Tiling::setPreserveSplit(bool preserve) {
	TilingManager::instance()->setPreserveSplit(preserve);
}
QStringList Tiling::excluded() const { return TilingManager::instance()->excluded(); }
void Tiling::setExcluded(const QStringList& excluded) {
	TilingManager::instance()->setExcluded(excluded);
}

QString Tiling::layout() const { return TilingManager::instance()->layout(); }
void Tiling::setLayout(const QString& layout) { TilingManager::instance()->setLayout(layout); }
bool Tiling::smartSplit() const { return TilingManager::instance()->smartSplit(); }
void Tiling::setSmartSplit(bool smart) { TilingManager::instance()->setSmartSplit(smart); }
qint32 Tiling::forceSplit() const { return TilingManager::instance()->forceSplit(); }
void Tiling::setForceSplit(qint32 force) { TilingManager::instance()->setForceSplit(force); }
double Tiling::defaultSplitRatio() const { return TilingManager::instance()->defaultSplitRatio(); }
void Tiling::setDefaultSplitRatio(double ratio) {
	TilingManager::instance()->setDefaultSplitRatio(ratio);
}
QString Tiling::masterNewStatus() const { return TilingManager::instance()->masterNewStatus(); }
void Tiling::setMasterNewStatus(const QString& status) {
	TilingManager::instance()->setMasterNewStatus(status);
}
bool Tiling::masterNewOnTop() const { return TilingManager::instance()->masterNewOnTop(); }
void Tiling::setMasterNewOnTop(bool onTop) { TilingManager::instance()->setMasterNewOnTop(onTop); }
QString Tiling::masterOrientation() const { return TilingManager::instance()->masterOrientation(); }
void Tiling::setMasterOrientation(const QString& orientation) {
	TilingManager::instance()->setMasterOrientation(orientation);
}
double Tiling::masterFactor() const { return TilingManager::instance()->masterFactor(); }
void Tiling::setMasterFactor(double factor) { TilingManager::instance()->setMasterFactor(factor); }

} // namespace qs::windows
