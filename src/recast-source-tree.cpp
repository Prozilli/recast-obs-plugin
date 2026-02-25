/*
 * recast-source-tree.cpp -- Custom source tree widget for Recast.
 *
 * Replaces basic QListWidget with a rich source tree showing type icons,
 * visibility eye, lock toggle, inline rename, and drag-and-drop reordering.
 */

#include "recast-source-tree.h"

#include <QColor>
#include <QDrag>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>

#include <algorithm>

extern "C" {
#include <obs-module.h>
}

/* ---- Source type icon generation ---- */

static QIcon generate_type_icon(QChar letter, const QColor &bg)
{
	int sz = 16;
	QPixmap pm(sz, sz);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setBrush(bg);
	p.setPen(Qt::NoPen);
	p.drawRoundedRect(0, 0, sz, sz, 3, 3);

	p.setPen(Qt::white);
	QFont f = p.font();
	f.setPixelSize(10);
	f.setBold(true);
	p.setFont(f);
	p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, QString(letter));
	p.end();

	return QIcon(pm);
}

/* ====================================================================
 * SourceTreeItem
 * ==================================================================== */

SourceTreeItem::SourceTreeItem(obs_sceneitem_t *item, QWidget *parent)
	: QFrame(parent), item_(item)
{
	setFrameShape(QFrame::NoFrame);
	setFixedHeight(28);
	setStyleSheet(
		"SourceTreeItem { background: transparent; "
		"border-bottom: 1px solid #2a2a2a; }");

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(4, 2, 4, 2);
	layout->setSpacing(6);

	/* Type icon */
	icon_label_ = new QLabel;
	icon_label_->setFixedSize(16, 16);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src) {
		QIcon icon = getSourceTypeIcon(src);
		icon_label_->setPixmap(icon.pixmap(16, 16));
	}
	layout->addWidget(icon_label_);

	/* Name label */
	name_label_ = new QLabel;
	name_label_->setSizePolicy(QSizePolicy::Expanding,
				   QSizePolicy::Preferred);
	name_label_->setStyleSheet(
		"QLabel { color: #ddd; font-size: 12px; }");
	if (src) {
		const char *name = obs_source_get_name(src);
		name_label_->setText(
			QString::fromUtf8(name ? name : "(unnamed)"));
	}
	layout->addWidget(name_label_, 1);

	/* Inline rename edit (hidden by default) */
	name_edit_ = new QLineEdit;
	name_edit_->setVisible(false);
	name_edit_->setSizePolicy(QSizePolicy::Expanding,
				  QSizePolicy::Preferred);
	connect(name_edit_, &QLineEdit::returnPressed,
		this, &SourceTreeItem::onRenameFinished);
	connect(name_edit_, &QLineEdit::editingFinished,
		this, &SourceTreeItem::onRenameFinished);
	layout->addWidget(name_edit_, 1);

	/* Visibility button (eye icon) */
	vis_btn_ = new QPushButton;
	vis_btn_->setFixedSize(22, 22);
	vis_btn_->setFlat(true);
	vis_btn_->setCheckable(true);
	vis_btn_->setChecked(obs_sceneitem_visible(item));
	vis_btn_->setText(obs_sceneitem_visible(item)
		? QString::fromUtf8("\xf0\x9f\x91\x81")   /* eye */
		: QString::fromUtf8("\xe2\x80\x94"));       /* dash */
	vis_btn_->setToolTip(
		obs_module_text("Recast.SourceTree.Visibility"));
	vis_btn_->setStyleSheet(
		"QPushButton { font-size: 12px; padding: 0; border: none; }"
		"QPushButton:checked { color: #ccc; }"
		"QPushButton:!checked { color: #555; }");
	connect(vis_btn_, &QPushButton::clicked,
		this, &SourceTreeItem::onVisibilityClicked);
	layout->addWidget(vis_btn_);

	/* Lock button (padlock icon) */
	lock_btn_ = new QPushButton;
	lock_btn_->setFixedSize(22, 22);
	lock_btn_->setFlat(true);
	lock_btn_->setCheckable(true);
	lock_btn_->setChecked(obs_sceneitem_locked(item));
	lock_btn_->setText(obs_sceneitem_locked(item)
		? QString::fromUtf8("\xf0\x9f\x94\x92")   /* locked */
		: QString::fromUtf8("\xf0\x9f\x94\x93"));  /* unlocked */
	lock_btn_->setToolTip(
		obs_module_text("Recast.SourceTree.Lock"));
	lock_btn_->setStyleSheet(
		"QPushButton { font-size: 12px; padding: 0; border: none; }"
		"QPushButton:checked { color: #f44; }"
		"QPushButton:!checked { color: #555; }");
	connect(lock_btn_, &QPushButton::clicked,
		this, &SourceTreeItem::onLockClicked);
	layout->addWidget(lock_btn_);
}

