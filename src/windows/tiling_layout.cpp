#include "tiling_layout.hpp"
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include <qlist.h>
#include <qpoint.h>
#include <qrect.h>
#include <qset.h>
#include <qtypes.h>

namespace qs::windows::tiling {

namespace {

constexpr double MIN_RATIO = 0.05;
constexpr double MAX_RATIO = 0.95;

double clampRatio(double ratio) { return qBound(MIN_RATIO, ratio, MAX_RATIO); }

bool isVerticalEdge(Edge edge) { return edge == Edge::Left || edge == Edge::Right; }

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

Edge opposite(Edge edge) {
	switch (edge) {
	case Edge::Left: return Edge::Right;
	case Edge::Right: return Edge::Left;
	case Edge::Top: return Edge::Bottom;
	case Edge::Bottom: return Edge::Top;
	}
	return edge;
}

int edgeOf(const QRect& box, Edge edge) {
	switch (edge) {
	case Edge::Left: return box.left();
	case Edge::Right: return rightOf(box);
	case Edge::Top: return box.top();
	case Edge::Bottom: return bottomOf(box);
	}
	return 0;
}

} // namespace

std::unique_ptr<LayoutEngine> LayoutEngine::create(Kind kind) {
	if (kind == Kind::Master) return std::make_unique<MasterLayout>();
	return std::make_unique<DwindleLayout>();
}

qsizetype DwindleLayout::count() const { return this->ids().length(); }

QList<DwindleLayout::Id> DwindleLayout::ids() const {
	QList<Id> out;
	if (this->root != nullptr) DwindleLayout::collect(this->root.get(), out);
	return out;
}

void DwindleLayout::collect(const Node* node, QList<Id>& out) {
	if (node->isLeaf()) {
		out.append(node->id);
		return;
	}

	DwindleLayout::collect(node->first.get(), out);
	DwindleLayout::collect(node->second.get(), out);
}

DwindleLayout::Node* DwindleLayout::find(Id id) const {
	if (this->root == nullptr) return nullptr;

	QList<Node*> stack {this->root.get()};
	while (!stack.isEmpty()) {
		auto* node = stack.takeLast();
		if (node->isLeaf()) {
			if (node->id == id) return node;
			continue;
		}

		stack.append(node->second.get());
		stack.append(node->first.get());
	}

	return nullptr;
}

DwindleLayout::Node* DwindleLayout::lastLeaf() const {
	auto* node = this->root.get();
	while (node != nullptr && !node->isLeaf()) node = node->second.get();
	return node;
}

void DwindleLayout::insert(Id id, Id target, const QPoint* cursor) {
	if (this->contains(id)) return;

	auto leaf = std::make_unique<Node>();
	leaf->id = id;

	if (this->root == nullptr) {
		this->root = std::move(leaf);
		return;
	}

	if (this->mArea.isValid()) this->compute(this->mArea);

	auto* split = this->find(target);
	if (split == nullptr) split = this->lastLeaf();

	auto old = std::make_unique<Node>();
	old->id = split->id;
	old->box = split->box;
	old->parent = split;
	leaf->parent = split;

	const auto box = split->box;
	split->id = 0;
	split->splitTop = box.height() > box.width();
	split->splitPinned = false;
	split->ratio = this->defaultRatio;

	auto newFirst = false;
	if (this->smartSplit && cursor != nullptr) {
		auto delta = *cursor - box.center();
		auto across = qAbs(qint64(delta.y())) * box.width();
		auto along = qAbs(qint64(delta.x())) * box.height();
		split->splitTop = across >= along;
		newFirst = split->splitTop ? delta.y() <= 0 : delta.x() <= 0;
	} else if (this->forceSplit == 1) {
		newFirst = true;
	} else if (this->forceSplit == 2) {
		newFirst = false;
	} else if (cursor != nullptr && box.contains(*cursor)) {
		newFirst = split->splitTop ? cursor->y() < box.center().y() : cursor->x() < box.center().x();
	}

	if (newFirst) {
		split->first = std::move(leaf);
		split->second = std::move(old);
	} else {
		split->first = std::move(old);
		split->second = std::move(leaf);
	}
}

void DwindleLayout::remove(Id id) {
	auto* node = this->find(id);
	if (node == nullptr) return;

	this->hidden.remove(id);

	if (node == this->root.get()) {
		this->root.reset();
		return;
	}

	auto* parent = node->parent;
	auto sibling = parent->first.get() == node ? std::move(parent->second) : std::move(parent->first);
	sibling->parent = parent->parent;

	if (parent->parent == nullptr) {
		this->root = std::move(sibling);
	} else {
		auto* grand = parent->parent;
		auto& slot = grand->first.get() == parent ? grand->first : grand->second;
		slot = std::move(sibling);
	}
}

void DwindleLayout::swap(Id a, Id b) {
	auto* nodeA = this->find(a);
	auto* nodeB = this->find(b);
	if (nodeA == nullptr || nodeB == nullptr || nodeA == nodeB) return;
	std::swap(nodeA->id, nodeB->id);
}

bool DwindleLayout::replace(Id id, Id with) {
	auto* node = this->find(id);
	if (node == nullptr) return false;
	node->id = with;
	this->hidden.remove(id);
	return true;
}

void DwindleLayout::setHidden(Id id, bool hidden) {
	if (hidden && this->contains(id)) this->hidden.insert(id);
	else this->hidden.remove(id);
}

bool DwindleLayout::hasVisible(const Node* node) const {
	if (node->isLeaf()) return !this->hidden.contains(node->id);
	return this->hasVisible(node->first.get()) || this->hasVisible(node->second.get());
}

bool DwindleLayout::toggleSplit(Id id) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;
	node->parent->splitTop = !node->parent->splitTop;
	node->parent->splitPinned = true;
	return true;
}

