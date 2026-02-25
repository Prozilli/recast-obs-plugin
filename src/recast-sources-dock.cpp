/*
 * recast-sources-dock.cpp -- Per-output sources dock widget.
 *
 * Lists scene items for the currently active scene, with controls to
 * add existing/new sources, toggle visibility, reorder, and transform.
 */

#include "recast-sources-dock.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
}

/* ---- Helpers ---- */

struct enum_items_ctx {
	QListWidget *list;
};

static bool enum_items_callback(obs_scene_t *scene, obs_sceneitem_t *item,
				void *param)
{
	UNUSED_PARAMETER(scene);
	struct enum_items_ctx *ctx = (struct enum_items_ctx *)param;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	const char *name = obs_source_get_name(src);
	auto *list_item =
		new QListWidgetItem(QString::fromUtf8(name ? name : "(unnamed)"));

	list_item->setFlags(list_item->flags() | Qt::ItemIsUserCheckable);
	list_item->setCheckState(obs_sceneitem_visible(item)
					 ? Qt::Checked
					 : Qt::Unchecked);

	/* Store sceneitem pointer in user data */
	list_item->setData(Qt::UserRole,
			   QVariant::fromValue((void *)item));

	ctx->list->addItem(list_item);
	return true;
}

/* ---- Constructor / Destructor ---- */

RecastSourcesDock::RecastSourcesDock(recast_output_target_t *target,
				     QWidget *parent)
	: QDockWidget(parent), target_(target), current_scene_(nullptr)
{
	QString title = QString("%1: %2")
				.arg(obs_module_text("Recast.Dock.Sources"))
				.arg(QString::fromUtf8(target->name));
	setWindowTitle(title);
	setObjectName(QString("RecastSources_%1").arg(
		QString::fromUtf8(target->id)));

	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable |
		    QDockWidget::DockWidgetClosable);

	auto *container = new QWidget;
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(4, 4, 4, 4);

	/* Source list with checkboxes */
	list_ = new QListWidget;
	connect(list_, &QListWidget::itemChanged,
		this, &RecastSourcesDock::onItemChanged);
	connect(list_, &QListWidget::itemDoubleClicked,
		this, &RecastSourcesDock::onItemDoubleClicked);
	layout->addWidget(list_);

	/* Toolbar */
	auto *toolbar = new QHBoxLayout;

	add_btn_ = new QPushButton(obs_module_text("Recast.Sources.Add"));
	connect(add_btn_, &QPushButton::clicked,
		this, &RecastSourcesDock::onAddSource);
	toolbar->addWidget(add_btn_);

	remove_btn_ =
		new QPushButton(obs_module_text("Recast.Sources.Remove"));
	connect(remove_btn_, &QPushButton::clicked,
		this, &RecastSourcesDock::onRemoveSource);
	toolbar->addWidget(remove_btn_);

	up_btn_ = new QPushButton(obs_module_text("Recast.Sources.Up"));
	up_btn_->setFixedWidth(30);
	connect(up_btn_, &QPushButton::clicked,
		this, &RecastSourcesDock::onMoveUp);
	toolbar->addWidget(up_btn_);

	down_btn_ = new QPushButton(obs_module_text("Recast.Sources.Down"));
	down_btn_->setFixedWidth(30);
	connect(down_btn_, &QPushButton::clicked,
		this, &RecastSourcesDock::onMoveDown);
	toolbar->addWidget(down_btn_);

	layout->addLayout(toolbar);

	setWidget(container);

	/* Show initial scene if available */
	if (target->use_private_scenes && target->scene_model) {
		current_scene_ = recast_scene_model_get_active_scene(
			target->scene_model);
		refreshItems();
	}
}

RecastSourcesDock::~RecastSourcesDock() {}

/* ---- Public Slots ---- */

void RecastSourcesDock::setCurrentScene(obs_scene_t *scene,
					obs_source_t *source)
{
	Q_UNUSED(source);
	current_scene_ = scene;
	refreshItems();
}

/* ---- Private ---- */

void RecastSourcesDock::refreshItems()
{
	list_->blockSignals(true);
	list_->clear();

	if (current_scene_) {
		struct enum_items_ctx ctx;
		ctx.list = list_;
		obs_scene_enum_items(current_scene_, enum_items_callback,
				     &ctx);
	}

	list_->blockSignals(false);
}