SourceTreeItem::~SourceTreeItem() {}

void SourceTreeItem::setSelected(bool selected)
{
	if (selected) {
		setStyleSheet(
			"SourceTreeItem { background: #001c3f; "
			"border-radius: 3px; "
			"border-bottom: 1px solid #002a5f; }");
	} else {
		setStyleSheet(
			"SourceTreeItem { background: transparent; "
			"border-bottom: 1px solid #2a2a2a; }");
	}
}

void SourceTreeItem::update()
{
	obs_source_t *src = obs_sceneitem_get_source(item_);
	if (src && !renaming_) {
		const char *name = obs_source_get_name(src);
		name_label_->setText(
			QString::fromUtf8(name ? name : "(unnamed)"));
	}

	bool vis = obs_sceneitem_visible(item_);
	vis_btn_->setChecked(vis);
	vis_btn_->setText(vis
		? QString::fromUtf8("\xf0\x9f\x91\x81")
		: QString::fromUtf8("\xe2\x80\x94"));

	bool locked = obs_sceneitem_locked(item_);
	lock_btn_->setChecked(locked);
	lock_btn_->setText(locked
		? QString::fromUtf8("\xf0\x9f\x94\x92")
		: QString::fromUtf8("\xf0\x9f\x94\x93"));
}

QIcon SourceTreeItem::getSourceTypeIcon(obs_source_t *source)
{
	if (!source)
		return generate_type_icon('?', QColor(128, 128, 128));

	enum obs_icon_type icon_type = obs_source_get_icon_type(
		obs_source_get_id(source));

	switch (icon_type) {
	case OBS_ICON_TYPE_IMAGE:
		return generate_type_icon('I', QColor(52, 120, 246));
	case OBS_ICON_TYPE_CAMERA:
		return generate_type_icon('C', QColor(255, 149, 0));
	case OBS_ICON_TYPE_GAME_CAPTURE:
		return generate_type_icon('G', QColor(255, 59, 48));
	case OBS_ICON_TYPE_DESKTOP_CAPTURE:
		return generate_type_icon('D', QColor(88, 86, 214));
	case OBS_ICON_TYPE_WINDOW_CAPTURE:
		return generate_type_icon('W', QColor(52, 199, 89));
	case OBS_ICON_TYPE_BROWSER:
		return generate_type_icon('B', QColor(0, 122, 255));
	case OBS_ICON_TYPE_MEDIA:
		return generate_type_icon('M', QColor(175, 82, 222));
	case OBS_ICON_TYPE_TEXT:
		return generate_type_icon('T', QColor(255, 204, 0));
	case OBS_ICON_TYPE_COLOR:
		return generate_type_icon('C', QColor(90, 200, 250));
	case OBS_ICON_TYPE_SLIDESHOW:
		return generate_type_icon('S', QColor(255, 149, 0));
	case OBS_ICON_TYPE_AUDIO_INPUT:
	case OBS_ICON_TYPE_AUDIO_OUTPUT:
		return generate_type_icon('A', QColor(48, 209, 88));
	default:
		return generate_type_icon('S', QColor(142, 142, 147));
	}
}

void SourceTreeItem::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		startRename();
	else
		QFrame::mouseDoubleClickEvent(event);
}

void SourceTreeItem::startRename()
{
	renaming_ = true;
	name_edit_->setText(name_label_->text());
	name_label_->setVisible(false);
	name_edit_->setVisible(true);
	name_edit_->setFocus();
	name_edit_->selectAll();
}

void SourceTreeItem::cancelRename()
{
	renaming_ = false;
	name_edit_->setVisible(false);
	name_label_->setVisible(true);
}

void SourceTreeItem::onVisibilityClicked()
{
	bool visible = vis_btn_->isChecked();
	obs_sceneitem_set_visible(item_, visible);
	vis_btn_->setText(visible
		? QString::fromUtf8("\xf0\x9f\x91\x81")
		: QString::fromUtf8("\xe2\x80\x94"));
	emit visibilityToggled(item_, visible);
}

