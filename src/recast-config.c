/*
 * recast-config.c — JSON config persistence for Recast output targets.
 *
 * Reads/writes <OBS profile dir>/recast-outputs.json.
 */

#include "recast-config.h"
#include "recast-scene-model.h"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>

#define CONFIG_FILENAME "recast-outputs.json"

/* ---- Helpers ---- */

char *recast_config_get_path(void)
{
	char *profile_dir = obs_frontend_get_current_profile_path();
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

/* ---- Scene model persistence ---- */

/*
 * Save scene item transform + visibility into obs_data_t.
 */
static obs_data_t *save_scene_item(obs_sceneitem_t *item)
{
	obs_data_t *d = obs_data_create();

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src) {
		obs_data_set_string(d, "source_name",
				    obs_source_get_name(src));

		/* Check if this is a "known" main OBS source (existing) or
		 * one we created privately. Private sources have no public
		 * name registration, so we save their type + settings. */
		obs_source_t *lookup =
			obs_get_source_by_name(obs_source_get_name(src));
		if (lookup) {
			obs_data_set_bool(d, "is_existing", true);
			obs_source_release(lookup);
		} else {
			obs_data_set_bool(d, "is_existing", false);
			obs_data_set_string(d, "source_type",
					    obs_source_get_id(src));
			obs_data_t *settings = obs_source_get_settings(src);
			if (settings) {
				obs_data_set_obj(d, "source_settings",
						 settings);
				obs_data_release(settings);
			}
		}
	}

	obs_data_set_bool(d, "visible", obs_sceneitem_visible(item));

	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	obs_data_set_double(d, "pos_x", pos.x);
	obs_data_set_double(d, "pos_y", pos.y);

	struct vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	obs_data_set_double(d, "scale_x", scale.x);
	obs_data_set_double(d, "scale_y", scale.y);

	obs_data_set_double(d, "rotation",
			    obs_sceneitem_get_rot(item));

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	obs_data_set_int(d, "crop_left", crop.left);
	obs_data_set_int(d, "crop_right", crop.right);
	obs_data_set_int(d, "crop_top", crop.top);
	obs_data_set_int(d, "crop_bottom", crop.bottom);

	return d;
}

struct save_items_ctx {
	obs_data_array_t *items_arr;
};

static bool save_items_callback(obs_scene_t *scene, obs_sceneitem_t *item,
				void *param)
{
	UNUSED_PARAMETER(scene);
	struct save_items_ctx *ctx = param;

	obs_data_t *item_data = save_scene_item(item);
	obs_data_array_push_back(ctx->items_arr, item_data);
	obs_data_release(item_data);

	return true; /* continue enumeration */
}

obs_data_t *recast_config_save_scene_model(
	const recast_scene_model_t *model)
{
	if (!model)
		return NULL;

	obs_data_t *d = obs_data_create();

	/* Active scene name */
	if (model->active_scene_idx >= 0 &&
	    model->active_scene_idx < model->scene_count) {
		obs_data_set_string(
			d, "active_scene",
			model->scenes[model->active_scene_idx].name);
	}

	/* Scenes array */
	obs_data_array_t *scenes_arr = obs_data_array_create();

	for (int i = 0; i < model->scene_count; i++) {
		const recast_scene_entry_t *e = &model->scenes[i];
		obs_data_t *scene_data = obs_data_create();

		obs_data_set_string(scene_data, "name", e->name);

		/* Scene items */
		obs_data_array_t *items_arr = obs_data_array_create();

		if (e->scene) {
			struct save_items_ctx ctx;
			ctx.items_arr = items_arr;
			obs_scene_enum_items(e->scene, save_items_callback,
					     &ctx);
		}

		obs_data_set_array(scene_data, "items", items_arr);
		obs_data_array_release(items_arr);

		obs_data_array_push_back(scenes_arr, scene_data);
		obs_data_release(scene_data);
	}

	obs_data_set_array(d, "scenes", scenes_arr);
	obs_data_array_release(scenes_arr);

	return d;
}