/* ---- Slots ---- */

void RecastSourcesDock::onAddSource()
{
	if (!current_scene_)
		return;

	QMenu menu;

	/* Section: Add Existing Source */
	QMenu *existing_menu =
		menu.addMenu(obs_module_text("Recast.Sources.AddExisting"));

	/* Enumerate all public OBS sources */
	auto add_source_cb = [](void *param, obs_source_t *src) -> bool {
		QMenu *m = static_cast<QMenu *>(param);

		/* Only list input and scene sources */
		uint32_t flags = obs_source_get_output_flags(src);
		if (!(flags & OBS_SOURCE_VIDEO))
			return true;

		const char *name = obs_source_get_name(src);
		if (!name || !*name)
			return true;

		QAction *action = m->addAction(QString::fromUtf8(name));
		action->setData(QString::fromUtf8(name));
		return true;
	};

	obs_enum_sources(add_source_cb, existing_menu);

	connect(existing_menu, &QMenu::triggered, this,
		[this](QAction *action) {
			QString src_name = action->data().toString();
			obs_source_t *src = obs_get_source_by_name(
				src_name.toUtf8().constData());
			if (src) {
				obs_scene_add(current_scene_, src);
				obs_source_release(src);
				refreshItems();
				emit sourcesModified();
			}
		});

	/* Section: Create New Source */
	menu.addSeparator();
	QMenu *new_menu =
		menu.addMenu(obs_module_text("Recast.Sources.CreateNew"));

	struct {
		const char *type_id;
		const char *label;
	} common_types[] = {
		{"image_source", "Image"},
		{"color_source_v3", "Color Source"},
		{"text_gdiplus_v3", "Text (GDI+)"},
		{"browser_source", "Browser"},
	};

	for (auto &ct : common_types) {
		QAction *action = new_menu->addAction(ct.label);
		action->setData(QString::fromUtf8(ct.type_id));
	}

	connect(new_menu, &QMenu::triggered, this, [this](QAction *action) {
		QString type_id = action->data().toString();
		createNewSource(type_id.toUtf8().constData(),
				action->text().toUtf8().constData());
	});

	menu.exec(QCursor::pos());
}

void RecastSourcesDock::createNewSource(const char *type_id,
					const char *label)
{
	if (!current_scene_)
		return;

	bool ok;
	QString name = QInputDialog::getText(
		this, obs_module_text("Recast.Sources.CreateNew"),
		obs_module_text("Recast.Sources.EnterName"),
		QLineEdit::Normal, QString::fromUtf8(label), &ok);

	if (!ok || name.trimmed().isEmpty())
		return;

	obs_source_t *src = obs_source_create(
		type_id, name.trimmed().toUtf8().constData(), NULL, NULL);

	if (src) {
		obs_scene_add(current_scene_, src);
		obs_source_release(src);
		refreshItems();
		emit sourcesModified();
	}
}

void RecastSourcesDock::onRemoveSource()
{
	if (!current_scene_)
		return;

	int row = list_->currentRow();
	if (row < 0)
		return;

	QListWidgetItem *item = list_->item(row);
	obs_sceneitem_t *si = static_cast<obs_sceneitem_t *>(
		item->data(Qt::UserRole).value<void *>());

	if (si) {
		obs_sceneitem_remove(si);
		refreshItems();
		emit sourcesModified();
	}
}

void RecastSourcesDock::onMoveUp()
{
	if (!current_scene_)
		return;

	int row = list_->currentRow();
	if (row <= 0)
		return;

	QListWidgetItem *item = list_->item(row);
	obs_sceneitem_t *si = static_cast<obs_sceneitem_t *>(
		item->data(Qt::UserRole).value<void *>());

	if (si) {
		obs_sceneitem_set_order_position(si, row - 1);
		refreshItems();
		list_->setCurrentRow(row - 1);
		emit sourcesModified();
	}
}

void RecastSourcesDock::onMoveDown()
{
	if (!current_scene_)
		return;

	int row = list_->currentRow();
	if (row < 0 || row >= list_->count() - 1)
		return;

	QListWidgetItem *item = list_->item(row);
	obs_sceneitem_t *si = static_cast<obs_sceneitem_t *>(
		item->data(Qt::UserRole).value<void *>());

	if (si) {
		obs_sceneitem_set_order_position(si, row + 1);
		refreshItems();
		list_->setCurrentRow(row + 1);
		emit sourcesModified();
	}
}

