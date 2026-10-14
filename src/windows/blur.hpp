#pragma once

#include <memory>
#include <optional>

#include <qt_windows.h>

#include <qbytearray.h>
#include <qfilesystemwatcher.h>
#include <qlist.h>
#include <qmetaobject.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qqmlintegration.h>
#include <qquickwindow.h>
#include <qrect.h>
#include <qregion.h>
#include <qregularexpression.h>
#include <qsize.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>

#include "blur_shapes.hpp"

class QWinEventNotifier;

namespace qs::windows {

class WinPanelWindow;

struct BlurRule {
	bool blur = false;
	std::optional<qreal> ignoreAlpha;
};

class BackdropWindow;
class PanelBlur;

class BlurManager: public QObject {
	Q_OBJECT;

public:
	~BlurManager() override;
	Q_DISABLE_COPY_MOVE(BlurManager);

	static BlurManager* instance();

	void setShellDir(const QString& shellDir);
	void reload();

	[[nodiscard]] BlurRule ruleFor(const QString& ns) const;

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

	[[nodiscard]] bool available() const;

	[[nodiscard]] QString configPath() const { return this->mConfigPath; }
	[[nodiscard]] QVariantList rulesInfo() const;

	enum class Backend : quint8 {
		HostBackdrop,
		Accent,
	};

	[[nodiscard]] Backend backend() const { return this->mBackend; }

	[[nodiscard]] bool layeredBackdrops() const;
	[[nodiscard]] bool debugTint() const { return this->mDebugTint; }

	void scheduleSystemCheck(bool recreate = false);

	[[nodiscard]] bool shutDown() const { return this->mShutDown; }

	struct Composition;
	[[nodiscard]] Composition* ensureComposition();
	[[nodiscard]] bool deferComposition();
	void markUnsupported(const QString& reason);

	void registerPanel(PanelBlur* panel);
	void unregisterPanel(PanelBlur* panel);

signals:
	void rulesChanged();
	void stateChanged();

private:
	explicit BlurManager(QObject* parent);

	struct Rule {
		QString pattern;
		QRegularExpression regex;
		std::optional<bool> blur;
		std::optional<std::optional<qreal>> ignoreAlpha;
	};

	void detectBackend();
	void loadFile();
	void parse(const QByteArray& data, const QString& path);
	void updateWatches();
	void scheduleReload();
	void checkSystem();
	void watchRegistry();
	void armRegistryNotification();
	void notifyPanels();
	void scheduleNotifyPanels();
	void shutdown();
	void closeCompositor();

	[[nodiscard]] QString userFilePath() const;
	[[nodiscard]] QString defaultFilePath() const;

	QString shellDir;
	QString mConfigPath;
	QByteArray loadedData;
	bool loaded = false;
	bool parsed = false;
	QList<Rule> rules;
	bool fileEnabled = true;
	bool fileLayered = false;
	bool envEnabled = true;
	bool envLayered = false;
	bool mDebugTint = false;
	bool mEnabled = true;
	Backend mBackend = Backend::HostBackdrop;

	bool transparencyEffects = true;
	bool highContrast = false;
	bool energySaver = false;
	bool unsupported = false;
	bool systemCheckPending = false;
	bool recreatePending = false;
	bool notifyPending = false;
	bool mShutDown = false;
	bool compositionWaiting = false;

	QFileSystemWatcher watcher;
	QTimer reloadTimer;

	HKEY personalizeKey = nullptr;
	HANDLE registryEvent = nullptr;
	QWinEventNotifier* registryNotifier = nullptr;

	std::unique_ptr<Composition> composition;

	QList<PanelBlur*> panels;
};

class PanelBlur: public QObject {
	Q_OBJECT;

public:
	explicit PanelBlur(WinPanelWindow* panel);
	~PanelBlur() override;
	Q_DISABLE_COPY_MOVE(PanelBlur);

	void adopt(PanelBlur* other);
	void attach();
	void release();

	void setInputMask(const QRegion& region, bool hasMask);

	void syncPlacement();
	void recreate();

	void updateActive();

private slots:
	void onFrame();
	void updateShapes();

private:
	struct ShapeInputs {
		bool shown = false;
		QSize size;
		qreal alpha = -1;
		qreal dpr = 0;

		[[nodiscard]] bool operator==(const ShapeInputs& other) const = default;
	};

	void connectFrames();
	void disconnectFrames();
	void scheduleShapes();
	[[nodiscard]] ShapeInputs shapeInputs(bool shown) const;
	void collectShapes(QList<BlurShape>& shapes);
	bool ensureBackdrop();
	void destroyBackdrop();
	[[nodiscard]] bool panelShown() const;

	WinPanelWindow* panel;
	QPointer<QQuickWindow> mWindow;
	QMetaObject::Connection frameConnection;
	QPointer<QQuickWindow> framesWindow;
	std::unique_ptr<BackdropWindow> backdrop;
	BlurRule rule;
	bool active = false;
	bool shapesPending = false;
	bool shapesForced = true;
	ShapeInputs lastInputs;
	bool stale = true;
	bool panelWasShown = false;
	QRegion mask;
	bool hasMask = false;
	QList<BlurShape> shapes;
	bool truncatedWarned = false;
};

class BackdropBlur: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged);
	Q_PROPERTY(bool available READ available NOTIFY stateChanged);
	Q_PROPERTY(QString configPath READ configPath NOTIFY rulesChanged);
	Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit BackdropBlur(QObject* parent);

	static BackdropBlur* create(QQmlEngine* engine, QJSEngine* jsEngine);

	[[nodiscard]] static bool enabled();
	static void setEnabled(bool enabled);
	[[nodiscard]] static bool available();
	[[nodiscard]] static QString configPath();
	[[nodiscard]] static QVariantList rules();

	Q_INVOKABLE static void reload();

signals:
	void stateChanged();
	void rulesChanged();
};

} // namespace qs::windows
