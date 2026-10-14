#include "animation_timing.hpp"
#include <algorithm>
#include <atomic>

#include <private/qsgcontextplugin_p.h>
#include <private/qsgdefaultcontext_p.h>
#include <qabstractanimation.h>
#include <qelapsedtimer.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qplugin.h>
#include <qquickwindow.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtenvironmentvariables.h>

#include "../core/logcat.hpp"

namespace {

QS_LOGGING_CATEGORY(logAnimation, "quickshell.windows.animation", QtWarningMsg);

constexpr auto BACKEND_KEY = "qswindows";
constexpr qreal DEFAULT_MAX_REFRESH_RATE = 250;

qreal gMaxRefreshRate = DEFAULT_MAX_REFRESH_RATE;
std::atomic<float> gInterval {1000.0f / 60.0f};

void updateInterval() {
	qreal fastest = 0;
	for (auto* screen: QGuiApplication::screens()) fastest = std::max(fastest, screen->refreshRate());
	if (fastest < 1) fastest = 60;
	fastest = std::min(fastest, gMaxRefreshRate);

	auto interval = static_cast<float>(1000.0 / fastest);
	if (interval == gInterval.exchange(interval)) return;
	qCInfo(logAnimation) << "Animations tick every" << interval << "ms (" << fastest << "Hz )";
}

void watchScreen(QScreen* screen) {
	QObject::connect(screen, &QScreen::refreshRateChanged, qGuiApp, &updateInterval);
}

class WallClockAnimationDriver: public QAnimationDriver {
public:
	explicit WallClockAnimationDriver(QObject* parent): QAnimationDriver(parent) {}

	void start() override {
		this->wallTime.start();
		QAnimationDriver::start();
	}

	[[nodiscard]] qint64 elapsed() const override {
		return this->wallTime.isValid() ? this->wallTime.elapsed() : 0;
	}

	void advance() override { this->advanceAnimation(); }

private:
	QElapsedTimer wallTime;
};

class SceneGraphContext: public QSGDefaultContext {
public:
	explicit SceneGraphContext(QObject* parent = nullptr): QSGDefaultContext(parent) {}

	QAnimationDriver* createAnimationDriver(QObject* parent) override {
		return new WallClockAnimationDriver(parent);
	}

	float vsyncIntervalForAnimationDriver(QAnimationDriver* /*driver*/) override {
		return gInterval.load();
	}

	bool isVSyncDependent(QAnimationDriver* /*driver*/) override { return true; }
};

} // namespace

class QsWindowsSceneGraphPlugin: public QSGContextPlugin {
	Q_OBJECT;
	Q_PLUGIN_METADATA(IID QSGContextFactoryInterface_iid FILE "animation_timing.json");

public:
	explicit QsWindowsSceneGraphPlugin(QObject* parent = nullptr): QSGContextPlugin(parent) {}

	[[nodiscard]] QStringList keys() const override { return {QString::fromLatin1(BACKEND_KEY)}; }

	[[nodiscard]] QSGContext* create(const QString& /*key*/) const override {
		return new SceneGraphContext();
	}

	[[nodiscard]] Flags flags(const QString& /*key*/) const override {
		return QSGContextFactoryInterface::SupportsShaderEffectNode;
	}
};

namespace qs::windows {

void installAnimationTiming() {
	if (qEnvironmentVariableIsSet("QT_QUICK_BACKEND") || qEnvironmentVariableIsSet("QMLSCENE_DEVICE")
	    || qEnvironmentVariableIsSet("QSG_USE_SIMPLE_ANIMATION_DRIVER")
	    || qEnvironmentVariableIsSet("QSG_FIXED_ANIMATION_STEP"))
	{
		return;
	}

	if (qEnvironmentVariableIsSet("QS_WINDOWS_ANIMATION_HZ")) {
		auto ok = false;
		auto hz = qEnvironmentVariable("QS_WINDOWS_ANIMATION_HZ").toDouble(&ok);
		if (!ok || hz <= 0) return;
		gMaxRefreshRate = hz;
	}

	for (auto* screen: QGuiApplication::screens()) watchScreen(screen);
	QObject::connect(qGuiApp, &QGuiApplication::screenAdded, qGuiApp, [](QScreen* screen) {
		watchScreen(screen);
		updateInterval();
	});
	QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, qGuiApp, [](QScreen* /*screen*/) {
		updateInterval();
	});
	updateInterval();

	QQuickWindow::setSceneGraphBackend(QString::fromLatin1(BACKEND_KEY));
}

} // namespace qs::windows

#include "animation_timing.moc"
