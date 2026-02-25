/*
 * recast-vertical-sources.cpp -- Always-present vertical sources dock.
 *
 * Uses the custom SourceTree widget for rich source display with
 * type icons, visibility eye, lock toggle, inline rename, and
 * drag-and-drop reordering.
 */

#include "recast-vertical-sources.h"
#include "recast-preview-widget.h"
#include "recast-source-tree.h"
#include "recast-vertical.h"

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
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <vector>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
}

/* ---- Constructor / Destructor ---- */

RecastVerticalSourcesDock::RecastVerticalSourcesDock(QWidget *parent)
	: QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);

	/* Source tree */
	tree_ = new SourceTree;
	tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(tree_, &SourceTree::itemSelected,
		this, &RecastVerticalSourcesDock::itemSelected);
	connect(tree_, &SourceTree::sourcesModified,
		this, &RecastVerticalSourcesDock::sourcesModified);
	connect(tree_, &QWidget::customContextMenuRequested,
		this, &RecastVerticalSourcesDock::onContextMenu);
	layout->addWidget(tree_);

	/* Toolbar */
	auto *toolbar = new QHBoxLayout;

	add_btn_ = new QPushButton;
	add_btn_->setProperty("themeID", "addIconSmall");
	add_btn_->setIcon(add_btn_->style()->standardIcon(
		QStyle::SP_FileDialogNewFolder));
	add_btn_->setFlat(true);
	add_btn_->setFixedSize(24, 24);
	add_btn_->setToolTip(obs_module_text("Recast.Sources.Add"));
	connect(add_btn_, &QPushButton::clicked,
		this, &RecastVerticalSourcesDock::onAddSource);
	toolbar->addWidget(add_btn_);

	remove_btn_ = new QPushButton;
	remove_btn_->setProperty("themeID", "removeIconSmall");
	remove_btn_->setIcon(remove_btn_->style()->standardIcon(
		QStyle::SP_TrashIcon));
	remove_btn_->setFlat(true);
	remove_btn_->setFixedSize(24, 24);
	remove_btn_->setToolTip(obs_module_text("Recast.Sources.Remove"));
	connect(remove_btn_, &QPushButton::clicked,
		this, &RecastVerticalSourcesDock::onRemoveSource);
	toolbar->addWidget(remove_btn_);

	props_btn_ = new QPushButton;
	props_btn_->setIcon(props_btn_->style()->standardIcon(
		QStyle::SP_FileDialogDetailedView));
	props_btn_->setFlat(true);
	props_btn_->setFixedSize(24, 24);
	props_btn_->setToolTip(
		obs_module_text("Recast.Sources.Properties"));
	connect(props_btn_, &QPushButton::clicked,
		this, &RecastVerticalSourcesDock::onProperties);
	toolbar->addWidget(props_btn_);

	up_btn_ = new QPushButton;
	up_btn_->setIcon(up_btn_->style()->standardIcon(
		QStyle::SP_ArrowUp));
	up_btn_->setFlat(true);
	up_btn_->setFixedSize(24, 24);
	up_btn_->setToolTip(obs_module_text("Recast.Sources.Up"));
	connect(up_btn_, &QPushButton::clicked,
		this, &RecastVerticalSourcesDock::onMoveUp);
	toolbar->addWidget(up_btn_);

	down_btn_ = new QPushButton;
	down_btn_->setIcon(down_btn_->style()->standardIcon(
		QStyle::SP_ArrowDown));
	down_btn_->setFlat(true);
	down_btn_->setFixedSize(24, 24);
	down_btn_->setToolTip(obs_module_text("Recast.Sources.Down"));
	connect(down_btn_, &QPushButton::clicked,
		this, &RecastVerticalSourcesDock::onMoveDown);
	toolbar->addWidget(down_btn_);

	toolbar->addStretch();

	layout->addLayout(toolbar);

	/* Show initial scene if available */
	RecastVertical *v = RecastVertical::instance();
	recast_scene_model_t *model = v->sceneModel();
	if (model) {
		current_scene_ = recast_scene_model_get_active_scene(model);
		tree_->setScene(current_scene_);
	}
}

