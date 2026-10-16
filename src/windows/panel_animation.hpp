#pragma once

#include <optional>

#include <qmatrix4x4.h>
#include <qnamespace.h>
#include <qpoint.h>
#include <qquickitem.h>
#include <qrect.h>
#include <qsize.h>
#include <qstring.h>
#include <qtypes.h>

namespace qs::windows {

struct PanelAnimationStyle {
	enum class Kind : quint8 {
		None,
		Slide,
		Popin,
		Fade,
	};

	Kind kind = Kind::None;
	std::optional<Qt::Edge> edge;
	qreal popinScale = 0;

	[[nodiscard]] static std::optional<PanelAnimationStyle> parse(const QString& text);
	[[nodiscard]] QString toString() const;
	[[nodiscard]] bool animates() const { return this->kind != Kind::None; }
	[[nodiscard]] bool moves() const {
		return this->kind == Kind::Slide || this->kind == Kind::Popin;
	}
	[[nodiscard]] bool operator==(const PanelAnimationStyle& other) const = default;
};

class CubicBezier {
public:
	constexpr CubicBezier(qreal x1, qreal y1, qreal x2, qreal y2)
	    : mX1(x1)
	    , mY1(y1)
	    , mX2(x2)
	    , mY2(y2) {}

	[[nodiscard]] qreal valueAt(qreal progress) const;

private:
	[[nodiscard]] qreal xAt(qreal t) const;
	[[nodiscard]] qreal yAt(qreal t) const;
	[[nodiscard]] qreal xSlopeAt(qreal t) const;

	qreal mX1;
	qreal mY1;
	qreal mX2;
	qreal mY2;
};

struct PanelAnimationTrack {
	qreal from = 1;
	qreal to = 1;
	qreal duration = 0;
	CubicBezier curve {0, 0, 1, 1};

	[[nodiscard]] qreal valueAt(qreal elapsed) const;
};

struct PanelAnimationTiming {
	qreal showMotionMs;
	qreal showFadeMs;
	qreal hideMotionMs;
	qreal hideFadeMs;
	CubicBezier showMotion;
	CubicBezier showFade;
	CubicBezier hideMotion;
	CubicBezier hideFade;

	[[nodiscard]] static const PanelAnimationTiming& hyprland();
};

class PanelAnimation {
public:
	void start(bool show, const PanelAnimationStyle& style);
	bool advance(qreal elapsed);
	void jumpTo(bool shown);

	[[nodiscard]] bool showing() const { return this->mShowing; }
	[[nodiscard]] qreal duration() const;
	[[nodiscard]] qreal motion() const { return this->mMotion; }
	[[nodiscard]] qreal opacity() const { return this->mOpacity; }
	[[nodiscard]] bool settled() const;

private:
	bool mShowing = true;
	qreal mMotion = 1;
	qreal mOpacity = 1;
	PanelAnimationTrack motionTrack;
	PanelAnimationTrack opacityTrack;
};

struct PanelAnimationFrame {
	QPointF offset;
	qreal scale = 1;
	qreal opacity = 1;

	[[nodiscard]] bool isIdentity() const;
};

[[nodiscard]] Qt::Edge panelSlideEdge(
    std::optional<Qt::Edge> forced,
    const QRectF& panel,
    const QRectF& screen,
    Qt::Edges anchors
);

[[nodiscard]] PanelAnimationFrame panelAnimationFrame(
    const PanelAnimationStyle& style,
    Qt::Edge edge,
    const QSizeF& size,
    qreal motion,
    qreal opacity,
    qreal dpr
);

class PanelContentTransform: public QQuickTransform {
public:
	explicit PanelContentTransform(QObject* parent = nullptr): QQuickTransform(parent) {}

	void setFrame(const PanelAnimationFrame& frame, const QSizeF& size);
	void setBypassed(bool bypassed) { this->mBypassed = bypassed; }

	void applyTo(QMatrix4x4* matrix) const override;

private:
	QPointF mOffset;
	qreal mScale = 1;
	QPointF mOrigin;
	bool mBypassed = false;
};

} // namespace qs::windows