void SourceTreeItem::onLockClicked()
{
	bool locked = lock_btn_->isChecked();
	obs_sceneitem_set_locked(item_, locked);
	lock_btn_->setText(locked
		? QString::fromUtf8("\xf0\x9f\x94\x92")
		: QString::fromUtf8("\xf0\x9f\x94\x93"));
	emit lockToggled(item_, locked);
}

void SourceTreeItem::onRenameFinished()
{
	if (!renaming_)
		return;

	QString new_name = name_edit_->text().trimmed();
	renaming_ = false;
	name_edit_->setVisible(false);
	name_label_->setVisible(true);

	if (!new_name.isEmpty() && new_name != name_label_->text()) {
		name_label_->setText(new_name);
		emit renamed(item_, new_name);
	}
}

void SourceTreeItem::onRenameEscaped()
{
	cancelRename();
}

/* ====================================================================
 * SourceTreeModel
 * ==================================================================== */

SourceTreeModel::SourceTreeModel(QObject *parent)
	: QAbstractListModel(parent)
{
}

struct enum_reverse_ctx {
	std::vector<obs_sceneitem_t *> *items;
};

static bool enum_reverse_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<struct enum_reverse_ctx *>(param);
	ctx->items->push_back(item);
	return true;
}

void SourceTreeModel::populate()
{
	items_.clear();
	if (!scene_)
		return;

	struct enum_reverse_ctx ctx;
	ctx.items = &items_;
	obs_scene_enum_items(scene_, enum_reverse_cb, &ctx);

	/* Reverse so topmost item is first in the list */
	std::reverse(items_.begin(), items_.end());
}

void SourceTreeModel::setScene(obs_scene_t *scene)
{
	beginResetModel();
	scene_ = scene;
	populate();
	endResetModel();
}

obs_sceneitem_t *SourceTreeModel::itemAt(int row) const
{
	if (row < 0 || row >= (int)items_.size())
		return nullptr;
	return items_[row];
}

int SourceTreeModel::findItem(obs_sceneitem_t *item) const
{
	for (int i = 0; i < (int)items_.size(); i++) {
		if (items_[i] == item)
			return i;
	}
	return -1;
}

int SourceTreeModel::rowCount(const QModelIndex &) const
{
	return (int)items_.size();
}

QVariant SourceTreeModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() >= (int)items_.size())
		return QVariant();

	if (role == Qt::DisplayRole) {
		obs_source_t *src =
			obs_sceneitem_get_source(items_[index.row()]);
		if (src) {
			const char *name = obs_source_get_name(src);
			return QString::fromUtf8(name ? name : "(unnamed)");
		}
	}

	return QVariant();
}

Qt::ItemFlags SourceTreeModel::flags(const QModelIndex &index) const
{
	Qt::ItemFlags default_flags = QAbstractListModel::flags(index);

	if (index.isValid())
		return Qt::ItemIsDragEnabled | default_flags;
	else
		return Qt::ItemIsDropEnabled | default_flags;
}

Qt::DropActions SourceTreeModel::supportedDropActions() const
{
	return Qt::MoveAction;
}

QStringList SourceTreeModel::mimeTypes() const
{
	return QStringList() << "application/x-recast-source-tree-row";
}

QMimeData *SourceTreeModel::mimeData(const QModelIndexList &indexes) const
{
	QMimeData *mime = new QMimeData;
	QByteArray encoded;
	QDataStream stream(&encoded, QIODevice::WriteOnly);

	for (const QModelIndex &index : indexes) {
		if (index.isValid())
			stream << index.row();
	}

	mime->setData("application/x-recast-source-tree-row", encoded);
	return mime;
}

