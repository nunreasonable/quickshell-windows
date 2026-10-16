#include <cmath>
#include <optional>

#include <qbytearray.h>
#include <qfile.h>
#include <qmatrix4x4.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qpoint.h>
#include <qqmlcomponent.h>
#include <qqmlengine.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qrect.h>
#include <qsize.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtenvironmentvariables.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qurl.h>

#include "layer_rules.hpp"
#include "panel_animation.hpp"

using qs::windows::CubicBezier;
using qs::windows::LayerRules;
using qs::windows::PanelAnimation;
using qs::windows::PanelAnimationStyle;
using qs::windows::PanelAnimationTiming;
using qs::windows::PanelContentTransform;
using Kind = qs::windows::PanelAnimationStyle::Kind;

namespace {

bool near(qreal a, qreal b, qreal tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

qreal effectiveOpacity(const QQuickItem* item) {
	auto opacity = 1.0;
	for (; item != nullptr; item = item->parentItem()) opacity *= item->opacity();
	return opacity;
}

qreal sampledBezier(qreal x1, qreal y1, qreal x2, qreal y2, qreal x) {
	auto bestT = 0.0;
	auto bestError = 2.0;

	for (auto i = 0; i <= 200000; i++) {
		auto t = i / 200000.0;
		auto u = 1.0 - t;
		auto bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
		auto error = std::abs(bx - x);
		if (error < bestError) {
			bestError = error;
			bestT = t;
		}
	}

	auto u = 1.0 - bestT;
	return 3 * u * u * bestT * y1 + 3 * u * bestT * bestT * y2 + bestT * bestT * bestT;
}

PanelAnimationStyle style(Kind kind, std::optional<Qt::Edge> edge = std::nullopt, qreal scale = 0) {
	auto result = PanelAnimationStyle();
	result.kind = kind;
	result.edge = edge;
	result.popinScale = scale;
	return result;
}

} // namespace

class TestPanelAnimation: public QObject {
	Q_OBJECT;

private slots:
	void parseStyles();
	void stylesRoundTrip();
	void bezierMatchesSampling();
	void showAndHideTimeline();
	void fadeOnlyTimeline();
	void reverseMidway();
	void frames();
	void slideEdges();
	void contentTransform();
	void maskBypass();
	void overlayFollowsRoot();
	void ruleMerging();
	void iiRules();
};

void TestPanelAnimation::parseStyles() {
	QCOMPARE(PanelAnimationStyle::parse("slide"), style(Kind::Slide));
	QCOMPARE(PanelAnimationStyle::parse("slide left"), style(Kind::Slide, Qt::LeftEdge));
	QCOMPARE(PanelAnimationStyle::parse("  Slide   RIGHT "), style(Kind::Slide, Qt::RightEdge));
	QCOMPARE(PanelAnimationStyle::parse("slide top"), style(Kind::Slide, Qt::TopEdge));
	QCOMPARE(PanelAnimationStyle::parse("slide bottom"), style(Kind::Slide, Qt::BottomEdge));
	QCOMPARE(PanelAnimationStyle::parse("popin"), style(Kind::Popin, std::nullopt, 0));
	QCOMPARE(PanelAnimationStyle::parse("popin 93%"), style(Kind::Popin, std::nullopt, 0.93));
	QCOMPARE(PanelAnimationStyle::parse("popin 120%"), style(Kind::Popin, std::nullopt, 1.2));
	QCOMPARE(PanelAnimationStyle::parse("popin 80"), style(Kind::Popin, std::nullopt, 0.8));
	QCOMPARE(PanelAnimationStyle::parse("fade"), style(Kind::Fade));
	QCOMPARE(PanelAnimationStyle::parse("noAnim"), style(Kind::None));
	QCOMPARE(PanelAnimationStyle::parse("none"), style(Kind::None));

	QVERIFY(!PanelAnimationStyle::parse("").has_value());
	QVERIFY(!PanelAnimationStyle::parse("slide up").has_value());
	QVERIFY(!PanelAnimationStyle::parse("slide left now").has_value());
	QVERIFY(!PanelAnimationStyle::parse("popin -5%").has_value());
	QVERIFY(!PanelAnimationStyle::parse("popin big").has_value());
	QVERIFY(!PanelAnimationStyle::parse("fade 50").has_value());
	QVERIFY(!PanelAnimationStyle::parse("slidevert").has_value());
	QVERIFY(!PanelAnimationStyle::parse("bounce").has_value());
}

void TestPanelAnimation::stylesRoundTrip() {
	const auto texts = {
	    "slide",
	    "slide left",
	    "slide right",
	    "slide top",
	    "slide bottom",
	    "popin 93%",
	    "popin 120%",
	    "fade",
	    "noAnim",
	};

	for (const auto* text: texts) {
		auto parsed = PanelAnimationStyle::parse(text);
		QVERIFY2(parsed.has_value(), text);
		QCOMPARE(parsed->toString(), QString(text));
	}
}

void TestPanelAnimation::bezierMatchesSampling() {
	struct Curve {
		qreal x1, y1, x2, y2;
	};

	const auto curves = {
	    Curve {0.05, 0.7, 0.1, 1},
	    Curve {0.1, 1, 0, 1},
	    Curve {0.52, 0.03, 0.72, 0.08},
	    Curve {1, -0.1, 0.7, 0.85},
	    Curve {0, 0, 1, 1},
	};

	for (const auto& curve: curves) {
		auto bezier = CubicBezier(curve.x1, curve.y1, curve.x2, curve.y2);
		QCOMPARE(bezier.valueAt(0), 0.0);
		QCOMPARE(bezier.valueAt(1), 1.0);

		for (auto i = 1; i < 40; i++) {
			auto x = i / 40.0;
			auto got = bezier.valueAt(x);
			auto want = sampledBezier(curve.x1, curve.y1, curve.x2, curve.y2, x);
			auto message = QString("bezier(%1, %2, %3, %4) at %5: %6, want %7")
			                   .arg(curve.x1)
			                   .arg(curve.y1)
			                   .arg(curve.x2)
			                   .arg(curve.y2)
			                   .arg(x)
			                   .arg(got)
			                   .arg(want);
			QVERIFY2(near(got, want, 2e-3), qPrintable(message));
		}
	}

	auto decel = CubicBezier(0.05, 0.7, 0.1, 1);
	auto previous = 0.0;
	for (auto i = 1; i <= 100; i++) {
		auto value = decel.valueAt(i / 100.0);
		QVERIFY(value >= previous - 1e-9);
		previous = value;
	}
}

void TestPanelAnimation::showAndHideTimeline() {
	const auto& timing = PanelAnimationTiming::hyprland();
	auto slide = style(Kind::Slide, Qt::RightEdge);
	auto animation = PanelAnimation();

	animation.jumpTo(false);
	QCOMPARE(animation.motion(), 0.0);
	QCOMPARE(animation.opacity(), 0.0);

	animation.start(true, slide);
	QCOMPARE(animation.duration(), timing.showMotionMs);
	QVERIFY(!animation.advance(0));
	QCOMPARE(animation.motion(), 0.0);
	QCOMPARE(animation.opacity(), 0.0);

	QVERIFY(!animation.advance(timing.showFadeMs));
	QCOMPARE(animation.opacity(), 1.0);
	QVERIFY(animation.motion() > 0.5 && animation.motion() < 1.0);

	QVERIFY(animation.advance(timing.showMotionMs));
	QCOMPARE(animation.motion(), 1.0);
	QVERIFY(animation.settled());

	animation.start(false, slide);
	QCOMPARE(animation.duration(), timing.hideMotionMs);
	QVERIFY(!animation.advance(timing.hideMotionMs / 2));
	QVERIFY(animation.motion() > 0.5);
	QVERIFY(animation.opacity() > 0.9);
	QVERIFY(animation.advance(timing.hideMotionMs));
	QCOMPARE(animation.motion(), 0.0);
	QCOMPARE(animation.opacity(), 0.0);
	QVERIFY(!animation.showing());
}

void TestPanelAnimation::fadeOnlyTimeline() {
	const auto& timing = PanelAnimationTiming::hyprland();
	auto animation = PanelAnimation();
	animation.jumpTo(false);

	animation.start(true, style(Kind::Fade));
	QCOMPARE(animation.duration(), timing.showFadeMs);
	QCOMPARE(animation.motion(), 1.0);
	QVERIFY(animation.advance(timing.showFadeMs));

	animation.start(false, style(Kind::Fade));
	QCOMPARE(animation.duration(), timing.hideFadeMs);
	QCOMPARE(animation.motion(), 0.0);
}

void TestPanelAnimation::reverseMidway() {
	const auto& timing = PanelAnimationTiming::hyprland();
	auto slide = style(Kind::Slide, Qt::LeftEdge);
	auto animation = PanelAnimation();
	animation.jumpTo(false);

	animation.start(true, slide);
	animation.advance(30);
	auto motion = animation.motion();
	auto opacity = animation.opacity();
	QVERIFY(motion > 0 && motion < 1);

	animation.start(false, slide);
	QVERIFY(!animation.advance(0));
	QCOMPARE(animation.motion(), motion);
	QCOMPARE(animation.opacity(), opacity);
	QVERIFY(near(animation.duration(), timing.hideMotionMs * std::max(motion, opacity), 1e-6));

	animation.advance(10);
	auto closing = animation.motion();
	QVERIFY(closing <= motion);

	animation.start(true, slide);
	QVERIFY(!animation.advance(0));
	QCOMPARE(animation.motion(), closing);
	QVERIFY(near(animation.duration(), timing.showMotionMs * (1 - closing), 1e-6));
	QVERIFY(animation.advance(animation.duration()));
	QVERIFY(animation.settled());
	QVERIFY(animation.showing());
}

void TestPanelAnimation::frames() {
	auto size = QSizeF(400, 1000);

	using qs::windows::panelAnimationFrame;

	auto right = panelAnimationFrame(style(Kind::Slide, Qt::RightEdge), Qt::RightEdge, size, 0, 0, 1);
	QCOMPARE(right.offset, QPointF(400, 0));
	QCOMPARE(right.opacity, 0.0);
	QCOMPARE(right.scale, 1.0);

	auto half = panelAnimationFrame(style(Kind::Slide), Qt::LeftEdge, size, 0.5, 1, 1);
	QCOMPARE(half.offset, QPointF(-200, 0));

	auto top = panelAnimationFrame(style(Kind::Slide), Qt::TopEdge, size, 0.25, 1, 1);
	QCOMPARE(top.offset, QPointF(0, -750));

	auto odd = QSizeF(100, 101);
	auto bottom = panelAnimationFrame(style(Kind::Slide), Qt::BottomEdge, odd, 0.5, 1, 1.5);
	QVERIFY(near(bottom.offset.y() * 1.5, std::round(bottom.offset.y() * 1.5), 1e-9));
	QVERIFY(near(bottom.offset.y(), 50.5, 0.34));

	auto shown = panelAnimationFrame(style(Kind::Slide), Qt::BottomEdge, size, 1, 1, 1);
	QVERIFY(shown.isIdentity());

	auto popin =
	    panelAnimationFrame(style(Kind::Popin, std::nullopt, 0.93), Qt::TopEdge, size, 0, 0.5, 1);
	QVERIFY(popin.offset.isNull());
	QVERIFY(near(popin.scale, 0.93, 1e-9));
	QCOMPARE(popin.opacity, 0.5);

	auto grow =
	    panelAnimationFrame(style(Kind::Popin, std::nullopt, 1.2), Qt::TopEdge, size, 0.5, 1, 1);
	QVERIFY(near(grow.scale, 1.1, 1e-9));

	auto fade = panelAnimationFrame(style(Kind::Fade), Qt::TopEdge, size, 0, 0.25, 1);
	QVERIFY(fade.offset.isNull());
	QCOMPARE(fade.scale, 1.0);
	QCOMPARE(fade.opacity, 0.25);

	auto clamped = panelAnimationFrame(style(Kind::Fade), Qt::TopEdge, size, 1, 1.05, 1);
	QCOMPARE(clamped.opacity, 1.0);
}

void TestPanelAnimation::slideEdges() {
	using qs::windows::panelSlideEdge;
	auto screen = QRectF(0, 0, 1920, 1080);
	auto second = QRectF(1920, 0, 1280, 1024);
	auto top = Qt::Edges(Qt::TopEdge);
	auto topLeft = Qt::TopEdge | Qt::LeftEdge;
	auto horizontalTop = Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge;
	auto horizontalBottom = Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge;
	auto verticalLeft = Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge;
	auto verticalRight = Qt::TopEdge | Qt::BottomEdge | Qt::RightEdge;
	auto all = Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge;
	auto none = std::optional<Qt::Edge>();

	QCOMPARE(panelSlideEdge(none, QRectF(0, 0, 1920, 40), screen, horizontalTop), Qt::TopEdge);
	QCOMPARE(
	    panelSlideEdge(none, QRectF(0, 1040, 1920, 40), screen, horizontalBottom),
	    Qt::BottomEdge
	);
	QCOMPARE(panelSlideEdge(none, QRectF(0, 0, 50, 1080), screen, verticalLeft), Qt::LeftEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(1460, 0, 460, 1080), screen, verticalRight), Qt::RightEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(860, 0, 200, 60), screen, top), Qt::TopEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(10, 10, 300, 80), screen, topLeft), Qt::TopEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(10, 10, 80, 300), screen, topLeft), Qt::LeftEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(560, 140, 800, 800), screen, Qt::Edges()), Qt::TopEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(0, 0, 1920, 1080), screen, all), Qt::TopEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(2740, 0, 460, 1024), second, verticalRight), Qt::RightEdge);
	QCOMPARE(panelSlideEdge(none, QRectF(1920, 0, 460, 1024), second, verticalLeft), Qt::LeftEdge);
	QCOMPARE(
	    panelSlideEdge(Qt::BottomEdge, QRectF(0, 0, 50, 1080), screen, verticalLeft),
	    Qt::BottomEdge
	);
}

