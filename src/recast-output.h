#pragma once

#include <obs-module.h>
#include "recast-scene-model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECAST_MAX_OUTPUTS 16

/* ---- Per-target state ---- */

typedef struct recast_output_target {
	/* Identity */
	char *id; /* unique, e.g. "recast_out_1709..." */

	/* Config */
	char *name;
	char *url;
	char *key;
	char *scene_name; /* NULL = use main scene (legacy) */
	bool enabled;
	bool auto_start;
	int width;  /* 0 = use main canvas */
	int height; /* 0 = use main canvas */

	/* Independent scene model (NULL when use_private_scenes=false) */
	recast_scene_model_t *scene_model;
	bool use_private_scenes;

	/* OBS objects */
	obs_output_t *output;   /* built-in "rtmp_output" instance */
	obs_service_t *service; /* "recast_rtmp_service" instance */

	/* Per-scene rendering (Phase 3) — NULL when sharing main */
	obs_view_t *view;
	video_t *video;
	obs_encoder_t *video_encoder;
	obs_encoder_t *audio_encoder;

	/* Runtime state */
	bool active;
	uint64_t start_time_ns;
} recast_output_target_t;

/* ---- Management API ---- */

recast_output_target_t *recast_output_target_create(const char *name,
						    const char *url,
						    const char *key,
						    const char *scene_name,
						    int width, int height);

void recast_output_target_destroy(recast_output_target_t *target);

bool recast_output_target_start(recast_output_target_t *target);
void recast_output_target_stop(recast_output_target_t *target);

/* Returns elapsed streaming time in seconds, 0 if not active */
uint64_t recast_output_target_elapsed_sec(const recast_output_target_t *t);

/* Bind the active scene from scene_model to the view.
 * Updates obs_view_set_source when user switches scenes. */
void recast_output_target_bind_active_scene(recast_output_target_t *target);

/* Create the view/video pipeline for preview (without encoders).
 * Call this to enable preview before streaming starts.
 * Pass scene_src = the source to render in the view. */
bool recast_output_target_ensure_view(recast_output_target_t *target,
				      obs_source_t *scene_src);

/* ---- OBS registration (called from plugin-main.c) ---- */

void recast_output_register(void);

#ifdef __cplusplus
}
#endif
