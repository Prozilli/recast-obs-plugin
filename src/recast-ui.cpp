/*
 * recast-ui.cpp -- Top-level UI setup for Recast plugin.
 *
 * Creates all 4 docks on OBS_FRONTEND_EVENT_FINISHED_LOADING,
 * wires signals between them, and handles config load/save.
 */

#include "recast-ui.h"
#include "recast-vertical.h"
#include "recast-vertical-preview.h"
#include "recast-vertical-scenes.h"
#include "recast-vertical-sources.h"
#include "recast-multistream.h"
#include "recast-preview-widget.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QObject>
#include <QTimer>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include "recast-config.h"
}

/* ---- Static instances ---- */

static RecastVerticalPreviewDock *preview_dock = nullptr;
static RecastVerticalScenesDock *scenes_dock = nullptr;
static RecastVerticalSourcesDock *sources_dock = nullptr;
static RecastMultistreamDock *multistream_dock = nullptr;

/* ---- Dock helpers ---- */

static QDockWidget *findParentDock(QWidget *w)
{
	QWidget *p = w ? w->parentWidget() : nullptr;
	while (p) {
		QDockWidget *dw = qobject_cast<QDockWidget *>(p);
		if (dw)
			return dw;
		p = p->parentWidget();
	}
	return nullptr;
}

struct dock_entry {
	const char *key;
	QWidget **widget;
};

static dock_entry dock_map[] = {
	{"dock_preview", (QWidget **)&preview_dock},
	{"dock_scenes", (QWidget **)&scenes_dock},
	{"dock_sources", (QWidget **)&sources_dock},
	{"dock_multistream", (QWidget **)&multistream_dock},
};

/* ---- Config persistence ---- */

static void save_all_config()
{
	if (!multistream_dock)
		return;

	/* Build root JSON */
	obs_data_t *root = obs_data_create();

	/* Save vertical canvas config */
	RecastVertical *v = RecastVertical::instance();
	obs_data_t *vertical_data = v->saveToConfig();
	if (vertical_data) {
		obs_data_set_obj(root, "vertical", vertical_data);
		obs_data_release(vertical_data);
	}

	/* Save destinations */
	obs_data_array_t *dests = multistream_dock->saveDestinations();
	if (dests) {
		obs_data_set_array(root, "destinations", dests);
		obs_data_array_release(dests);
	}

	/* Preserve server token */
	char *token = recast_config_get_server_token();
	obs_data_set_string(root, "server_token", token ? token : "");
	bfree(token);

	/* Save main window state (captures all dock positions/sizes).
	 * Guard against accessing already-destroyed widgets on shutdown. */
	try {
		QMainWindow *mw = qobject_cast<QMainWindow *>(
			static_cast<QWidget *>(
				obs_frontend_get_main_window()));
		if (mw) {
			QByteArray state = mw->saveState();
			obs_data_set_string(root, "mainwindow_state",
					    state.toBase64().constData());
		}
	} catch (...) {
		/* Widgets may be partially destroyed during shutdown */
	}

	/* Write to disk */
	char *path = recast_config_get_path();
	if (path) {
		obs_data_save_json(root, path);
		blog(LOG_INFO, "[Recast] Config saved to %s", path);
		bfree(path);
	}

	obs_data_release(root);
}