void TestPanelAnimation::contentTransform() {
	auto transform = PanelContentTransform();
	auto size = QSizeF(200, 100);

	auto frame = qs::windows::PanelAnimationFrame();
	frame.offset = QPointF(100, -20);
	transform.setFrame(frame, size);

	auto matrix = QMatrix4x4();
	transform.applyTo(&matrix);
	QCOMPARE(matrix.map(QPointF(0, 0)), QPointF(100, -20));
	QCOMPARE(matrix.map(QPointF(200, 100)), QPointF(300, 80));

	frame.offset = QPointF();
	frame.scale = 0.5;
	transform.setFrame(frame, size);
	matrix = QMatrix4x4();
	transform.applyTo(&matrix);
	QCOMPARE(matrix.map(QPointF(0, 0)), QPointF(50, 25));
	QCOMPARE(matrix.map(QPointF(100, 50)), QPointF(100, 50));
	QCOMPARE(matrix.map(QPointF(200, 100)), QPointF(150, 75));

	transform.setBypassed(true);
	matrix = QMatrix4x4();
	transform.applyTo(&matrix);
	QVERIFY(matrix.isIdentity());
}

void TestPanelAnimation::maskBypass() {
	auto window = QQuickWindow();
	window.resize(400, 300);

	auto content = QQuickItem();
	content.setParentItem(window.contentItem());
	content.setSize(QSizeF(400, 300));

	auto child = QQuickItem();
	child.setParentItem(&content);
	child.setPosition(QPointF(10, 20));
	child.setSize(QSizeF(50, 60));

	QCOMPARE(child.mapToScene(QPointF(0, 0)), QPointF(10, 20));

	{
		auto transform = PanelContentTransform();
		transform.appendToItem(&content);

		auto frame = qs::windows::PanelAnimationFrame();
		frame.offset = QPointF(400, 0);
		transform.setFrame(frame, content.size());
		QCOMPARE(child.mapToScene(QPointF(0, 0)), QPointF(410, 20));

		transform.setBypassed(true);
		QCOMPARE(child.mapToScene(QPointF(0, 0)), QPointF(10, 20));
		transform.setBypassed(false);
		QCOMPARE(child.mapToScene(QPointF(0, 0)), QPointF(410, 20));
	}

	QCOMPARE(child.mapToScene(QPointF(0, 0)), QPointF(10, 20));
}