void RecastSourcesDock::onItemChanged(QListWidgetItem *item)
{
	if (!item)
		return;

	obs_sceneitem_t *si = static_cast<obs_sceneitem_t *>(
		item->data(Qt::UserRole).value<void *>());

	if (si) {
		bool visible = (item->checkState() == Qt::Checked);
		obs_sceneitem_set_visible(si, visible);
		emit sourcesModified();
	}
}

void RecastSourcesDock::onItemDoubleClicked(QListWidgetItem *item)
{
	if (!item)
		return;

	obs_sceneitem_t *si = static_cast<obs_sceneitem_t *>(
		item->data(Qt::UserRole).value<void *>());

	if (si)
		showTransformDialog(si);
}

void RecastSourcesDock::showTransformDialog(obs_sceneitem_t *item)
{
	if (!item)
		return;

	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("Recast.Sources.Transform"));
	dlg.setMinimumWidth(300);

	auto *form = new QFormLayout;

	/* Position */
	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);

	auto *pos_x = new QDoubleSpinBox;
	pos_x->setRange(-10000, 10000);
	pos_x->setValue(pos.x);
	form->addRow(obs_module_text("Recast.Sources.PosX"), pos_x);

	auto *pos_y = new QDoubleSpinBox;
	pos_y->setRange(-10000, 10000);
	pos_y->setValue(pos.y);
	form->addRow(obs_module_text("Recast.Sources.PosY"), pos_y);

	/* Scale */
	struct vec2 scale;
	obs_sceneitem_get_scale(item, &scale);

	auto *scale_x = new QDoubleSpinBox;
	scale_x->setRange(0.01, 100.0);
	scale_x->setSingleStep(0.1);
	scale_x->setDecimals(3);
	scale_x->setValue(scale.x);
	form->addRow(obs_module_text("Recast.Sources.ScaleX"), scale_x);

	auto *scale_y = new QDoubleSpinBox;
	scale_y->setRange(0.01, 100.0);
	scale_y->setSingleStep(0.1);
	scale_y->setDecimals(3);
	scale_y->setValue(scale.y);
	form->addRow(obs_module_text("Recast.Sources.ScaleY"), scale_y);

	/* Rotation */
	auto *rotation = new QDoubleSpinBox;
	rotation->setRange(-360, 360);
	rotation->setValue(obs_sceneitem_get_rot(item));
	form->addRow(obs_module_text("Recast.Sources.Rotation"), rotation);

	/* Crop */
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	auto *crop_left = new QSpinBox;
	crop_left->setRange(0, 10000);
	crop_left->setValue(crop.left);
	form->addRow(obs_module_text("Recast.Sources.CropLeft"), crop_left);

	auto *crop_right = new QSpinBox;
	crop_right->setRange(0, 10000);
	crop_right->setValue(crop.right);
	form->addRow(obs_module_text("Recast.Sources.CropRight"), crop_right);

	auto *crop_top = new QSpinBox;
	crop_top->setRange(0, 10000);
	crop_top->setValue(crop.top);
	form->addRow(obs_module_text("Recast.Sources.CropTop"), crop_top);

	auto *crop_bottom = new QSpinBox;
	crop_bottom->setRange(0, 10000);
	crop_bottom->setValue(crop.bottom);
	form->addRow(obs_module_text("Recast.Sources.CropBottom"),
		     crop_bottom);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
					     QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto *root = new QVBoxLayout(&dlg);
	root->addLayout(form);
	root->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return;

	/* Apply transform */
	struct vec2 new_pos;
	new_pos.x = (float)pos_x->value();
	new_pos.y = (float)pos_y->value();
	obs_sceneitem_set_pos(item, &new_pos);

	struct vec2 new_scale;
	new_scale.x = (float)scale_x->value();
	new_scale.y = (float)scale_y->value();
	obs_sceneitem_set_scale(item, &new_scale);

	obs_sceneitem_set_rot(item, (float)rotation->value());

	struct obs_sceneitem_crop new_crop;
	new_crop.left = crop_left->value();
	new_crop.right = crop_right->value();
	new_crop.top = crop_top->value();
	new_crop.bottom = crop_bottom->value();
	obs_sceneitem_set_crop(item, &new_crop);

	emit sourcesModified();
}
