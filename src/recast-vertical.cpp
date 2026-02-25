/*
 * recast-vertical.cpp -- Singleton vertical canvas controller.
 *
 * Owns the 1080x1920 canvas with its own scene model, obs_view_t,
 * and video_t pipeline. Always running for preview. Provides a
 * shared encoder for multistream destinations targeting vertical.
 */

#include "recast-vertical.h"

extern "C" {
#include <obs-module.h>
#include <util/platform.h>
#include "recast-config.h"
}

RecastVertical *RecastVertical::instance_ = nullptr;

RecastVertical::RecastVertical(QObject *parent)
	: QObject(parent)
{
	scene_model_ = recast_scene_model_create();
}

RecastVertical::~RecastVertical()
{
	shutdown();
	if (scene_model_) {
		recast_scene_model_destroy(scene_model_);
		scene_model_ = nullptr;
	}
}

RecastVertical *RecastVertical::instance()
{
	if (!instance_)
		instance_ = new RecastVertical();
	return instance_;
}

void RecastVertical::destroyInstance()
{
	delete instance_;
	instance_ = nullptr;
}

/* ---- Scene model wrappers ---- */

int RecastVertical::sceneCount() const
{
	return scene_model_ ? scene_model_->scene_count : 0;
}

const char *RecastVertical::sceneName(int idx) const
{
	if (!scene_model_ || idx < 0 || idx >= scene_model_->scene_count)
		return nullptr;
	return scene_model_->scenes[idx].name;
}

int RecastVertical::activeSceneIndex() const
{
	return scene_model_ ? scene_model_->active_scene_idx : -1;
}

void RecastVertical::addScene(const char *name)
{
	if (!scene_model_)
		return;

	int idx = recast_scene_model_add_scene(scene_model_, name);
	if (idx >= 0) {
		emit scenesModified();

		/* If this is the first scene, bind it to the view */
		if (scene_model_->scene_count == 1) {
			setActiveScene(0);
		}
	}
}

void RecastVertical::removeScene(int idx)
{
	if (!scene_model_)
		return;

	recast_scene_model_remove_scene(scene_model_, idx);
	bindActiveSceneToView();
	emit scenesModified();
	emit activeSceneChanged(scene_model_->active_scene_idx);
}

void RecastVertical::renameScene(int idx, const char *new_name)
{
	if (!scene_model_)
		return;

	recast_scene_model_rename_scene(scene_model_, idx, new_name);
	emit scenesModified();
}

void RecastVertical::setActiveScene(int idx)
{
	if (!scene_model_)
		return;

	recast_scene_model_set_active(scene_model_, idx);
	bindActiveSceneToView();
	emit activeSceneChanged(idx);
}

void RecastVertical::linkScene(int idx, const char *main_scene_name)
{
	if (!scene_model_)
		return;

	recast_scene_model_link_scene(scene_model_, idx, main_scene_name);
	emit scenesModified();
}

const char *RecastVertical::sceneLinkedMain(int idx) const
{
	if (!scene_model_ || idx < 0 || idx >= scene_model_->scene_count)
		return nullptr;
	return scene_model_->scenes[idx].linked_main_scene;
}

/* ---- Video pipeline ---- */

void RecastVertical::initialize()
{
	setupView();
	obs_frontend_add_event_callback(onFrontendEvent, this);
	blog(LOG_INFO, "[Recast] Vertical canvas initialized (%dx%d)",
	     canvas_width_, canvas_height_);
}

void RecastVertical::shutdown()
{
	obs_frontend_remove_event_callback(onFrontendEvent, this);

	/* Release shared encoder */
	if (shared_video_encoder_) {
		obs_encoder_release(shared_video_encoder_);
		shared_video_encoder_ = nullptr;
		shared_encoder_refs_ = 0;
	}

	teardownView();
	blog(LOG_INFO, "[Recast] Vertical canvas shut down");
}

void RecastVertical::setupView()
{
	if (view_)
		return;

	view_ = obs_view_create();

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	struct obs_video_info custom_ovi = ovi;
	custom_ovi.base_width = (uint32_t)canvas_width_;
	custom_ovi.base_height = (uint32_t)canvas_height_;
	custom_ovi.output_width = (uint32_t)canvas_width_;
	custom_ovi.output_height = (uint32_t)canvas_height_;

	video_ = obs_view_add2(view_, &custom_ovi);
	if (!video_) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create vertical video output");
		obs_view_destroy(view_);
		view_ = nullptr;
		return;
	}

	/* Bind active scene if we have one */
	bindActiveSceneToView();
}

void RecastVertical::teardownView()
{
	if (view_ && video_) {
		obs_view_remove(view_);
		video_ = nullptr;
	}
	if (view_) {
		obs_view_set_source(view_, 0, nullptr);
		obs_view_destroy(view_);
		view_ = nullptr;
	}
}

void RecastVertical::bindActiveSceneToView()
{
	if (!view_)
		return;

	obs_source_t *src = scene_model_
		? recast_scene_model_get_active_source(scene_model_)
		: nullptr;
	obs_view_set_source(view_, 0, src);
}