recast_scene_model_t *recast_config_load_scene_model(obs_data_t *data)
{
	if (!data)
		return NULL;

	recast_scene_model_t *model = recast_scene_model_create();

	obs_data_array_t *scenes_arr = obs_data_get_array(data, "scenes");
	if (!scenes_arr) {
		recast_scene_model_destroy(model);
		return NULL;
	}

	size_t scene_count = obs_data_array_count(scenes_arr);
	for (size_t i = 0; i < scene_count; i++) {
		obs_data_t *scene_data = obs_data_array_item(scenes_arr, i);
		const char *scene_name =
			obs_data_get_string(scene_data, "name");

		int scene_idx =
			recast_scene_model_add_scene(model, scene_name);
		if (scene_idx < 0) {
			obs_data_release(scene_data);
			continue;
		}

		obs_scene_t *scene = model->scenes[scene_idx].scene;

		/* Load scene items */
		obs_data_array_t *items_arr =
			obs_data_get_array(scene_data, "items");
		if (items_arr) {
			size_t item_count = obs_data_array_count(items_arr);
			for (size_t j = 0; j < item_count; j++) {
				obs_data_t *item_data =
					obs_data_array_item(items_arr, j);

				const char *src_name = obs_data_get_string(
					item_data, "source_name");
				bool is_existing = obs_data_get_bool(
					item_data, "is_existing");

				obs_source_t *src = NULL;

				if (is_existing) {
					src = obs_get_source_by_name(src_name);
				} else {
					const char *src_type =
						obs_data_get_string(
							item_data,
							"source_type");
					obs_data_t *src_settings =
						obs_data_get_obj(
							item_data,
							"source_settings");

					if (src_type && *src_type) {
						src = obs_source_create(
							src_type, src_name,
							src_settings, NULL);
					}
					obs_data_release(src_settings);
				}

				if (src) {
					obs_sceneitem_t *si =
						obs_scene_add(scene, src);

					if (si) {
						obs_sceneitem_set_visible(
							si,
							obs_data_get_bool(
								item_data,
								"visible"));

						struct vec2 pos;
						pos.x = (float)obs_data_get_double(
							item_data, "pos_x");
						pos.y = (float)obs_data_get_double(
							item_data, "pos_y");
						obs_sceneitem_set_pos(si, &pos);

						struct vec2 scale;
						scale.x = (float)obs_data_get_double(
							item_data, "scale_x");
						scale.y = (float)obs_data_get_double(
							item_data, "scale_y");
						if (scale.x > 0.0f &&
						    scale.y > 0.0f)
							obs_sceneitem_set_scale(
								si, &scale);

						float rot =
							(float)obs_data_get_double(
								item_data,
								"rotation");
						obs_sceneitem_set_rot(si, rot);

						struct obs_sceneitem_crop crop;
						crop.left =
							(int)obs_data_get_int(
								item_data,
								"crop_left");
						crop.right =
							(int)obs_data_get_int(
								item_data,
								"crop_right");
						crop.top =
							(int)obs_data_get_int(
								item_data,
								"crop_top");
						crop.bottom =
							(int)obs_data_get_int(
								item_data,
								"crop_bottom");
						obs_sceneitem_set_crop(si,
								       &crop);
					}

					obs_source_release(src);
				}

				obs_data_release(item_data);
			}
			obs_data_array_release(items_arr);
		}

		obs_data_release(scene_data);
	}

	/* Set active scene */
	const char *active_name =
		obs_data_get_string(data, "active_scene");
	if (active_name && *active_name) {
		int idx = recast_scene_model_find(model, active_name);
		if (idx >= 0)
			recast_scene_model_set_active(model, idx);
	}

	obs_data_array_release(scenes_arr);
	return model;
}