void TestPanelAnimation::overlayFollowsRoot() {
	auto engine = QQmlEngine();
	auto component = QQmlComponent(&engine);
	component.setData(
	    R"(
		import QtQuick
		import QtQuick.Controls
		Window {
			width: 400
			height: 300
			visible: true
			property Item content: content
			property Item anchorItem: anchorItem
			property Popup popup: popup
			Item {
				id: content
				anchors.fill: parent
				Item { id: anchorItem; x: 10; y: 20; width: 50; height: 60 }
				Popup { id: popup; parent: anchorItem; x: 5; y: 5; width: 30; height: 30; visible: true }
			}
		}
	)",
	    QUrl()
	);

	auto* object = component.create();
	QVERIFY2(object != nullptr, qPrintable(component.errorString()));
	auto* window = qobject_cast<QQuickWindow*>(object);
	QVERIFY(window != nullptr);
	QVERIFY(QTest::qWaitForWindowExposed(window));

	auto* root = window->contentItem();
	auto* content = object->property("content").value<QQuickItem*>();
	auto* anchorItem = object->property("anchorItem").value<QQuickItem*>();
	auto* popup = object->property("popup").value<QObject*>();
	QVERIFY(content != nullptr && anchorItem != nullptr && popup != nullptr);
	QTRY_VERIFY(popup->property("opened").toBool());

	auto* popupContent = popup->property("contentItem").value<QQuickItem*>();
	QVERIFY(popupContent != nullptr);

	auto underContent = false;
	auto underRoot = false;
	for (auto* item = popupContent->parentItem(); item != nullptr; item = item->parentItem()) {
		if (item == content) underContent = true;
		if (item == root) underRoot = true;
	}

	QVERIFY(!underContent);
	QVERIFY(underRoot);

	auto popupBefore = popupContent->mapToScene(QPointF(0, 0));
	auto anchorBefore = anchorItem->mapToScene(QPointF(0, 0));
	auto relativeBefore = content->mapFromItem(anchorItem, QPointF(0, 0));

	{
		auto transform = PanelContentTransform();
		transform.appendToItem(content);

		auto frame = qs::windows::PanelAnimationFrame();
		frame.offset = QPointF(400, 0);
		transform.setFrame(frame, QSizeF(window->size()));

		QCOMPARE(anchorItem->mapToScene(QPointF(0, 0)), anchorBefore + QPointF(400, 0));
		QCOMPARE(popupContent->mapToScene(QPointF(0, 0)), popupBefore);
	}

	{
		auto transform = PanelContentTransform();
		transform.appendToItem(root);

		auto frame = qs::windows::PanelAnimationFrame();
		frame.offset = QPointF(400, 0);
		transform.setFrame(frame, QSizeF(window->size()));

		QCOMPARE(popupContent->mapToScene(QPointF(0, 0)), popupBefore + QPointF(400, 0));
		QCOMPARE(anchorItem->mapToScene(QPointF(0, 0)), anchorBefore + QPointF(400, 0));
		QCOMPARE(content->mapFromItem(anchorItem, QPointF(0, 0)), relativeBefore);

		transform.setBypassed(true);
		QCOMPARE(anchorItem->mapToScene(QPointF(0, 0)), anchorBefore);
		QCOMPARE(popupContent->mapToScene(QPointF(0, 0)), popupBefore);
		transform.setBypassed(false);

		root->setOpacity(0.25);
		QVERIFY(qFuzzyCompare(effectiveOpacity(popupContent), 0.25));
		root->setOpacity(1);
	}

	QCOMPARE(anchorItem->mapToScene(QPointF(0, 0)), anchorBefore);
	delete object;
}