static void load_all_config()
{
	char *path = recast_config_get_path();
	if (!path)
		return;

	obs_data_t *root = obs_data_create_from_json_file(path);
	bfree(path);
	if (!root)
		return;

	RecastVertical *v = RecastVertical::instance();

	/* Check for old format and migrate */
	obs_data_array_t *old_outputs = obs_data_get_array(root, "outputs");
	if (old_outputs) {
		/* Migration: old format detected */
		blog(LOG_INFO, "[Recast] Migrating old config format...");

		obs_data_t *vertical_data = obs_data_create();
		obs_data_set_int(vertical_data, "canvas_width", 1080);
		obs_data_set_int(vertical_data, "canvas_height", 1920);

		obs_data_array_t *new_dests = obs_data_array_create();
		bool vertical_migrated = false;

		size_t count = obs_data_array_count(old_outputs);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *old = obs_data_array_item(old_outputs, i);

			bool use_private =
				obs_data_get_bool(old, "usePrivateScenes");

			if (use_private && !vertical_migrated) {
				/* First private-scene output becomes the
				 * vertical canvas scene model */
				obs_data_t *sm =
					obs_data_get_obj(old, "scene_model");
				if (sm) {
					obs_data_array_t *scenes =
						obs_data_get_array(sm,
								   "scenes");
					if (scenes) {
						obs_data_set_array(
							vertical_data,
							"scenes", scenes);
						obs_data_array_release(scenes);
					}
					const char *active =
						obs_data_get_string(
							sm, "active_scene");
					if (active && *active)
						obs_data_set_string(
							vertical_data,
							"active_scene",
							active);
					obs_data_release(sm);
				}

				/* Also convert to a destination */
				obs_data_t *dest = obs_data_create();
				obs_data_set_string(
					dest, "name",
					obs_data_get_string(old, "name"));
				obs_data_set_string(
					dest, "url",
					obs_data_get_string(old, "url"));
				obs_data_set_string(
					dest, "key",
					obs_data_get_string(old, "key"));
				obs_data_set_string(dest, "canvas",
						    "vertical");
				obs_data_set_bool(
					dest, "autoStart",
					obs_data_get_bool(old, "autoStart"));
				obs_data_set_bool(
					dest, "autoStop",
					obs_data_get_bool(old, "autoStop"));
				obs_data_array_push_back(new_dests, dest);
				obs_data_release(dest);

				vertical_migrated = true;
			} else {
				/* Regular output becomes a destination */
				obs_data_t *dest = obs_data_create();
				obs_data_set_string(
					dest, "name",
					obs_data_get_string(old, "name"));
				obs_data_set_string(
					dest, "url",
					obs_data_get_string(old, "url"));
				obs_data_set_string(
					dest, "key",
					obs_data_get_string(old, "key"));
				obs_data_set_string(dest, "canvas", "main");
				obs_data_set_bool(
					dest, "autoStart",
					obs_data_get_bool(old, "autoStart"));
				obs_data_set_bool(
					dest, "autoStop",
					obs_data_get_bool(old, "autoStop"));
				obs_data_array_push_back(new_dests, dest);
				obs_data_release(dest);
			}

			obs_data_release(old);
		}

		/* Load migrated data */
		v->loadFromConfig(vertical_data);
		if (multistream_dock)
			multistream_dock->loadDestinations(new_dests);

		obs_data_release(vertical_data);
		obs_data_array_release(new_dests);
		obs_data_array_release(old_outputs);

		blog(LOG_INFO, "[Recast] Config migration complete");

		/* Save in new format */
		obs_data_release(root);
		save_all_config();
		return;
	}

	/* New format */
	obs_data_t *vertical_data = obs_data_get_obj(root, "vertical");
	if (vertical_data) {
		v->loadFromConfig(vertical_data);
		obs_data_release(vertical_data);
	}

	obs_data_array_t *dests = obs_data_get_array(root, "destinations");
	if (dests && multistream_dock) {
		multistream_dock->loadDestinations(dests);
		obs_data_array_release(dests);
	}

	obs_data_release(root);
}

/* ---- Dock-geometry-only save (safe during shutdown) ---- */

static void save_dock_geometry_only()
{
	/* Read existing config so we preserve scene/destination data */
	char *path = recast_config_get_path();
	if (!path)
		return;

	obs_data_t *root = obs_data_create_from_json_file(path);
	if (!root) {
		/* No existing config — nothing to preserve */
		bfree(path);
		return;
	}

	/* Update main window state (captures all dock positions/sizes) */
	try {
		QMainWindow *mw = qobject_cast<QMainWindow *>(
			static_cast<QWidget *>(
				obs_frontend_get_main_window()));
		if (mw) {
			QByteArray state = mw->saveState();
			obs_data_set_string(root, "mainwindow_state",
					    state.toBase64().constData());
		}
	} catch (...) {
		/* Widgets may be partially destroyed during shutdown */
	}

	obs_data_save_json(root, path);
	blog(LOG_INFO, "[Recast] Dock geometry saved to %s", path);
	bfree(path);
	obs_data_release(root);
}

/* ---- Frontend event: save before exit ---- */

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* Only save dock geometry on exit.
		 * Scene data is saved during normal operation via signals.
		 * By EXIT time OBS has already torn down private scene
		 * contents, so a full save would overwrite items with []. */
		save_dock_geometry_only();
	}
}

/* ---- Public API ---- */

