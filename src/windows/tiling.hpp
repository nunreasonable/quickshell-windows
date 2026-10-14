#pragma once

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "tiling_layout.hpp"

namespace qs::windows {

class TrackedWindow;
class WindowTracker;
class VirtualDesktops;

class TilingManager: public QObject {
	Q_OBJECT;

public:
	using Edge = tiling::Edge;

	static TilingManager* instance();
	static TilingManager* active();

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);
	[[nodiscard]] qint32 gapsIn() const { return this->mGapsIn; }
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const { return this->mGapsOut; }
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const { return this->mPreserveSplit; }
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const { return this->mExcluded; }
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QString layout() const { return this->mLayout; }
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const { return this->mSmartSplit; }
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const { return this->mForceSplit; }
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const { return this->mDefaultSplitRatio; }
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const { return this->mMasterNewStatus; }
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const { return this->mMasterNewOnTop; }
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const { return this->mMasterOrientation; }
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const { return this->mMasterFactor; }
	void setMasterFactor(double factor);

	void relayout();

	[[nodiscard]] bool isTiled(TrackedWindow* window) const;

	bool focusDirection(Edge direction);
	bool moveDirection(Edge direction);
	bool swapDirection(Edge direction);
	bool toggleSplit(TrackedWindow* window);
	bool swapSplit(TrackedWindow* window);
	bool splitRatio(TrackedWindow* window, double value, bool exact);
	bool masterRatio(TrackedWindow* window, double value, bool exact);
	bool swapWithMaster(TrackedWindow* window);
	bool focusMaster(TrackedWindow* window);
	bool resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy);
	bool setFloating(TrackedWindow* window, bool floating, bool toggle);

	bool beginEdgeDrag(TrackedWindow* window, bool left, bool top);
	void edgeDrag(const QPoint& delta);
	void endEdgeDrag();

	static void resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact);
	static void center(TrackedWindow* window);
	void moveFloating(TrackedWindow* window, Edge direction) const;

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
	void windowTilingChanged(TrackedWindow* window);