void TestPanelAnimation::ruleMerging() {
	auto data = QByteArray(R"({
		"enable": false,
		"layered": true,
		"rules": [
			{"namespace": ".*", "animation": "popin 93%", "blur": true},
			{"namespace": "quickshell:.*", "ignoreAlpha": 0.79},
			{"namespace": "quickshell:bar", "animation": "slide"},
			{"namespace": "quickshell:session", "animation": null},
			{"namespace": "quickshell:overview", "animation": "noAnim", "ignoreAlpha": null},
			{"namespace": "quickshell:bad", "animation": "wobble"},
			{"namespace": "quickshell:number", "animation": 3},
			{"namespace": "quickshell:(", "animation": "fade"},
			{"animation": "fade"}
		]
	})");

	auto warnings = QStringList();
	auto rules = LayerRules::parse(data, "test.json", &warnings);
	QVERIFY(rules.valid());
	QVERIFY(!rules.enabled());
	QVERIFY(rules.layered());
	QCOMPARE(rules.size(), qsizetype(7));
	QCOMPARE(warnings.size(), qsizetype(4));

	QCOMPARE(rules.ruleFor("quickshell:bar").animation, style(Kind::Slide));
	QCOMPARE(rules.ruleFor("quickshell:bar").ignoreAlpha, std::optional<qreal>(0.79));
	QVERIFY(rules.ruleFor("quickshell:bar").blur);
	QCOMPARE(rules.ruleFor("quickshell:barx").animation, style(Kind::Popin, std::nullopt, 0.93));
	QCOMPARE(rules.ruleFor("xquickshell:bar").animation, style(Kind::Popin, std::nullopt, 0.93));
	QCOMPARE(rules.ruleFor("quickshell:session").animation, style(Kind::None));
	QCOMPARE(rules.ruleFor("quickshell:overview").animation, style(Kind::None));
	QVERIFY(!rules.ruleFor("quickshell:overview").ignoreAlpha.has_value());
	QCOMPARE(rules.ruleFor("quickshell:bad").animation, style(Kind::Popin, std::nullopt, 0.93));
	QCOMPARE(rules.ruleFor("quickshell:number").animation, style(Kind::Popin, std::nullopt, 0.93));

	auto info = rules.info();
	QCOMPARE(info.size(), qsizetype(7));
	QCOMPARE(info.at(0).toMap().value("animation").toString(), QString("popin 93%"));
	QCOMPARE(info.at(3).toMap().value("animation").toString(), QString("noAnim"));

	auto broken = LayerRules::parse("{", "broken.json", &warnings);
	QVERIFY(!broken.valid());
	QVERIFY(broken.enabled());
	QCOMPARE(broken.ruleFor("quickshell:bar").animation, style(Kind::None));

	auto empty = LayerRules::parse(QByteArray(), "empty.json");
	QVERIFY(!empty.valid());
	QCOMPARE(empty.ruleFor("anything").animation, style(Kind::None));
}

