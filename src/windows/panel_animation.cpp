#include "panel_animation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include <qmatrix4x4.h>
#include <qnamespace.h>
#include <qpoint.h>
#include <qrect.h>
#include <qsize.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtypes.h>

namespace qs::windows {

namespace {

constexpr qreal SOLVE_EPSILON = 1e-6;
constexpr int NEWTON_STEPS = 8;
constexpr int BISECTION_STEPS = 40;
constexpr qreal MAX_POPIN_PERCENT = 1000;

const auto HYPRLAND_TIMING = PanelAnimationTiming {
    .showMotionMs = 270,
    .showFadeMs = 50,
    .hideMotionMs = 240,
    .hideFadeMs = 240,
    .showMotion = CubicBezier(0.05, 0.7, 0.1, 1),
    .showFade = CubicBezier(0.1, 1, 0, 1),
    .hideMotion = CubicBezier(0.52, 0.03, 0.72, 0.08),
    .hideFade = CubicBezier(1, -0.1, 0.7, 0.85),
};

std::optional<Qt::Edge> edgeFromName(const QString& name) {
	if (name == "top") return Qt::TopEdge;
	if (name == "bottom") return Qt::BottomEdge;
	if (name == "left") return Qt::LeftEdge;
	if (name == "right") return Qt::RightEdge;
	return std::nullopt;
}

QString edgeName(Qt::Edge edge) {
	switch (edge) {
	case Qt::TopEdge: return "top";
	case Qt::BottomEdge: return "bottom";
	case Qt::LeftEdge: return "left";
	case Qt::RightEdge: return "right";
	}

	return QString();
}

qreal snap(qreal value, qreal dpr) {
	if (dpr <= 0) return value;
	return std::round(value * dpr) / dpr;
}

} // namespace

std::optional<PanelAnimationStyle> PanelAnimationStyle::parse(const QString& text) {
	auto words = text.simplified().toLower().split(' ', Qt::SkipEmptyParts);
	if (words.isEmpty()) return std::nullopt;

	auto style = PanelAnimationStyle();
	const auto& name = words.first();

	if (name == "noanim" || name == "none") {
		if (words.size() != 1) return std::nullopt;
		return style;
	}

	if (name == "fade") {
		if (words.size() != 1) return std::nullopt;
		style.kind = Kind::Fade;
		return style;
	}

	if (name == "slide") {
		style.kind = Kind::Slide;
		if (words.size() == 1) return style;
		if (words.size() != 2) return std::nullopt;

		style.edge = edgeFromName(words.at(1));
		if (!style.edge.has_value()) return std::nullopt;
		return style;
	}

	if (name == "popin") {
		style.kind = Kind::Popin;
		if (words.size() == 1) return style;
		if (words.size() != 2) return std::nullopt;

		auto number = words.at(1);
		if (number.endsWith('%')) number.chop(1);

		auto ok = false;
		auto percent = number.toDouble(&ok);
		if (!ok || !std::isfinite(percent) || percent < 0) return std::nullopt;

		style.popinScale = std::min(percent, MAX_POPIN_PERCENT) / 100.0;
		return style;
	}

	return std::nullopt;
}

QString PanelAnimationStyle::toString() const {
	switch (this->kind) {
	case Kind::None: return "noAnim";
	case Kind::Fade: return "fade";
	case Kind::Slide: return this->edge.has_value() ? "slide " + edgeName(*this->edge) : "slide";
	case Kind::Popin: return QString("popin %1%").arg(this->popinScale * 100.0);
	}

	return QString();
}

qreal CubicBezier::xAt(qreal t) const {
	auto cx = 3.0 * this->mX1;
	auto bx = 3.0 * (this->mX2 - this->mX1) - cx;
	auto ax = 1.0 - cx - bx;
	return ((ax * t + bx) * t + cx) * t;
}

qreal CubicBezier::yAt(qreal t) const {
	auto cy = 3.0 * this->mY1;
	auto by = 3.0 * (this->mY2 - this->mY1) - cy;
	auto ay = 1.0 - cy - by;
	return ((ay * t + by) * t + cy) * t;
}

qreal CubicBezier::xSlopeAt(qreal t) const {
	auto cx = 3.0 * this->mX1;
	auto bx = 3.0 * (this->mX2 - this->mX1) - cx;
	auto ax = 1.0 - cx - bx;
	return (3.0 * ax * t + 2.0 * bx) * t + cx;
}

qreal CubicBezier::valueAt(qreal progress) const {
	if (progress <= 0) return 0;
	if (progress >= 1) return 1;

	auto t = progress;
	for (auto i = 0; i < NEWTON_STEPS; i++) {
		auto error = this->xAt(t) - progress;
		if (std::abs(error) < SOLVE_EPSILON) return this->yAt(t);

		auto slope = this->xSlopeAt(t);
		if (std::abs(slope) < SOLVE_EPSILON) break;
		t -= error / slope;
	}

	auto low = 0.0;
	auto high = 1.0;
	t = progress;

	for (auto i = 0; i < BISECTION_STEPS; i++) {
		auto x = this->xAt(t);
		if (std::abs(x - progress) < SOLVE_EPSILON) break;
		if (x < progress) low = t;
		else high = t;
		t = (low + high) / 2.0;
	}

	return this->yAt(t);
}

qreal PanelAnimationTrack::valueAt(qreal elapsed) const {
	if (this->duration <= 0 || elapsed >= this->duration) return this->to;
	if (elapsed <= 0) return this->from;
	return this->from + (this->to - this->from) * this->curve.valueAt(elapsed / this->duration);
}

const PanelAnimationTiming& PanelAnimationTiming::hyprland() { return HYPRLAND_TIMING; }

void PanelAnimation::start(bool show, const PanelAnimationStyle& style) {
	const auto& timing = PanelAnimationTiming::hyprland();
	auto target = show ? 1.0 : 0.0;
	auto motionMs = show ? timing.showMotionMs : timing.hideMotionMs;
	auto fadeMs = show ? timing.showFadeMs : timing.hideFadeMs;

	this->mShowing = show;
	if (!style.moves()) this->mMotion = target;

	this->motionTrack = PanelAnimationTrack {
	    .from = this->mMotion,
	    .to = target,
	    .duration = std::abs(target - this->mMotion) * motionMs,
	    .curve = show ? timing.showMotion : timing.hideMotion,
	};

	this->opacityTrack = PanelAnimationTrack {
	    .from = this->mOpacity,
	    .to = target,
	    .duration = std::abs(target - this->mOpacity) * fadeMs,
	    .curve = show ? timing.showFade : timing.hideFade,
	};
}

bool PanelAnimation::advance(qreal elapsed) {
	this->mMotion = this->motionTrack.valueAt(elapsed);
	this->mOpacity = this->opacityTrack.valueAt(elapsed);
	return elapsed >= this->duration();
}

void PanelAnimation::jumpTo(bool shown) {
	auto target = shown ? 1.0 : 0.0;
	this->mShowing = shown;
	this->mMotion = target;
	this->mOpacity = target;
	this->motionTrack = PanelAnimationTrack {.from = target, .to = target};
	this->opacityTrack = PanelAnimationTrack {.from = target, .to = target};
}

qreal PanelAnimation::duration() const {
	return std::max(this->motionTrack.duration, this->opacityTrack.duration);
}

bool PanelAnimation::settled() const {
	auto target = this->mShowing ? 1.0 : 0.0;
	return this->mMotion == target && this->mOpacity == target;
}

bool PanelAnimationFrame::isIdentity() const {
	return this->offset.isNull() && this->scale == 1 && this->opacity >= 1;
}

Qt::Edge panelSlideEdge(
    std::optional<Qt::Edge> forced,
    const QRectF& panel,
    const QRectF& screen,
    Qt::Edges anchors
) {
	if (forced.has_value()) return *forced;

	struct Candidate {
		Qt::Edge edge;
		qreal distance;
	};

	auto center = panel.center();
	auto candidates = std::array<Candidate, 4> {{
	    {.edge = Qt::TopEdge, .distance = center.y() - screen.top()},
	    {.edge = Qt::BottomEdge, .distance = screen.bottom() - center.y()},
	    {.edge = Qt::LeftEdge, .distance = center.x() - screen.left()},
	    {.edge = Qt::RightEdge, .distance = screen.right() - center.x()},
	}};

	auto best = Qt::TopEdge;
	auto bestDistance = std::numeric_limits<qreal>::max();

	for (const auto& candidate: candidates) {
		if (anchors != Qt::Edges() && !anchors.testFlag(candidate.edge)) continue;
		if (candidate.distance >= bestDistance) continue;
		best = candidate.edge;
		bestDistance = candidate.distance;
	}

	return best;
}

PanelAnimationFrame panelAnimationFrame(
    const PanelAnimationStyle& style,
    Qt::Edge edge,
    const QSizeF& size,
    qreal motion,
    qreal opacity,
    qreal dpr
) {
	auto frame = PanelAnimationFrame();
	frame.opacity = std::clamp(opacity, 0.0, 1.0);
	auto hidden = 1.0 - std::clamp(motion, 0.0, 1.0);

	switch (style.kind) {
	case PanelAnimationStyle::Kind::Slide:
		switch (edge) {
		case Qt::LeftEdge: frame.offset.setX(snap(-hidden * size.width(), dpr)); break;
		case Qt::RightEdge: frame.offset.setX(snap(hidden * size.width(), dpr)); break;
		case Qt::TopEdge: frame.offset.setY(snap(-hidden * size.height(), dpr)); break;
		case Qt::BottomEdge: frame.offset.setY(snap(hidden * size.height(), dpr)); break;
		}
		break;
	case PanelAnimationStyle::Kind::Popin:
		frame.scale = style.popinScale + (1.0 - style.popinScale) * (1.0 - hidden);
		break;
	case PanelAnimationStyle::Kind::Fade:
	case PanelAnimationStyle::Kind::None: break;
	}

	return frame;
}

void PanelContentTransform::setFrame(const PanelAnimationFrame& frame, const QSizeF& size) {
	auto center = QPointF(size.width() / 2.0, size.height() / 2.0);
	if (frame.offset == this->mOffset && frame.scale == this->mScale && center == this->mOrigin) {
		return;
	}

	this->mOffset = frame.offset;
	this->mScale = frame.scale;
	this->mOrigin = center;
	this->update();
}

void PanelContentTransform::applyTo(QMatrix4x4* matrix) const {
	if (this->mBypassed || (this->mOffset.isNull() && this->mScale == 1)) return;

	matrix->translate(
	    static_cast<float>(this->mOffset.x() + this->mOrigin.x()),
	    static_cast<float>(this->mOffset.y() + this->mOrigin.y())
	);

	if (this->mScale != 1) {
		matrix->scale(static_cast<float>(this->mScale), static_cast<float>(this->mScale));
	}

	matrix->translate(static_cast<float>(-this->mOrigin.x()), static_cast<float>(-this->mOrigin.y()));
}

} // namespace qs::windows