RecastVerticalSourcesDock::~RecastVerticalSourcesDock() {}

/* ---- Public Slots ---- */

void RecastVerticalSourcesDock::setCurrentScene(int idx)
{
	Q_UNUSED(idx);
	RecastVertical *v = RecastVertical::instance();
	recast_scene_model_t *model = v->sceneModel();

	current_scene_ = model
		? recast_scene_model_get_active_scene(model)
		: nullptr;
	tree_->setScene(current_scene_);
}

void RecastVerticalSourcesDock::selectSceneItem(obs_sceneitem_t *item)
{
	tree_->selectItem(item);
}

/* ---- Build full add-source menu ---- */

struct source_type_entry {
	QString id;
	QString display_name;
};

void RecastVerticalSourcesDock::buildAddSourceMenu(QMenu *menu)
{
	/* Section: Add Existing Source */
	QMenu *existing_menu =
		menu->addMenu(obs_module_text("Recast.Sources.AddExisting"));

	auto add_source_cb = [](void *param, obs_source_t *src) -> bool {
		QMenu *m = static_cast<QMenu *>(param);

		/* Only show input sources — skip filters, transitions */
		enum obs_source_type type = obs_source_get_type(src);
		if (type != OBS_SOURCE_TYPE_INPUT)
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
				tree_->refreshItems();
				emit sourcesModified();
			}
		});

	/* Separator */
	menu->addSeparator();

	/* Section: All available input source types (deduplicated) */
	std::vector<source_type_entry> types;
	std::set<QString> seen_names;

	const char *type_id;
	for (size_t i = 0; obs_enum_input_types(i, &type_id); i++) {
		if (!type_id || !*type_id)
			continue;

		/* Skip deprecated sources */
		uint32_t caps = obs_get_source_output_flags(type_id);
		if (caps & OBS_SOURCE_DEPRECATED)
			continue;

		const char *display = obs_source_get_display_name(type_id);
		if (!display || !*display)
			continue;

		/* Skip internal-use-only and non-video sources */
		QString display_str = QString::fromUtf8(display);
		if (display_str.contains("(internal",
					  Qt::CaseInsensitive))
			continue;

		/* Skip sources that don't produce video */
		if (!(caps & (OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC_VIDEO)))
			continue;
		if (seen_names.count(display_str))
			continue;
		seen_names.insert(display_str);

		source_type_entry e;
		e.id = QString::fromUtf8(type_id);
		e.display_name = display_str;
		types.push_back(e);
	}

	std::sort(types.begin(), types.end(),
		  [](const source_type_entry &a, const source_type_entry &b) {
			  return a.display_name.toLower() <
				 b.display_name.toLower();
		  });

	for (auto &entry : types) {
		QAction *action = menu->addAction(entry.display_name);
		action->setData(entry.id);
	}

	connect(menu, &QMenu::triggered, this, [this](QAction *action) {
		if (action->parent() != sender())
			return;

		QString type_id_str = action->data().toString();
		if (type_id_str.isEmpty())
			return;
		createNewSource(type_id_str.toUtf8().constData(),
				action->text().toUtf8().constData());
	});
}

/* ---- Slots ---- */

void RecastVerticalSourcesDock::onAddSource()
{
	if (!current_scene_)
		return;

	QMenu menu;
	buildAddSourceMenu(&menu);
	menu.exec(QCursor::pos());
}