bool SourceTreeModel::dropMimeData(const QMimeData *data,
				   Qt::DropAction action, int row,
				   int column, const QModelIndex &parent)
{
	Q_UNUSED(column);
	Q_UNUSED(parent);

	if (action == Qt::IgnoreAction)
		return true;

	if (!data->hasFormat("application/x-recast-source-tree-row"))
		return false;

	QByteArray encoded =
		data->data("application/x-recast-source-tree-row");
	QDataStream stream(&encoded, QIODevice::ReadOnly);

	int from_row;
	if (stream.atEnd())
		return false;
	stream >> from_row;

	if (from_row < 0 || from_row >= (int)items_.size())
		return false;

	int to_row = row;
	if (to_row < 0)
		to_row = (int)items_.size();
	if (from_row == to_row || from_row == to_row - 1)
		return false;

	/* Convert visual row back to scene order position.
	 * The visual list is reversed (topmost first), so:
	 * visual row 0 = scene order (count - 1)
	 * visual row N = scene order (count - 1 - N)
	 */
	int count = (int)items_.size();
	int scene_pos = count - 1 - (to_row < from_row ? to_row : to_row - 1);

	obs_sceneitem_t *item = items_[from_row];
	obs_sceneitem_set_order_position(item, scene_pos);

	beginResetModel();
	populate();
	endResetModel();

	emit orderChanged();
	return true;
}

/* ====================================================================
 * SourceTree
 * ==================================================================== */

SourceTree::SourceTree(QWidget *parent) : QListView(parent)
{
	model_ = new SourceTreeModel(this);
	setModel(model_);

	setDragEnabled(true);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setSelectionMode(QAbstractItemView::SingleSelection);

	connect(selectionModel(), &QItemSelectionModel::currentChanged,
		this, &SourceTree::onSelectionChanged);
	connect(model_, &SourceTreeModel::orderChanged,
		this, &SourceTree::onOrderChanged);
}

SourceTree::~SourceTree() {}

void SourceTree::setScene(obs_scene_t *scene)
{
	model_->setScene(scene);
	setupItemWidgets();
}

void SourceTree::refreshItems()
{
	obs_scene_t *scene = model_->scene();
	model_->setScene(scene);
	setupItemWidgets();
}

void SourceTree::setupItemWidgets()
{
	for (int i = 0; i < model_->rowCount(); i++) {
		QModelIndex index = model_->index(i);
		obs_sceneitem_t *item = model_->itemAt(i);
		if (!item)
			continue;

		auto *widget = new SourceTreeItem(item, this);
		setIndexWidget(index, widget);

		connect(widget, &SourceTreeItem::visibilityToggled,
			this, &SourceTree::onItemVisibilityToggled);
		connect(widget, &SourceTreeItem::lockToggled,
			this, &SourceTree::onItemLockToggled);
		connect(widget, &SourceTreeItem::renamed,
			this, &SourceTree::onItemRenamed);
	}
}

obs_sceneitem_t *SourceTree::selectedItem() const
{
	QModelIndex index = currentIndex();
	if (!index.isValid())
		return nullptr;
	return model_->itemAt(index.row());
}

void SourceTree::selectItem(obs_sceneitem_t *item)
{
	if (!item) {
		clearSelection();
		return;
	}

	int row = model_->findItem(item);
	if (row >= 0) {
		setCurrentIndex(model_->index(row));
	}
}

void SourceTree::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_F2) {
		QModelIndex index = currentIndex();
		if (index.isValid()) {
			auto *widget = static_cast<SourceTreeItem *>(
				indexWidget(index));
			if (widget) {
				widget->startRename();
			}
		}
		return;
	}

	if (event->key() == Qt::Key_Delete) {
		obs_sceneitem_t *item = selectedItem();
		if (item) {
			obs_sceneitem_remove(item);
			refreshItems();
			emit sourcesModified();
		}
		return;
	}

	QListView::keyPressEvent(event);
}

void SourceTree::onSelectionChanged()
{
	obs_sceneitem_t *item = selectedItem();
	emit itemSelected(item);

	/* Update visual selection on item widgets */
	for (int i = 0; i < model_->rowCount(); i++) {
		auto *widget = static_cast<SourceTreeItem *>(
			indexWidget(model_->index(i)));
		if (widget)
			widget->setSelected(i == currentIndex().row());
	}
}

void SourceTree::onItemVisibilityToggled(obs_sceneitem_t *, bool)
{
	emit sourcesModified();
}

void SourceTree::onItemLockToggled(obs_sceneitem_t *, bool)
{
	emit sourcesModified();
}

void SourceTree::onItemRenamed(obs_sceneitem_t *item, const QString &name)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src)
		obs_source_set_name(src, name.toUtf8().constData());
	emit sourcesModified();
}

void SourceTree::onOrderChanged()
{
	setupItemWidgets();
	emit sourcesModified();
}
