#pragma once

#include <obs-module.h>
#include "recast-output.h"
#include "recast-scene-model.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Config file: <OBS profile dir>/recast-outputs.json
 *
 * Format:
 * {
 *   "outputs": [
 *     {
 *       "name": "Twitch",
 *       "url": "rtmp://live.twitch.tv/app",
 *       "key": "live_xxx",
 *       "scene": "Main Gaming",
 *       "enabled": true,
 *       "autoStart": false,
 *       "width": 0,
 *       "height": 0
 *     }
 *   ],
 *   "server_token": ""
 * }
 */

/* Load all output target configs from JSON. Returns an obs_data_array_t* that
 * the caller must release with obs_data_array_release().  Returns NULL on
 * error or if the config file does not exist yet. */
obs_data_array_t *recast_config_load(void);

/* Save an array of output target configs to JSON. Each element must have:
 *   name, url, key, scene, enabled, autoStart, width, height */
bool recast_config_save(obs_data_array_t *targets);

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
