/*
 * recast-scene-model.c -- Per-output private scene collection.
 *
 * Each output can own a set of private OBS scenes (invisible to the main UI)
 * that can still reference shared sources like cameras.
 */

#include "recast-scene-model.h"

#include <util/dstr.h>

recast_scene_model_t *recast_scene_model_create(void)
{
	recast_scene_model_t *model =
		bzalloc(sizeof(recast_scene_model_t));
	model->active_scene_idx = -1;
	return model;
}

void recast_scene_model_destroy(recast_scene_model_t *model)
{
	if (!model)
		return;

	for (int i = 0; i < model->scene_count; i++) {
		recast_scene_entry_t *e = &model->scenes[i];

		if (e->scene) {
			obs_scene_release(e->scene);
			e->scene = NULL;
		}
		e->scene_source = NULL;

		bfree(e->name);
		e->name = NULL;
	}

	bfree(model);
}

int recast_scene_model_add_scene(recast_scene_model_t *model,
				 const char *name)
{
	if (!model || !name || !*name)
		return -1;
	if (model->scene_count >= RECAST_MAX_SCENES)
		return -1;

	/* Create a private scene (invisible to main OBS UI) */
	obs_scene_t *scene = obs_scene_create_private(name);
	if (!scene)
		return -1;

	int idx = model->scene_count;
	recast_scene_entry_t *e = &model->scenes[idx];
	e->name = bstrdup(name);
	e->scene = scene;
	e->scene_source = obs_scene_get_source(scene);
	model->scene_count++;

	/* If this is the first scene, make it active */
	if (model->active_scene_idx < 0)
		model->active_scene_idx = idx;

	return idx;
}

bool recast_scene_model_remove_scene(recast_scene_model_t *model, int idx)
{
	if (!model || idx < 0 || idx >= model->scene_count)
		return false;

	recast_scene_entry_t *e = &model->scenes[idx];

	if (e->scene) {
		obs_scene_release(e->scene);
		e->scene = NULL;
	}
	e->scene_source = NULL;
	bfree(e->name);
	e->name = NULL;

	/* Shift remaining entries down */
	for (int i = idx; i < model->scene_count - 1; i++)
		model->scenes[i] = model->scenes[i + 1];

	model->scene_count--;

	/* Clear the last slot */
	memset(&model->scenes[model->scene_count], 0,
	       sizeof(recast_scene_entry_t));

	/* Adjust active index */
	if (model->scene_count == 0) {
		model->active_scene_idx = -1;
	} else if (model->active_scene_idx == idx) {
		model->active_scene_idx =
			idx < model->scene_count ? idx : model->scene_count - 1;
	} else if (model->active_scene_idx > idx) {
		model->active_scene_idx--;
	}

	return true;
}

bool recast_scene_model_rename_scene(recast_scene_model_t *model, int idx,
				     const char *new_name)
{
	if (!model || idx < 0 || idx >= model->scene_count)
		return false;
	if (!new_name || !*new_name)
		return false;

	recast_scene_entry_t *e = &model->scenes[idx];
	bfree(e->name);
	e->name = bstrdup(new_name);

	return true;
}

void recast_scene_model_set_active(recast_scene_model_t *model, int idx)
{
	if (!model)
		return;
	if (idx < 0 || idx >= model->scene_count)
		return;
	model->active_scene_idx = idx;
}

obs_scene_t *recast_scene_model_get_active_scene(
	const recast_scene_model_t *model)
{
	if (!model || model->active_scene_idx < 0 ||
	    model->active_scene_idx >= model->scene_count)
		return NULL;
	return model->scenes[model->active_scene_idx].scene;
}

obs_source_t *recast_scene_model_get_active_source(
	const recast_scene_model_t *model)
{
	if (!model || model->active_scene_idx < 0 ||
	    model->active_scene_idx >= model->scene_count)
		return NULL;
	return model->scenes[model->active_scene_idx].scene_source;
}

int recast_scene_model_find(const recast_scene_model_t *model,
			    const char *name)
{
	if (!model || !name)
		return -1;

	for (int i = 0; i < model->scene_count; i++) {
		if (model->scenes[i].name &&
		    strcmp(model->scenes[i].name, name) == 0)
			return i;
	}
	return -1;
}
