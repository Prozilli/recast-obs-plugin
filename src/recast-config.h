#pragma once

#include <obs-module.h>
#include "recast-scene-model.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Config file: <OBS profile dir>/recast-outputs.json
 *
 * New format (v3):
 * {
 *   "vertical": {
 *     "canvas_width": 1080,
 *     "canvas_height": 1920,
 *     "scenes": [...],
 *     "active_scene": "Scene 1"
 *   },
 *   "destinations": [
 *     { "id": "...", "name": "Twitch", "url": "rtmp://...", "key": "...",
 *       "canvas": "main", "autoStart": true, "autoStop": true }
 *   ],
 *   "server_token": ""
 * }
 *
 * Old format (v2) is detected by presence of "outputs" key with
 * "usePrivateScenes" and automatically migrated.
 */

/* Load the stored Recast server auth token (empty string if unset). Caller
 * must bfree() the returned string. */
char *recast_config_get_server_token(void);

/* Store the Recast server auth token. */
bool recast_config_set_server_token(const char *token);

/* Convenience: get full path to the config file. Caller must bfree(). */
char *recast_config_get_path(void);

/* Save a scene model's scenes + items into an obs_data_t object.
 * The returned obs_data_t must be released by the caller. */
obs_data_t *recast_config_save_scene_model(
	const struct recast_scene_model *model);

/* Load a scene model from an obs_data_t object. Returns a newly created
 * model, or NULL on failure. Caller must destroy with
 * recast_scene_model_destroy(). */
struct recast_scene_model *recast_config_load_scene_model(obs_data_t *data);

#ifdef __cplusplus
}
#endif