bool DwindleLayout::swapSplit(Id id) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;
	std::swap(node->parent->first, node->parent->second);
	return true;
}

bool DwindleLayout::splitRatio(Id id, double value, bool exact) {
	auto* node = this->find(id);
	if (node == nullptr || node->parent == nullptr) return false;

	auto* parent = node->parent;
	auto hyprland = exact ? value : parent->ratio * 2.0 + value;
	parent->ratio = clampRatio(hyprland / 2.0);
	return true;
}

DwindleLayout::Node* DwindleLayout::dividerAncestor(Node* leaf, Edge edge) const {
	auto vertical = isVerticalEdge(edge);
	auto trailing = edge == Edge::Right || edge == Edge::Bottom;

	for (Node *child = leaf, *node = leaf->parent; node != nullptr;
	     child = node, node = node->parent)
	{
		if (node->splitTop == vertical) continue;

		auto childFirst = node->first.get() == child;
		if (childFirst != trailing) continue;

		if (this->hasVisible(childFirst ? node->second.get() : node->first.get())) return node;
	}

	return nullptr;
}

void DwindleLayout::setDivider(Node* node, int position) {
	const auto& box = node->box;
	auto length = node->splitTop ? box.height() : box.width();
	if (length <= 0) return;

	auto offset = position - (node->splitTop ? box.top() : box.left());
	node->ratio = clampRatio(static_cast<double>(offset) / length);
}

bool DwindleLayout::moveEdge(Id id, Edge edge, int position) {
	auto* leaf = this->find(id);
	if (leaf == nullptr) return false;

	auto* node = this->dividerAncestor(leaf, edge);
	if (node == nullptr) return false;

	DwindleLayout::setDivider(node, position);
	return true;
}

bool DwindleLayout::hasEdge(Id id, Edge edge) const {
	auto* leaf = this->find(id);
	return leaf != nullptr && this->dividerAncestor(leaf, edge) != nullptr;
}

bool DwindleLayout::resize(Id id, int dx, int dy) {
	auto* leaf = this->find(id);
	if (leaf == nullptr) return false;

	auto changed = false;

	auto grow = [&](int delta, Edge trailing, Edge leading) {
		if (delta == 0) return;

		auto divider = [](const Node* node) {
			const auto& box = node->box;
			return node->splitTop ? box.top() + static_cast<int>(std::lround(box.height() * node->ratio))
			                      : box.left() + static_cast<int>(std::lround(box.width() * node->ratio));
		};

		if (auto* node = this->dividerAncestor(leaf, trailing)) {
			DwindleLayout::setDivider(node, divider(node) + delta);
			changed = true;
		} else if (auto* node = this->dividerAncestor(leaf, leading)) {
			DwindleLayout::setDivider(node, divider(node) - delta);
			changed = true;
		}
	};

	grow(dx, Edge::Right, Edge::Left);
	grow(dy, Edge::Bottom, Edge::Top);
	return changed;
}