private:
	explicit TilingManager();
	~TilingManager() override = default;
	Q_DISABLE_COPY_MOVE(TilingManager);

	struct Layout {
		GUID desktop {};
		QPointer<QScreen> screen;
		std::unique_ptr<tiling::LayoutEngine> tree;
		TrackedWindow* lastFocused = nullptr;
	};

	enum class State : quint8 {
		Out,
		Hidden,
		Suspended,
		Tiled,
	};

	enum class Override : quint8 { None, Float, Tile };

	struct EdgeDrag {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		QRect box;
		std::optional<Edge> horizontal;
		std::optional<Edge> vertical;
	};

	struct Managed {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		Override override = Override::None;
		bool elevated = false;
		bool autoFloat = false;
		QRect target;
		QRect before;
		QRect accepted;
		qint32 corrections = 0;
		qint64 lastCorrection = 0;
		bool verifying = false;
		qint64 deadline = 0;
		bool parked = false;
		QRect preTiling;
		quint64 focusStamp = 0;
	};

	bool start();
	void stop();
	void manage(TrackedWindow* window);
	void unmanage(TrackedWindow* window);

	[[nodiscard]] State stateOf(const Managed& m) const;
	[[nodiscard]] bool floatsByRule(const Managed& m) const;
	[[nodiscard]] bool isExcluded(TrackedWindow* window) const;
	[[nodiscard]] Layout* layoutFor(TrackedWindow* window, bool create);
	[[nodiscard]] Layout* layoutAt(const GUID& desktop, QScreen* screen, bool create);
	[[nodiscard]] bool layoutAlive(const Layout* layout) const;
	[[nodiscard]] static tiling::LayoutEngine::Id idOf(TrackedWindow* window);
	[[nodiscard]] TrackedWindow* windowOf(tiling::LayoutEngine::Id id) const;
	[[nodiscard]] tiling::Kind layoutKind() const;
	[[nodiscard]] tiling::Options options() const;
	void rebuildLayouts();

	void markWindow(TrackedWindow* window);
	void markLayout(Layout* layout);
	void schedule();
	void sync();
	void syncWindow(TrackedWindow* window);
	void insertInto(Managed& m, Layout* layout, TrackedWindow* target, const QPoint* cursor);
	void takeOut(Managed& m);
	void dropEmptyLayouts();
	void apply(Layout* layout);
	[[nodiscard]] QRect tileRect(const Layout* layout, TrackedWindow* window) const;
	void place(Managed& m, const QRect& tile);
	void send(Managed& m, const QRect& frame);
	void armDeadline();
	void restoreFloating(Managed& m);

	void onActiveWindowChanged();
	void onRectChanged(TrackedWindow* window);
	void onMoveSizeStarted(TrackedWindow* window);
	void onMoveSizeEnded(TrackedWindow* window);
	void onScreensChanged();
	void verify();
	void onDeadline();
	void drop(Managed& m, const QPoint& cursor);
	void giveUp(Managed& m, const QRect& frame);
	void markAll();

	[[nodiscard]] Managed* tiledActive();
	[[nodiscard]] Managed* neighbour(const Managed& from, Edge direction);
	[[nodiscard]] QScreen* screenInDirection(QScreen* from, Edge direction) const;
	void swapWindows(Managed& a, Managed& b);
	void moveToLayout(Managed& m, Layout* layout, TrackedWindow* target);

	WindowTracker* tracker = nullptr;
	VirtualDesktops* desktops = nullptr;

	bool mEnabled = false;
	qint32 mGapsIn = 4;
	qint32 mGapsOut = 5;
	bool mPreserveSplit = true;
	QStringList mExcluded;
	QString mLayout = QStringLiteral("dwindle");
	bool mSmartSplit = false;
	qint32 mForceSplit = 0;
	double mDefaultSplitRatio = 1.0;
	QString mMasterNewStatus = QStringLiteral("slave");
	bool mMasterNewOnTop = false;
	QString mMasterOrientation = QStringLiteral("left");
	double mMasterFactor = 0.55;
	QList<QRegularExpression> excludedPatterns;
	QList<QRegularExpression> excludedTitles;

	std::unordered_map<TrackedWindow*, Managed> managed;
	std::map<QString, std::unique_ptr<Layout>> layouts;
	QSet<TrackedWindow*> dirtyWindows;
	QSet<Layout*> dirtyLayouts;
	QSet<TrackedWindow*> unverified;
	TrackedWindow* dragging = nullptr;
	EdgeDrag edgeDragState;
	quint64 focusCounter = 0;

	HANDLE ownerMutex = nullptr;
	QTimer syncTimer;
	QTimer verifyTimer;
	QTimer deadlineTimer;
	QList<QMetaObject::Connection> connections;
};

class Tiling: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(qint32 gapsIn READ gapsIn WRITE setGapsIn NOTIFY gapsInChanged);
	Q_PROPERTY(qint32 gapsOut READ gapsOut WRITE setGapsOut NOTIFY gapsOutChanged);
	Q_PROPERTY(bool preserveSplit READ preserveSplit WRITE setPreserveSplit NOTIFY preserveSplitChanged);
	Q_PROPERTY(QStringList excluded READ excluded WRITE setExcluded NOTIFY excludedChanged);
	Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged);
	Q_PROPERTY(bool smartSplit READ smartSplit WRITE setSmartSplit NOTIFY smartSplitChanged);
	Q_PROPERTY(qint32 forceSplit READ forceSplit WRITE setForceSplit NOTIFY forceSplitChanged);
	Q_PROPERTY(double defaultSplitRatio READ defaultSplitRatio WRITE setDefaultSplitRatio NOTIFY defaultSplitRatioChanged);
	Q_PROPERTY(QString masterNewStatus READ masterNewStatus WRITE setMasterNewStatus NOTIFY masterNewStatusChanged);
	Q_PROPERTY(bool masterNewOnTop READ masterNewOnTop WRITE setMasterNewOnTop NOTIFY masterNewOnTopChanged);
	Q_PROPERTY(QString masterOrientation READ masterOrientation WRITE setMasterOrientation NOTIFY masterOrientationChanged);
	Q_PROPERTY(double masterFactor READ masterFactor WRITE setMasterFactor NOTIFY masterFactorChanged);
	// clang-format on

public:
	explicit Tiling(QObject* parent = nullptr);

	Q_INVOKABLE void relayout();

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] qint32 gapsIn() const;
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const;
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const;
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const;
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QString layout() const;
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const;
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const;
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const;
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const;
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const;
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const;
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const;
	void setMasterFactor(double factor);

signals:
	void enabledChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
};

} // namespace qs::windows
