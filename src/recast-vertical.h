#pragma once

#include <QObject>

extern "C" {
#include <obs.h>
#include <obs-frontend-api.h>
#include "recast-scene-model.h"
}

/*
 * RecastVertical -- Singleton controller for the 9:16 vertical canvas.
 *
 * Owns the single recast_scene_model_t, obs_view_t, and video_t
 * for the vertical canvas (always running for preview).
 * Provides getOutputVideo() for multistream destinations targeting
 * the vertical canvas, and manages a shared vertical encoder
 * (ref-counted: created on first start, destroyed on last stop).
 */

class RecastVertical : public QObject {
	Q_OBJECT

public:
	static RecastVertical *instance();
	static void destroyInstance();

	/* Scene model access */
	recast_scene_model_t *sceneModel() const { return scene_model_; }

	int sceneCount() const;
	const char *sceneName(int idx) const;
	int activeSceneIndex() const;

	void addScene(const char *name);
	void removeScene(int idx);
	void renameScene(int idx, const char *new_name);
	void setActiveScene(int idx);
	void linkScene(int idx, const char *main_scene_name);
	const char *sceneLinkedMain(int idx) const;

	/* Video pipeline */
	video_t *getOutputVideo() const { return video_; }
	int canvasWidth() const { return canvas_width_; }
	int canvasHeight() const { return canvas_height_; }

	/* Shared vertical encoder (ref-counted) */
	obs_encoder_t *acquireSharedEncoder();
	void releaseSharedEncoder();

	/* Initialize/teardown (called from plugin-main) */
	void initialize();
	void shutdown();

	/* Save/load scene model (called from config) */
	void loadFromConfig(obs_data_t *vertical_data);
	obs_data_t *saveToConfig() const;

signals:
	void activeSceneChanged(int idx);
	void scenesModified();
	void canvasSizeChanged(int w, int h);

private:
	explicit RecastVertical(QObject *parent = nullptr);
	~RecastVertical();

	static RecastVertical *instance_;

	recast_scene_model_t *scene_model_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;

	int canvas_width_ = 1080;
	int canvas_height_ = 1920;

	/* Shared vertical encoder */
	obs_encoder_t *shared_video_encoder_ = nullptr;
	int shared_encoder_refs_ = 0;

	void setupView();
	void teardownView();
	void bindActiveSceneToView();

	/* Frontend event handler for scene linking */
	static void onFrontendEvent(enum obs_frontend_event event, void *data);
	void handleMainSceneChanged();
};

/* C accessors for use from recast-output.c */
#ifdef __cplusplus
extern "C" {
#endif

video_t *recast_vertical_get_video(void);
obs_encoder_t *recast_vertical_acquire_encoder(void);
void recast_vertical_release_encoder(void);

#ifdef __cplusplus
}
#endif