void recast_ui_create(void)
{
	if (preview_dock)
		return; /* already created */

	QWidget *main_window =
		static_cast<QWidget *>(obs_frontend_get_main_window());

	/* Initialize the vertical canvas singleton */
	RecastVertical *v = RecastVertical::instance();

	/* Create all 4 docks */
	preview_dock = new RecastVerticalPreviewDock(main_window);
	scenes_dock = new RecastVerticalScenesDock(main_window);
	sources_dock = new RecastVerticalSourcesDock(main_window);
	multistream_dock = new RecastMultistreamDock(main_window);

	/* Load config (populates scenes + destinations) */
	load_all_config();

	/* Initialize the vertical video pipeline (after config load) */
	v->initialize();

	/* Refresh docks with loaded data */
	scenes_dock->refresh();
	sources_dock->setCurrentScene(v->activeSceneIndex());
	preview_dock->onActiveSceneChanged(v->activeSceneIndex());

	/* ---- Wire signals ---- */

	/* ScenesDock::sceneActivated -> RecastVertical::setActiveScene
	 * (already handled inside onSceneSelected) */

	/* RecastVertical::activeSceneChanged -> PreviewDock + SourcesDock */
	QObject::connect(v, &RecastVertical::activeSceneChanged,
			 preview_dock,
			 &RecastVerticalPreviewDock::onActiveSceneChanged);

	QObject::connect(v, &RecastVertical::activeSceneChanged,
			 sources_dock,
			 &RecastVerticalSourcesDock::setCurrentScene);

	/* RecastVertical::scenesModified -> ScenesDock::refresh */
	QObject::connect(v, &RecastVertical::scenesModified,
			 scenes_dock,
			 &RecastVerticalScenesDock::refresh);

	/* RecastVertical::canvasSizeChanged -> PreviewDock */
	QObject::connect(v, &RecastVertical::canvasSizeChanged,
			 preview_dock,
			 &RecastVerticalPreviewDock::onCanvasSizeChanged);

	/* Bidirectional PreviewWidget <-> SourcesDock item selection */
	RecastPreviewWidget *pw = preview_dock->previewWidget();

	QObject::connect(pw, &RecastPreviewWidget::itemSelected,
			 sources_dock,
			 &RecastVerticalSourcesDock::selectSceneItem);

	QObject::connect(sources_dock,
			 &RecastVerticalSourcesDock::itemSelected,
			 pw,
			 &RecastPreviewWidget::SetSelectedItem);

	/* Preview transform -> refresh source tree (lock/vis icons) */
	QObject::connect(pw, &RecastPreviewWidget::itemTransformed,
			 sources_dock,
			 &RecastVerticalSourcesDock::refreshTree);

	/* Config save triggers */
	QObject::connect(pw, &RecastPreviewWidget::itemTransformed,
			 [](){ save_all_config(); });

	QObject::connect(scenes_dock,
			 &RecastVerticalScenesDock::scenesModified,
			 [](){ save_all_config(); });

	QObject::connect(sources_dock,
			 &RecastVerticalSourcesDock::sourcesModified,
			 [](){ save_all_config(); });

	QObject::connect(multistream_dock,
			 &RecastMultistreamDock::configChanged,
			 [](){ save_all_config(); });

	/* Save dock state when OBS exits (while widgets are still alive) */
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	/* Register docks with OBS frontend */
	obs_frontend_add_dock_by_id(
		"RecastVerticalPreviewDock",
		obs_module_text("Recast.Vertical.Preview"),
		preview_dock);

	obs_frontend_add_dock_by_id(
		"RecastVerticalScenesDock",
		obs_module_text("Recast.Vertical.Scenes"),
		scenes_dock);

	obs_frontend_add_dock_by_id(
		"RecastVerticalSourcesDock",
		obs_module_text("Recast.Vertical.Sources"),
		sources_dock);

	obs_frontend_add_dock_by_id(
		"RecastMultistreamDock",
		obs_module_text("Recast.Multistream.DockTitle"),
		multistream_dock);

	/* Restore dock positions and force visible.
	 * OBS does not persist plugin dock state, so we manage it ourselves.
	 * Deferred so OBS has finished parenting the dock widgets. */
	QTimer::singleShot(0, [=]() {
		/* Read saved config */
		char *cfg_path = recast_config_get_path();
		obs_data_t *cfg = nullptr;
		if (cfg_path) {
			cfg = obs_data_create_from_json_file(cfg_path);
			bfree(cfg_path);
		}

		QMainWindow *mw = qobject_cast<QMainWindow *>(
			static_cast<QWidget *>(
				obs_frontend_get_main_window()));

		/* Restore main window state (dock positions/sizes) */
		if (cfg && mw) {
			const char *state_str = obs_data_get_string(
				cfg, "mainwindow_state");
			if (state_str && *state_str) {
				QByteArray state =
					QByteArray::fromBase64(
						QByteArray(state_str));
				mw->restoreState(state);
			}
		}

		/* Ensure all Recast docks are visible */
		for (auto &d : dock_map) {
			if (!*d.widget)
				continue;
			QDockWidget *dw = findParentDock(*d.widget);
			if (dw)
				dw->setVisible(true);
		}

		if (cfg)
			obs_data_release(cfg);
	});

	blog(LOG_INFO, "[Recast] All 4 docks created and wired");
}

void recast_ui_destroy(void)
{
	/* Dock state was already saved by the EXIT event callback.
	 * Do NOT call save_all_config() here — dock widgets may
	 * already be destroyed by OBS at this point. */

	/* OBS manages dock widget lifecycle, clear our references */
	preview_dock = nullptr;
	scenes_dock = nullptr;
	sources_dock = nullptr;
	multistream_dock = nullptr;

	/* Destroy the vertical canvas singleton */
	RecastVertical::destroyInstance();

	blog(LOG_INFO, "[Recast] UI destroyed");
}