void RecastVerticalSourcesDock::onContextMenu(const QPoint &pos)
{
	QMenu menu;

	obs_sceneitem_t *si = tree_->selectedItem();

	if (si) {
		obs_source_t *src = obs_sceneitem_get_source(si);

		/* Properties */
		QAction *props_action = menu.addAction(
			obs_module_text("Recast.Sources.Properties"));
		connect(props_action, &QAction::triggered, this, [src]() {
			obs_frontend_open_source_properties(src);
		});

		/* Filters */
		QAction *filters_action = menu.addAction(
			obs_module_text("Recast.Sources.Filters"));
		connect(filters_action, &QAction::triggered, this, [src]() {
			obs_frontend_open_source_filters(src);
		});

		menu.addSeparator();

		/* Transform submenu */
		QMenu *transform_menu = menu.addMenu(
			obs_module_text("Recast.Sources.Transform"));
		buildTransformMenu(transform_menu, si);

		menu.addSeparator();

		/* Visibility toggle */
		bool visible = obs_sceneitem_visible(si);
		QAction *vis_action = menu.addAction(
			visible ? obs_module_text("Recast.Sources.Hide")
				: obs_module_text("Recast.Sources.Show"));
		connect(vis_action, &QAction::triggered, this,
			[this, si, visible]() {
				obs_sceneitem_set_visible(si, !visible);
				tree_->refreshItems();
				emit sourcesModified();
			});

		menu.addSeparator();

		/* Order */
		QMenu *order_menu = menu.addMenu(
			obs_module_text("Recast.Sources.Order"));

		QAction *move_top = order_menu->addAction(
			obs_module_text("Recast.Sources.MoveTop"));
		connect(move_top, &QAction::triggered, this, [this, si]() {
			obs_sceneitem_set_order(si, OBS_ORDER_MOVE_TOP);
			tree_->refreshItems();
			emit sourcesModified();
		});

		QAction *move_up = order_menu->addAction(
			obs_module_text("Recast.Sources.MoveUp"));
		connect(move_up, &QAction::triggered, this, [this]() {
			onMoveUp();
		});

		QAction *move_down = order_menu->addAction(
			obs_module_text("Recast.Sources.MoveDown"));
		connect(move_down, &QAction::triggered, this, [this]() {
			onMoveDown();
		});

		QAction *move_bottom = order_menu->addAction(
			obs_module_text("Recast.Sources.MoveBottom"));
		connect(move_bottom, &QAction::triggered, this,
			[this, si]() {
				obs_sceneitem_set_order(
					si, OBS_ORDER_MOVE_BOTTOM);
				tree_->refreshItems();
				emit sourcesModified();
			});

		menu.addSeparator();

		/* Rename */
		QAction *rename_action = menu.addAction(
			obs_module_text("Recast.Sources.RenameSource"));
		connect(rename_action, &QAction::triggered, this,
			[this, src]() {
				bool ok;
				QString new_name = QInputDialog::getText(
					this,
					obs_module_text(
						"Recast.Sources.RenameSource"),
					obs_module_text(
						"Recast.Sources.EnterName"),
					QLineEdit::Normal,
					QString::fromUtf8(
						obs_source_get_name(src)),
					&ok);
				if (ok && !new_name.trimmed().isEmpty()) {
					obs_source_set_name(
						src,
						new_name.trimmed()
							.toUtf8()
							.constData());
					tree_->refreshItems();
					emit sourcesModified();
				}
			});

		/* Remove */
		QAction *remove_action = menu.addAction(
			obs_module_text("Recast.Sources.Remove"));
		connect(remove_action, &QAction::triggered, this, [this]() {
			onRemoveSource();
		});
	}

	menu.addSeparator();

	/* Add Source submenu */
	QMenu *add_menu = menu.addMenu(
		obs_module_text("Recast.Sources.Add"));
	buildAddSourceMenu(add_menu);

	menu.exec(tree_->mapToGlobal(pos));
}