void DwindleLayout::configure(const Options& options) {
	this->preserveSplit = options.preserveSplit;
	this->smartSplit = options.smartSplit;
	this->forceSplit = options.forceSplit;
	this->defaultRatio = clampRatio(options.defaultSplitRatio / 2.0);
}

void DwindleLayout::compute(const QRect& area) {
	this->mArea = area;
	if (this->root != nullptr) this->computeNode(this->root.get(), area);
}

void DwindleLayout::computeNode(Node* node, const QRect& box) const {
	if (node->isLeaf()) {
		node->box = this->hidden.contains(node->id) ? QRect() : box;
		return;
	}

	node->box = box;

	auto firstVisible = this->hasVisible(node->first.get());
	auto secondVisible = this->hasVisible(node->second.get());
	if (!firstVisible || !secondVisible) {
		this->computeNode(node->first.get(), firstVisible ? box : QRect());
		this->computeNode(node->second.get(), secondVisible ? box : QRect());
		return;
	}

	if (!this->preserveSplit && !this->smartSplit && !node->splitPinned) {
		node->splitTop = box.height() > box.width();
	}

	if (node->splitTop) {
		auto firstHeight = static_cast<int>(std::lround(box.height() * node->ratio));
		this->computeNode(node->first.get(), QRect(box.left(), box.top(), box.width(), firstHeight));
		this->computeNode(
		    node->second.get(),
		    QRect(box.left(), box.top() + firstHeight, box.width(), box.height() - firstHeight)
		);
	} else {
		auto firstWidth = static_cast<int>(std::lround(box.width() * node->ratio));
		this->computeNode(node->first.get(), QRect(box.left(), box.top(), firstWidth, box.height()));
		this->computeNode(
		    node->second.get(),
		    QRect(box.left() + firstWidth, box.top(), box.width() - firstWidth, box.height())
		);
	}
}

QRect DwindleLayout::box(Id id) const {
	auto* node = this->find(id);
	return node == nullptr ? QRect() : node->box;
}

QList<LayoutEngine::Id> MasterLayout::ids() const {
	QList<Id> out;
	out.reserve(this->entries.length());
	for (const auto& entry: this->entries) out.append(entry.id);
	return out;
}

qsizetype MasterLayout::indexOf(Id id) const {
	for (qsizetype i = 0; i < this->entries.length(); i++) {
		if (this->entries[i].id == id) return i;
	}

	return -1;
}

qsizetype MasterLayout::firstVisible(qsizetype from) const {
	for (auto i = from; i < this->entries.length(); i++) {
		if (!this->hidden.contains(this->entries[i].id)) return i;
	}

	return -1;
}

LayoutEngine::Id MasterLayout::master() const {
	auto index = this->firstVisible(0);
	return index == -1 ? 0 : this->entries[index].id;
}

LayoutEngine::Id MasterLayout::firstInStack() const {
	auto index = this->firstVisible(0);
	if (index == -1) return 0;
	index = this->firstVisible(index + 1);
	return index == -1 ? 0 : this->entries[index].id;
}

void MasterLayout::insert(Id id, Id target, const QPoint* /*cursor*/) {
	if (this->contains(id)) return;

	auto entry = Entry {.id = id, .weight = 1.0, .box = QRect()};

	if (this->entries.isEmpty()) {
		this->entries.append(entry);
		return;
	}

	auto masterIndex = this->firstVisible(0);
	auto targetIsMaster = target != 0 && masterIndex != -1 && this->entries[masterIndex].id == target;

	if (this->newStatus == NewStatus::Master
	    || (this->newStatus == NewStatus::Inherit && targetIsMaster))
	{
		this->entries.prepend(entry);
	} else if (this->newOnTop) {
		this->entries.insert(masterIndex == -1 ? 1 : masterIndex + 1, entry);
	} else {
		this->entries.append(entry);
	}
}

void MasterLayout::remove(Id id) {
	auto index = this->indexOf(id);
	if (index == -1) return;

	this->entries.removeAt(index);
	this->hidden.remove(id);
}

void MasterLayout::swap(Id a, Id b) {
	auto indexA = this->indexOf(a);
	auto indexB = this->indexOf(b);
	if (indexA == -1 || indexB == -1 || indexA == indexB) return;
	std::swap(this->entries[indexA].id, this->entries[indexB].id);
}

bool MasterLayout::replace(Id id, Id with) {
	auto index = this->indexOf(id);
	if (index == -1) return false;
	this->entries[index].id = with;
	this->hidden.remove(id);
	return true;
}

