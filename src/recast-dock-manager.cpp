/*
 * recast-dock-manager.cpp -- Coordinates dock lifecycle per output.
 *
 * Creates/destroys 4 OBS-registered docks per output and wires
 * cross-dock signals for scene changes, source modifications,
 * and settings updates.
 */

#include "recast-dock-manager.h"
#include "recast-dock.h"
#include "recast-scenes-dock.h"
#include "recast-sources-dock.h"
#include "recast-preview-dock.h"
#include "recast-settings-dock.h"

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

RecastDockManager::RecastDockManager(RecastDock *parent_dock)
	: QObject(parent_dock), parent_dock_(parent_dock)
{
}

RecastDockManager::~RecastDockManager()
{
	destroyAll();
}

void RecastDockManager::createDocksForOutput(recast_output_target_t *target)
{
	if (!target || !target->id)
		return;

	QString key = QString::fromUtf8(target->id);

	/* Don't create twice */
	if (docks_.contains(key))
		return;

	/* Ensure the target has a scene model */
	if (!target->scene_model && target->use_private_scenes) {
		target->scene_model = recast_scene_model_create();
	}

	QWidget *main_window =
		static_cast<QWidget *>(obs_frontend_get_main_window());

	auto *dock_set = new RecastDockSet;

	/* Create the 4 docks */
	dock_set->scenes = new RecastScenesDock(target, main_window);
	dock_set->sources = new RecastSourcesDock(target, main_window);
	dock_set->preview = new RecastPreviewDock(target, main_window);
	dock_set->settings = new RecastSettingsDock(target, main_window);

	/* Wire signals: ScenesDock -> SourcesDock + PreviewDock */
	connect(dock_set->scenes, &RecastScenesDock::activeSceneChanged,
		dock_set->sources, &RecastSourcesDock::setCurrentScene);

	connect(dock_set->scenes, &RecastScenesDock::activeSceneChanged,
		this, [dock_set](obs_scene_t *, obs_source_t *source) {
			dock_set->preview->updateScene(source);
		});

	/* Wire ScenesDock -> PreviewDock interactive scene */
	connect(dock_set->scenes, &RecastScenesDock::activeSceneChanged,
		dock_set->preview,
		&RecastPreviewDock::updateInteractiveScene);

	/* Wire signals: SettingsDock -> PreviewDock */
	connect(dock_set->settings, &RecastSettingsDock::settingsChanged,
		this, [dock_set](recast_output_target_t *t) {
			dock_set->preview->updateResolution(t->width,
							    t->height);
		});

	/* Bidirectional preview <-> sources item selection */
	RecastPreviewWidget *pw = dock_set->preview->previewWidget();

	connect(pw, &RecastPreviewWidget::itemSelected,
		dock_set->sources, &RecastSourcesDock::selectSceneItem);

	connect(dock_set->sources, &RecastSourcesDock::itemSelected,
		pw, &RecastPreviewWidget::SetSelectedItem);

	/* Preview transform changes -> config save */
	connect(pw, &RecastPreviewWidget::itemTransformed,
		this, &RecastDockManager::configChanged);

	/* Wire all modification signals -> config save */
	connect(dock_set->scenes, &RecastScenesDock::scenesModified,
		this, &RecastDockManager::configChanged);
	connect(dock_set->sources, &RecastSourcesDock::sourcesModified,
		this, &RecastDockManager::configChanged);
	connect(dock_set->settings, &RecastSettingsDock::settingsChanged,
		this, [this](recast_output_target_t *) {
			emit configChanged();
		});

	/* Register with OBS frontend */
	obs_frontend_add_dock_by_id(
		dock_set->scenes->objectName().toUtf8().constData(),
		dock_set->scenes->windowTitle().toUtf8().constData(),
		dock_set->scenes);

	obs_frontend_add_dock_by_id(
		dock_set->sources->objectName().toUtf8().constData(),
		dock_set->sources->windowTitle().toUtf8().constData(),
		dock_set->sources);

	obs_frontend_add_dock_by_id(
		dock_set->preview->objectName().toUtf8().constData(),
		dock_set->preview->windowTitle().toUtf8().constData(),
		dock_set->preview);

	obs_frontend_add_dock_by_id(
		dock_set->settings->objectName().toUtf8().constData(),
		dock_set->settings->windowTitle().toUtf8().constData(),
		dock_set->settings);

	docks_.insert(key, dock_set);

	blog(LOG_INFO, "[Recast] Created 4 docks for output '%s'",
	     target->name);
}

void RecastDockManager::destroyDocksForOutput(recast_output_target_t *target)
{
	if (!target || !target->id)
		return;

	QString key = QString::fromUtf8(target->id);

	auto it = docks_.find(key);
	if (it == docks_.end())
		return;

	RecastDockSet *dock_set = it.value();

	/* Remove from OBS frontend */
	obs_frontend_remove_dock(
		dock_set->scenes->objectName().toUtf8().constData());
	obs_frontend_remove_dock(
		dock_set->sources->objectName().toUtf8().constData());
	obs_frontend_remove_dock(
		dock_set->preview->objectName().toUtf8().constData());
	obs_frontend_remove_dock(
		dock_set->settings->objectName().toUtf8().constData());

	delete dock_set;
	docks_.erase(it);

	blog(LOG_INFO, "[Recast] Destroyed 4 docks for output '%s'",
	     target->name);
}

void RecastDockManager::destroyAll()
{
	for (auto it = docks_.begin(); it != docks_.end(); ++it) {
		RecastDockSet *dock_set = it.value();

		obs_frontend_remove_dock(
			dock_set->scenes->objectName().toUtf8().constData());
		obs_frontend_remove_dock(
			dock_set->sources->objectName().toUtf8().constData());
		obs_frontend_remove_dock(
			dock_set->preview->objectName().toUtf8().constData());
		obs_frontend_remove_dock(
			dock_set->settings->objectName().toUtf8().constData());

		delete dock_set;
	}

	docks_.clear();
}