void RecastVerticalSourcesDock::createNewSource(const char *type_id,
						const char *display_name)
{
	if (!current_scene_)
		return;

	bool ok;
	QString name = QInputDialog::getText(
		this, obs_module_text("Recast.Sources.CreateNew"),
		obs_module_text("Recast.Sources.EnterName"),
		QLineEdit::Normal, QString::fromUtf8(display_name), &ok);

	if (!ok || name.trimmed().isEmpty())
		return;

	obs_source_t *src = obs_source_create(
		type_id, name.trimmed().toUtf8().constData(), NULL, NULL);

	if (src) {
		obs_scene_add(current_scene_, src);
		obs_source_release(src);
		tree_->refreshItems();
		emit sourcesModified();

		/* Open properties for the new source */
		obs_source_t *lookup = obs_get_source_by_name(
			name.trimmed().toUtf8().constData());
		if (lookup) {
			obs_frontend_open_source_properties(lookup);
			obs_source_release(lookup);
		}
	}
}

void RecastVerticalSourcesDock::onProperties()
{
	obs_sceneitem_t *si = tree_->selectedItem();
	if (si) {
		obs_source_t *src = obs_sceneitem_get_source(si);
		if (src)
			obs_frontend_open_source_properties(src);
	}
}

void RecastVerticalSourcesDock::onRemoveSource()
{
	if (!current_scene_)
		return;

	obs_sceneitem_t *si = tree_->selectedItem();
	if (si) {
		obs_sceneitem_remove(si);
		tree_->refreshItems();
		emit sourcesModified();
	}
}

void RecastVerticalSourcesDock::onMoveUp()
{
	if (!current_scene_)
		return;

	obs_sceneitem_t *si = tree_->selectedItem();
	if (si) {
		obs_sceneitem_set_order(si, OBS_ORDER_MOVE_UP);
		tree_->refreshItems();
		emit sourcesModified();
	}
}

void RecastVerticalSourcesDock::onMoveDown()
{
	if (!current_scene_)
		return;

	obs_sceneitem_t *si = tree_->selectedItem();
	if (si) {
		obs_sceneitem_set_order(si, OBS_ORDER_MOVE_DOWN);
		tree_->refreshItems();
		emit sourcesModified();
	}
}

void RecastVerticalSourcesDock::buildTransformMenu(QMenu *menu,
						    obs_sceneitem_t *item)
{
	RecastVertical *v = RecastVertical::instance();
	int cw = v->canvasWidth();
	int ch = v->canvasHeight();

	QAction *fit_act = menu->addAction("Fit to Canvas");
	connect(fit_act, &QAction::triggered, this, [this, item, cw, ch]() {
		RecastPreviewWidget::FitToCanvas(item, cw, ch);
		emit sourcesModified();
	});

	QAction *stretch_act = menu->addAction("Stretch to Canvas");
	connect(stretch_act, &QAction::triggered, this,
		[this, item, cw, ch]() {
			RecastPreviewWidget::StretchToCanvas(item, cw, ch);
			emit sourcesModified();
		});

	QAction *center_act = menu->addAction("Center on Canvas");
	connect(center_act, &QAction::triggered, this,
		[this, item, cw, ch]() {
			RecastPreviewWidget::CenterOnCanvas(item, cw, ch);
			emit sourcesModified();
		});

	menu->addSeparator();

	QAction *flip_h_act = menu->addAction("Flip Horizontal");
	connect(flip_h_act, &QAction::triggered, this,
		[this, item, cw]() {
			RecastPreviewWidget::FlipHorizontal(item, cw);
			emit sourcesModified();
		});

	QAction *flip_v_act = menu->addAction("Flip Vertical");
	connect(flip_v_act, &QAction::triggered, this,
		[this, item, ch]() {
			RecastPreviewWidget::FlipVertical(item, ch);
			emit sourcesModified();
		});

	menu->addSeparator();

	/* Edit Transform (dialog) */
	QAction *edit_act = menu->addAction("Edit Transform...");
	connect(edit_act, &QAction::triggered, this,
		[this, item]() { showTransformDialog(item); });
}

void RecastVerticalSourcesDock::showTransformDialog(obs_sceneitem_t *item)
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
