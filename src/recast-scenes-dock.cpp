/*
 * recast-scenes-dock.cpp -- Per-output scenes dock widget.
 *
 * Provides a list of private scenes for an output, with
 * add/remove/rename controls.
 */

#include "recast-scenes-dock.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QToolBar>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
}

RecastScenesDock::RecastScenesDock(recast_output_target_t *target,
				   QWidget *parent)
	: QDockWidget(parent), target_(target)
{
	QString title = QString("%1: %2")
				.arg(obs_module_text("Recast.Dock.Scenes"))
				.arg(QString::fromUtf8(target->name));
	setWindowTitle(title);
	setObjectName(QString("RecastScenes_%1").arg(
		QString::fromUtf8(target->id)));

	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable |
		    QDockWidget::DockWidgetClosable);

	auto *container = new QWidget;
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(4, 4, 4, 4);

	/* Scene list */
	list_ = new QListWidget;
	list_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list_, &QListWidget::currentRowChanged,
		this, &RecastScenesDock::onSceneSelected);
	connect(list_, &QListWidget::customContextMenuRequested,
		this, &RecastScenesDock::onContextMenu);
	layout->addWidget(list_);

	/* Toolbar */
	auto *toolbar = new QHBoxLayout;

	add_btn_ = new QPushButton(obs_module_text("Recast.Scenes.Add"));
	connect(add_btn_, &QPushButton::clicked,
		this, &RecastScenesDock::onAddScene);
	toolbar->addWidget(add_btn_);

	remove_btn_ = new QPushButton(obs_module_text("Recast.Scenes.Remove"));
	connect(remove_btn_, &QPushButton::clicked,
		this, &RecastScenesDock::onRemoveScene);
	toolbar->addWidget(remove_btn_);

	rename_btn_ = new QPushButton(obs_module_text("Recast.Scenes.Rename"));
	connect(rename_btn_, &QPushButton::clicked,
		this, &RecastScenesDock::onRenameScene);
	toolbar->addWidget(rename_btn_);

	layout->addLayout(toolbar);

	setWidget(container);

	refresh();
}

RecastScenesDock::~RecastScenesDock() {}

void RecastScenesDock::refresh()
{
	list_->blockSignals(true);
	list_->clear();

	if (!target_->scene_model) {
		list_->blockSignals(false);
		return;
	}

	recast_scene_model_t *model = target_->scene_model;
	for (int i = 0; i < model->scene_count; i++) {
		list_->addItem(QString::fromUtf8(model->scenes[i].name));
	}

	if (model->active_scene_idx >= 0 &&
	    model->active_scene_idx < model->scene_count) {
		list_->setCurrentRow(model->active_scene_idx);
	}

	list_->blockSignals(false);
}

void RecastScenesDock::onAddScene()
{
	if (!target_->scene_model)
		return;

	bool ok;
	QString name = QInputDialog::getText(
		this, obs_module_text("Recast.Scenes.Add"),
		obs_module_text("Recast.Scenes.EnterName"),
		QLineEdit::Normal, QString(), &ok);

	if (!ok || name.trimmed().isEmpty())
		return;

	int idx = recast_scene_model_add_scene(
		target_->scene_model,
		name.trimmed().toUtf8().constData());

	if (idx < 0) {
		QMessageBox::warning(
			this, obs_module_text("Recast.Error"),
			obs_module_text("Recast.Scenes.AddFailed"));
		return;
	}

	refresh();

	/* Select the newly added scene */
	list_->setCurrentRow(idx);

	emit scenesModified();
}

void RecastScenesDock::onRemoveScene()
{
	if (!target_->scene_model)
		return;

	int row = list_->currentRow();
	if (row < 0)
		return;

	auto answer = QMessageBox::question(
		this, obs_module_text("Recast.Confirm"),
		QString("%1 '%2'?")
			.arg(obs_module_text("Recast.Scenes.ConfirmRemove"))
			.arg(list_->item(row)->text()));

	if (answer != QMessageBox::Yes)
		return;

	recast_scene_model_remove_scene(target_->scene_model, row);
	refresh();

	/* Notify about active scene change */
	obs_scene_t *scene =
		recast_scene_model_get_active_scene(target_->scene_model);
	obs_source_t *src =
		recast_scene_model_get_active_source(target_->scene_model);
	emit activeSceneChanged(scene, src);
	emit scenesModified();
}

void RecastScenesDock::onRenameScene()
{
	if (!target_->scene_model)
		return;

	int row = list_->currentRow();
	if (row < 0)
		return;

	bool ok;
	QString new_name = QInputDialog::getText(
		this, obs_module_text("Recast.Scenes.Rename"),
		obs_module_text("Recast.Scenes.EnterName"),
		QLineEdit::Normal,
		list_->item(row)->text(), &ok);

	if (!ok || new_name.trimmed().isEmpty())
		return;

	recast_scene_model_rename_scene(
		target_->scene_model, row,
		new_name.trimmed().toUtf8().constData());

	refresh();
	emit scenesModified();
}

void RecastScenesDock::onSceneSelected(int row)
{
	if (!target_->scene_model || row < 0)
		return;

	recast_scene_model_set_active(target_->scene_model, row);

	/* Update the view if it exists */
	recast_output_target_bind_active_scene(target_);

	obs_scene_t *scene =
		recast_scene_model_get_active_scene(target_->scene_model);
	obs_source_t *src =
		recast_scene_model_get_active_source(target_->scene_model);

	emit activeSceneChanged(scene, src);
}

void RecastScenesDock::onContextMenu(const QPoint &pos)
{
	QMenu menu;

	int row = list_->currentRow();

	/* Add Scene — always available */
	QAction *add_action =
		menu.addAction(obs_module_text("Recast.Scenes.Add"));
	connect(add_action, &QAction::triggered, this,
		&RecastScenesDock::onAddScene);

	if (row >= 0) {
		menu.addSeparator();

		/* Rename */
		QAction *rename_action =
			menu.addAction(obs_module_text("Recast.Scenes.Rename"));
		connect(rename_action, &QAction::triggered, this,
			&RecastScenesDock::onRenameScene);

		/* Link to Main Scene */
		QMenu *link_menu = menu.addMenu("Link to Main Scene");

		/* Unlink option */
		recast_scene_entry_t *entry =
			&target_->scene_model->scenes[row];
		if (entry->linked_main_scene) {
			QAction *unlink = link_menu->addAction(
				QString("Unlink (currently: %1)")
					.arg(QString::fromUtf8(
						entry->linked_main_scene)));
			connect(unlink, &QAction::triggered, this,
				[this, row]() {
					recast_scene_model_link_scene(
						target_->scene_model,
						row, NULL);
					emit scenesModified();
				});
			link_menu->addSeparator();
		}

		/* List all main scenes */
		char **main_scenes = obs_frontend_get_scene_names();
		if (main_scenes) {
			for (char **s = main_scenes; *s; s++) {
				QString scene_name =
					QString::fromUtf8(*s);
				QAction *link_action =
					link_menu->addAction(scene_name);
				connect(link_action,
					&QAction::triggered, this,
					[this, row, scene_name]() {
						recast_scene_model_link_scene(
							target_->scene_model,
							row,
							scene_name.toUtf8()
								.constData());
						emit scenesModified();
					});
			}
			bfree(main_scenes);
		}

		menu.addSeparator();

		/* Remove */
		QAction *remove_action = menu.addAction(
			obs_module_text("Recast.Scenes.Remove"));
		connect(remove_action, &QAction::triggered, this,
			&RecastScenesDock::onRemoveScene);
	}

	menu.exec(list_->mapToGlobal(pos));
}
