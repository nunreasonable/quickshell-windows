#include <qcoreapplication.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qelapsedtimer.h>
#include <qevent.h>
#include <qfont.h>
#include <qfontdatabase.h>
#include <qguiapplication.h>
#include <qicon.h>
#include <qlist.h>
#include <qlogging.h>
#include <qpointer.h>
#include <qqml.h>
#include <qquickwindow.h>
#include <qstring.h>
#include <qtimer.h>

#include "../core/logcat.hpp"
#include "../core/plugin.hpp"
#include "appbar.hpp"
#include "desktopentry_backend.hpp"
#include "panel_window.hpp"
#include "util.hpp"

#include <dwmapi.h>

namespace {

QS_LOGGING_CATEGORY(logStartup, "quickshell.startup", QtWarningMsg);

constexpr const char* PERUSER_FONT_PREFIX = "jetbrainsmononerdfont-";
constexpr const char* PERUSER_FONT_REGULAR_FILE = "jetbrainsmononerdfont-regular.ttf";
constexpr const char* PERUSER_FONT_FAMILY = "JetBrainsMono Nerd Font";

bool allPeruserFontsPresent(const QDir& dir, const QDir& userFontsDir) {
	if (!userFontsDir.exists()) return false;

	auto iter = QDirIterator(
	    dir.path(),
	    {"*.ttf", "*.otf", "*.ttc"},
	    QDir::Files,
	    QDirIterator::Subdirectories
	);
	auto any = false;

	while (iter.hasNext()) {
		iter.next();
		auto name = iter.fileName();
		if (!name.startsWith(PERUSER_FONT_PREFIX, Qt::CaseInsensitive)) continue;
		any = true;
		if (!userFontsDir.exists(name)) return false;
	}

	return any;
}

void loadBundledFonts() {
	auto dir = QDir(QCoreApplication::applicationDirPath() + "/fonts");
	if (!dir.exists()) return;

	auto userFontsDir = QDir(qEnvironmentVariable("LocalAppData") + "/Microsoft/Windows/Fonts");
	auto skipPeruser = allPeruserFontsPresent(dir, userFontsDir);

	enum class Select { NonPeruser, PeruserRegularOnly, PeruserAll };

	auto load = [&](Select which) {
		auto iter = QDirIterator(
		    dir.path(),
		    {"*.ttf", "*.otf", "*.ttc"},
		    QDir::Files,
		    QDirIterator::Subdirectories
		);

		while (iter.hasNext()) {
			auto path = iter.next();
			auto name = iter.fileName();
			auto isPeruser = name.startsWith(PERUSER_FONT_PREFIX, Qt::CaseInsensitive);

			if (which == Select::NonPeruser) {
				if (isPeruser) continue;
			} else if (which == Select::PeruserRegularOnly) {
				if (!isPeruser || name.compare(PERUSER_FONT_REGULAR_FILE, Qt::CaseInsensitive) != 0)
					continue;
			} else {
				if (!isPeruser) continue;
			}

			if (QFontDatabase::addApplicationFont(path) == -1) {
				qWarning() << "Failed to load bundled font" << path;
			}
		}
	};

	auto timer = QElapsedTimer();
	timer.start();
	load(Select::NonPeruser);

	if (!skipPeruser) load(Select::PeruserRegularOnly);
	qCDebug(logStartup) << "Added the bundled font files in" << timer.restart() << "ms";

	if (!QFontDatabase::families().contains(QString(PERUSER_FONT_FAMILY))) {
		load(Select::PeruserAll);
	}
	qCDebug(logStartup) << "Listed the system fonts in" << timer.restart() << "ms";

	QFont::insertSubstitution("JetBrains Mono NF", PERUSER_FONT_FAMILY);
	QFont::insertSubstitution("JetBrains Mono", PERUSER_FONT_FAMILY);
}

void addBundledIconPath() {
	auto dir = QCoreApplication::applicationDirPath() + "/icons";
	if (!QDir(dir).exists()) return;

	auto paths = QIcon::fallbackSearchPaths();
	if (!paths.contains(dir)) paths.prepend(dir);
	QIcon::setFallbackSearchPaths(paths);
}

void setCloaked(QWindow* window, bool cloaked) {
	if (window == nullptr || window->handle() == nullptr) return;
	BOOL value = cloaked ? TRUE : FALSE;
	DwmSetWindowAttribute(qs::windows::hwndOf(window), DWMWA_CLOAK, &value, sizeof(value));
}

class FirstFrameCloak: public QObject {
public:
	bool eventFilter(QObject* object, QEvent* event) override {
		if (event->type() != QEvent::PlatformSurface) return false;

		auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
		if (surfaceEvent->surfaceEventType() != QPlatformSurfaceEvent::SurfaceCreated) return false;

		auto* window = qobject_cast<QQuickWindow*>(object);
		if (window == nullptr || window->parent() != nullptr || window->type() != Qt::Window) return false;
		if (qobject_cast<qs::windows::WinProxiedWindow*>(window) != nullptr) return false;

		setCloaked(window, true);

		auto uncloak = [guard = QPointer<QQuickWindow>(window)]() { setCloaked(guard, false); };
		QObject::connect(
		    window,
		    &QQuickWindow::frameSwapped,
		    window,
		    uncloak,
		    static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection)
		);
		QTimer::singleShot(2000, window, uncloak);

		return false;
	}
};

class WindowsPlugin: public QsEnginePlugin {
	QString name() override { return "windows"; }
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override {
		qs::windows::optOutOfProcessPowerThrottling();
		qs::windows::WinAppBar::removeStale();
		auto timer = QElapsedTimer();
		timer.start();
		loadBundledFonts();
		qCDebug(logStartup) << "Loaded bundled fonts in" << timer.restart() << "ms";
		addBundledIconPath();
		qs::windows::WindowsDesktopEntryBackend::install();
		QCoreApplication::instance()->installEventFilter(new FirstFrameCloak());
		qCDebug(logStartup) << "Installed the desktop entry backend in" << timer.restart() << "ms";
	}

	void registerTypes() override {
		qmlRegisterType<qs::windows::WinPanelInterface>(
		    "Quickshell._WindowsOverlay",
		    1,
		    0,
		    "PanelWindow"
		);

		qmlRegisterModuleImport(
		    "Quickshell",
		    QQmlModuleImportModuleAny,
		    "Quickshell._WindowsOverlay",
		    QQmlModuleImportLatest
		);
	}
};

QS_REGISTER_PLUGIN(WindowsPlugin);

} // namespace
