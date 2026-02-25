/*
 * recast-config.c — JSON config persistence for Recast output targets.
 *
 * Reads/writes <OBS profile dir>/recast-outputs.json.
 */

#include "recast-config.h"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>

#define CONFIG_FILENAME "recast-outputs.json"

/* ---- Helpers ---- */

char *recast_config_get_path(void)
{
	char *profile_dir = obs_frontend_get_current_profile_path(NULL);
	if (!profile_dir)
		return NULL;

	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", profile_dir, CONFIG_FILENAME);
	bfree(profile_dir);

	return path.array; /* caller must bfree() */
}

static obs_data_t *load_root(void)
{
	char *path = recast_config_get_path();
	if (!path)
		return NULL;

	obs_data_t *root = obs_data_create_from_json_file(path);
	bfree(path);
	return root;
}

static bool save_root(obs_data_t *root)
{
	char *path = recast_config_get_path();
	if (!path)
		return false;

	bool ok = obs_data_save_json(root, path);
	if (ok) {
		blog(LOG_INFO, "[Recast] Config saved to %s", path);
	} else {
		blog(LOG_ERROR, "[Recast] Failed to save config to %s", path);
	}

	bfree(path);
	return ok;
}

/* ---- Public API ---- */

obs_data_array_t *recast_config_load(void)
{
	obs_data_t *root = load_root();
	if (!root)
		return NULL;

	obs_data_array_t *arr = obs_data_get_array(root, "outputs");
	obs_data_release(root);
	return arr; /* caller must obs_data_array_release() */
}

bool recast_config_save(obs_data_array_t *targets)
{
	obs_data_t *root = load_root();
	if (!root)
		root = obs_data_create();

	obs_data_set_array(root, "outputs", targets);

	bool ok = save_root(root);
	obs_data_release(root);
	return ok;
}

char *recast_config_get_server_token(void)
{
	obs_data_t *root = load_root();
	if (!root)
		return bstrdup("");

	const char *token = obs_data_get_string(root, "server_token");
	char *result = bstrdup(token ? token : "");
	obs_data_release(root);
	return result;
}

bool recast_config_set_server_token(const char *token)
{
	obs_data_t *root = load_root();
	if (!root)
		root = obs_data_create();

	obs_data_set_string(root, "server_token", token ? token : "");

	bool ok = save_root(root);
	obs_data_release(root);
	return ok;
}