/* ---- Shared encoder ---- */

obs_encoder_t *RecastVertical::acquireSharedEncoder()
{
	if (!video_) {
		blog(LOG_ERROR,
		     "[Recast] Cannot acquire encoder: no vertical video");
		return nullptr;
	}

	if (!shared_video_encoder_) {
		obs_data_t *enc_settings = obs_data_create();
		obs_data_set_int(enc_settings, "bitrate", 4000);
		obs_data_set_string(enc_settings, "rate_control", "CBR");

		shared_video_encoder_ = obs_video_encoder_create(
			"obs_x264", "recast_vertical_venc",
			enc_settings, nullptr);
		obs_data_release(enc_settings);

		if (!shared_video_encoder_) {
			blog(LOG_ERROR,
			     "[Recast] Failed to create shared vertical encoder");
			return nullptr;
		}

		obs_encoder_set_video(shared_video_encoder_, video_);
		blog(LOG_INFO,
		     "[Recast] Created shared vertical encoder");
	}

	shared_encoder_refs_++;
	return shared_video_encoder_;
}

void RecastVertical::releaseSharedEncoder()
{
	if (shared_encoder_refs_ <= 0)
		return;

	shared_encoder_refs_--;

	if (shared_encoder_refs_ == 0 && shared_video_encoder_) {
		obs_encoder_release(shared_video_encoder_);
		shared_video_encoder_ = nullptr;
		blog(LOG_INFO,
		     "[Recast] Released shared vertical encoder (no more refs)");
	}
}

/* ---- Frontend event handler ---- */

void RecastVertical::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<RecastVertical *>(data);
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
		self->handleMainSceneChanged();
}

void RecastVertical::handleMainSceneChanged()
{
	if (!scene_model_)
		return;

	obs_source_t *current = obs_frontend_get_current_scene();
	if (!current)
		return;

	const char *main_scene_name = obs_source_get_name(current);
	int linked_idx = recast_scene_model_find_linked(
		scene_model_, main_scene_name);

	if (linked_idx >= 0 &&
	    linked_idx != scene_model_->active_scene_idx) {
		recast_scene_model_set_active(scene_model_, linked_idx);
		bindActiveSceneToView();
		emit activeSceneChanged(linked_idx);
		blog(LOG_INFO,
		     "[Recast] Vertical scene auto-switched to %d "
		     "(linked to '%s')",
		     linked_idx, main_scene_name);
	}

	obs_source_release(current);
}

/* ---- Config save/load ---- */

void RecastVertical::loadFromConfig(obs_data_t *vertical_data)
{
	if (!vertical_data)
		return;

	canvas_width_ = (int)obs_data_get_int(vertical_data, "canvas_width");
	canvas_height_ = (int)obs_data_get_int(vertical_data, "canvas_height");
	if (canvas_width_ <= 0) canvas_width_ = 1080;
	if (canvas_height_ <= 0) canvas_height_ = 1920;

	/* Load scene model */
	if (scene_model_) {
		recast_scene_model_destroy(scene_model_);
		scene_model_ = nullptr;
	}

	obs_data_array_t *scenes_arr =
		obs_data_get_array(vertical_data, "scenes");
	if (scenes_arr) {
		/* Reconstruct using the config loader */
		scene_model_ = recast_config_load_scene_model(vertical_data);
		obs_data_array_release(scenes_arr);
	}

	if (!scene_model_)
		scene_model_ = recast_scene_model_create();

	/* Teardown and re-setup view with potentially new resolution */
	teardownView();
	setupView();

	emit canvasSizeChanged(canvas_width_, canvas_height_);
}

obs_data_t *RecastVertical::saveToConfig() const
{
	obs_data_t *d = obs_data_create();

	obs_data_set_int(d, "canvas_width", canvas_width_);
	obs_data_set_int(d, "canvas_height", canvas_height_);

	if (scene_model_) {
		obs_data_t *sm = recast_config_save_scene_model(scene_model_);
		if (sm) {
			/* Merge scene model fields into vertical data */
			obs_data_array_t *scenes =
				obs_data_get_array(sm, "scenes");
			if (scenes) {
				obs_data_set_array(d, "scenes", scenes);
				obs_data_array_release(scenes);
			}
			const char *active =
				obs_data_get_string(sm, "active_scene");
			if (active && *active)
				obs_data_set_string(d, "active_scene", active);
			obs_data_release(sm);
		}
	}

	return d;
}

/* ---- C accessors ---- */

extern "C" {

video_t *recast_vertical_get_video(void)
{
	RecastVertical *v = RecastVertical::instance();
	return v ? v->getOutputVideo() : nullptr;
}

obs_encoder_t *recast_vertical_acquire_encoder(void)
{
	RecastVertical *v = RecastVertical::instance();
	return v ? v->acquireSharedEncoder() : nullptr;
}

void recast_vertical_release_encoder(void)
{
	RecastVertical *v = RecastVertical::instance();
	if (v)
		v->releaseSharedEncoder();
}

} /* extern "C" */