void MasterLayout::setHidden(Id id, bool hidden) {
	if (hidden && this->contains(id)) this->hidden.insert(id);
	else this->hidden.remove(id);
}

bool MasterLayout::splitRatio(Id id, double value, bool exact) {
	if (!this->contains(id)) return false;
	this->mfact = clampRatio(exact ? value / 2.0 : this->mfact + value / 2.0);
	return true;
}

void MasterLayout::configure(const Options& options) {
	this->newStatus = options.newStatus;
	this->newOnTop = options.newOnTop;
	this->orientation = options.orientation;

	if (options.mfact != this->configuredMfact) {
		this->configuredMfact = options.mfact;
		this->mfact = clampRatio(options.mfact);
	}
}

void MasterLayout::layoutColumn(const QList<qsizetype>& column, const QRect& rect, bool vertical) {
	auto total = 0.0;
	for (auto index: column) total += qMax(this->entries[index].weight, 0.01);

	auto length = vertical ? rect.height() : rect.width();
	auto start = vertical ? rect.top() : rect.left();
	auto previous = start;
	auto sum = 0.0;

	for (qsizetype i = 0; i < column.length(); i++) {
		auto& entry = this->entries[column[i]];
		sum += qMax(entry.weight, 0.01);

		auto end = i == column.length() - 1
		             ? start + length
		             : start + static_cast<int>(std::lround(length * sum / total));

		entry.box = vertical ? QRect(rect.left(), previous, rect.width(), end - previous)
		                     : QRect(previous, rect.top(), end - previous, rect.height());
		previous = end;
	}
}

void MasterLayout::compute(const QRect& area) {
	this->mArea = area;
	this->masterIndex = -1;
	this->columns.clear();
	this->columnSides.clear();

	for (auto& entry: this->entries) entry.box = QRect();

	QList<qsizetype> visible;
	for (qsizetype i = 0; i < this->entries.length(); i++) {
		if (!this->hidden.contains(this->entries[i].id)) visible.append(i);
	}

	if (visible.isEmpty()) return;

	this->masterIndex = visible.takeFirst();
	auto& master = this->entries[this->masterIndex];

	if (visible.isEmpty()) {
		master.box = area;
		return;
	}

	this->effective = this->orientation;
	if (this->effective == Orientation::Center && visible.length() < 2) {
		this->effective = Orientation::Left;
	}

	auto width = area.width();
	auto height = area.height();
	auto masterWidth = static_cast<int>(std::lround(width * this->mfact));
	auto masterHeight = static_cast<int>(std::lround(height * this->mfact));

	switch (this->effective) {
	case Orientation::Left:
		master.box = QRect(area.left(), area.top(), masterWidth, height);
		this->columns.append(visible);
		this->columnSides.append(Edge::Right);
		this->layoutColumn(
		    visible,
		    QRect(area.left() + masterWidth, area.top(), width - masterWidth, height),
		    true
		);
		break;
	case Orientation::Right:
		master.box = QRect(area.left() + width - masterWidth, area.top(), masterWidth, height);
		this->columns.append(visible);
		this->columnSides.append(Edge::Left);
		this->layoutColumn(visible, QRect(area.left(), area.top(), width - masterWidth, height), true);
		break;
	case Orientation::Top:
		master.box = QRect(area.left(), area.top(), width, masterHeight);
		this->columns.append(visible);
		this->columnSides.append(Edge::Bottom);
		this->layoutColumn(
		    visible,
		    QRect(area.left(), area.top() + masterHeight, width, height - masterHeight),
		    false
		);
		break;
	case Orientation::Bottom:
		master.box = QRect(area.left(), area.top() + height - masterHeight, width, masterHeight);
		this->columns.append(visible);
		this->columnSides.append(Edge::Top);
		this->layoutColumn(visible, QRect(area.left(), area.top(), width, height - masterHeight), false);
		break;
	case Orientation::Center: {
		auto sides = width - masterWidth;
		auto leftWidth = sides / 2;
		auto rightWidth = sides - leftWidth;
		master.box = QRect(area.left() + leftWidth, area.top(), masterWidth, height);

		QList<qsizetype> right;
		QList<qsizetype> left;
		for (qsizetype i = 0; i < visible.length(); i++) {
			if (i % 2 == 0) right.append(visible[i]);
			else left.append(visible[i]);
		}

		this->columns.append(right);
		this->columnSides.append(Edge::Right);
		this->columns.append(left);
		this->columnSides.append(Edge::Left);
		this->layoutColumn(
		    right,
		    QRect(area.left() + leftWidth + masterWidth, area.top(), rightWidth, height),
		    true
		);
		this->layoutColumn(left, QRect(area.left(), area.top(), leftWidth, height), true);
		break;
	}
	}
}