void TestPanelAnimation::iiRules() {
	auto path = qEnvironmentVariable("IIW_LAYER_RULES");
	if (path.isEmpty()) QSKIP("IIW_LAYER_RULES is not set to ii's defaults/windows/layerrules.json");

	auto file = QFile(path);
	QVERIFY2(file.open(QFile::ReadOnly), qPrintable(path));

	auto warnings = QStringList();
	auto rules = LayerRules::parse(file.readAll(), path, &warnings);
	QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
	QVERIFY(rules.valid());
	QVERIFY(rules.enabled());

	struct Expect {
		const char* ns;
		PanelAnimationStyle animation;
	};

	const auto expected = {
	    Expect {"quickshell:sidebarRight", style(Kind::Slide, Qt::RightEdge)},
	    Expect {"quickshell:sidebarLeft", style(Kind::Slide, Qt::LeftEdge)},
	    Expect {"quickshell:bar", style(Kind::Slide)},
	    Expect {"quickshell:verticalBar", style(Kind::Slide)},
	    Expect {"quickshell:dock", style(Kind::Slide, Qt::BottomEdge)},
	    Expect {"quickshell:cheatsheet", style(Kind::Slide, Qt::BottomEdge)},
	    Expect {"quickshell:osk", style(Kind::Slide, Qt::BottomEdge)},
	    Expect {"quickshell:wallpaperSelector", style(Kind::Slide, Qt::TopEdge)},
	    Expect {"quickshell:reloadPopup", style(Kind::Slide)},
	    Expect {"quickshell:screenCorners", style(Kind::Popin, std::nullopt, 1.2)},
	    Expect {"quickshell:notificationPopup", style(Kind::Fade)},
	    Expect {"quickshell:overview", style(Kind::None)},
	    Expect {"quickshell:session", style(Kind::None)},
	    Expect {"quickshell:overlay", style(Kind::None)},
	    Expect {"quickshell:regionSelector", style(Kind::None)},
	    Expect {"quickshell:screenshot", style(Kind::None)},
	    Expect {"quickshell:polkit", style(Kind::None)},
	    Expect {"quickshell:actionCenter", style(Kind::None)},
	    Expect {"quickshell:wStartMenu", style(Kind::None)},
	    Expect {"quickshell:wTaskView", style(Kind::None)},
	    Expect {"quickshell:popup", style(Kind::None)},
	    Expect {"quickshell:background", style(Kind::None)},
	    Expect {"quickshell:spotlight", style(Kind::None)},
	    Expect {"osk", style(Kind::None)},
	    Expect {"sideright", style(Kind::Slide, Qt::RightEdge)},
	};

	for (const auto& expect: expected) {
		auto got = rules.ruleFor(expect.ns).animation;
		auto message =
		    QString("%1: %2, want %3").arg(expect.ns, got.toString(), expect.animation.toString());
		QVERIFY2(got == expect.animation, qPrintable(message));
	}

	QVERIFY(rules.ruleFor("quickshell:sidebarRight").blur);
	QCOMPARE(rules.ruleFor("quickshell:sidebarRight").ignoreAlpha, std::optional<qreal>(0.79));
	QVERIFY(!rules.ruleFor("quickshell:background").blur);
	QCOMPARE(rules.ruleFor("quickshell:overlay").ignoreAlpha, std::optional<qreal>(1));
	QVERIFY(rules.ruleFor("quickshell:session").blur);
	QCOMPARE(rules.ruleFor("quickshell:session").ignoreAlpha, std::optional<qreal>(0));
	QCOMPARE(rules.ruleFor("quickshell:wTaskView").ignoreAlpha, std::optional<qreal>(0));
	QCOMPARE(rules.ruleFor("quickshell:popup").ignoreAlpha, std::optional<qreal>(1));
	QCOMPARE(rules.ruleFor("bar0").ignoreAlpha, std::optional<qreal>(0.6));
}

QTEST_MAIN(TestPanelAnimation)

#include "panel_animation_test.moc"