QRect MasterLayout::box(Id id) const {
	auto index = this->indexOf(id);
	return index == -1 ? QRect() : this->entries[index].box;
}

std::optional<MasterLayout::Divider> MasterLayout::dividerAt(Id id, Edge edge) const {
	auto index = this->indexOf(id);
	if (index == -1 || this->masterIndex == -1 || this->hidden.contains(id)) return std::nullopt;

	if (index == this->masterIndex) {
		for (qsizetype c = 0; c < this->columns.length(); c++) {
			if (this->columnSides[c] == edge && !this->columns[c].isEmpty()) {
				return Divider {.master = true, .side = edge};
			}
		}

		return std::nullopt;
	}

	for (qsizetype c = 0; c < this->columns.length(); c++) {
		const auto& column = this->columns[c];
		auto position = column.indexOf(index);
		if (position == -1) continue;

		auto side = this->columnSides[c];
		if (edge == opposite(side)) return Divider {.master = true, .side = side};

		auto vertical = isVerticalEdge(side);
		auto leading = vertical ? Edge::Top : Edge::Left;
		auto trailing = vertical ? Edge::Bottom : Edge::Right;

		if (edge == leading && position > 0) {
			return Divider {.before = column[position - 1], .after = index, .vertical = vertical};
		}

		if (edge == trailing && position < column.length() - 1) {
			return Divider {.before = index, .after = column[position + 1], .vertical = vertical};
		}

		return std::nullopt;
	}

	return std::nullopt;
}

void MasterLayout::moveDivider(const Divider& divider, int position) {
	const auto& area = this->mArea;

	if (divider.master) {
		if (area.width() <= 0 || area.height() <= 0) return;

		auto size = 0.0;
		auto whole = 1.0;

		if (this->effective == Orientation::Center) {
			auto center = area.left() + area.width() / 2.0;
			size = 2.0 * (divider.side == Edge::Right ? position - center : center - position);
			whole = area.width();
		} else {
			switch (divider.side) {
			case Edge::Right:
				size = position - area.left();
				whole = area.width();
				break;
			case Edge::Left:
				size = rightOf(area) - position;
				whole = area.width();
				break;
			case Edge::Bottom:
				size = position - area.top();
				whole = area.height();
				break;
			case Edge::Top:
				size = bottomOf(area) - position;
				whole = area.height();
				break;
			}
		}

		this->mfact = clampRatio(size / whole);
		return;
	}

	auto& before = this->entries[divider.before];
	auto& after = this->entries[divider.after];

	auto start = divider.vertical ? before.box.top() : before.box.left();
	auto combined = divider.vertical ? before.box.height() + after.box.height()
	                                 : before.box.width() + after.box.width();
	if (combined <= 0) return;

	auto share = clampRatio(static_cast<double>(position - start) / combined);
	auto total = qMax(before.weight, 0.01) + qMax(after.weight, 0.01);
	before.weight = total * share;
	after.weight = total - before.weight;
}

bool MasterLayout::moveEdge(Id id, Edge edge, int position) {
	auto divider = this->dividerAt(id, edge);
	if (!divider) return false;

	this->moveDivider(*divider, position);
	return true;
}

bool MasterLayout::hasEdge(Id id, Edge edge) const { return this->dividerAt(id, edge).has_value(); }

bool MasterLayout::resize(Id id, int dx, int dy) {
	auto index = this->indexOf(id);
	if (index == -1) return false;

	const auto box = this->entries[index].box;
	auto changed = false;

	auto grow = [&](int delta, Edge trailing, Edge leading) {
		if (delta == 0) return;

		if (auto divider = this->dividerAt(id, trailing)) {
			this->moveDivider(*divider, edgeOf(box, trailing) + delta);
			changed = true;
		} else if (auto divider = this->dividerAt(id, leading)) {
			this->moveDivider(*divider, edgeOf(box, leading) - delta);
			changed = true;
		}
	};

	grow(dx, Edge::Right, Edge::Left);
	grow(dy, Edge::Bottom, Edge::Top);
	return changed;
}

} // namespace qs::windows::tiling
