#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECAST_MAX_SCENES 32

typedef struct recast_scene_entry {
	char *name;
	obs_scene_t *scene;         /* obs_scene_create_private() */
	obs_source_t *scene_source; /* obs_scene_get_source(scene), not addref'd */
} recast_scene_entry_t;

typedef struct recast_scene_model {
	recast_scene_entry_t scenes[RECAST_MAX_SCENES];
	int scene_count;
	int active_scene_idx;
} recast_scene_model_t;

/* Create / destroy the model */
recast_scene_model_t *recast_scene_model_create(void);
void recast_scene_model_destroy(recast_scene_model_t *model);

/* Add a new private scene. Returns index or -1 on failure. */
int recast_scene_model_add_scene(recast_scene_model_t *model,
				 const char *name);

/* Remove a scene by index. Returns true on success. */
bool recast_scene_model_remove_scene(recast_scene_model_t *model, int idx);

/* Rename a scene by index. Returns true on success. */
bool recast_scene_model_rename_scene(recast_scene_model_t *model, int idx,
				     const char *new_name);

/* Set the active scene index. */
void recast_scene_model_set_active(recast_scene_model_t *model, int idx);

/* Get the active scene (NULL if none). */
obs_scene_t *recast_scene_model_get_active_scene(
	const recast_scene_model_t *model);

/* Get the active scene's source (NULL if none). Not addref'd. */
obs_source_t *recast_scene_model_get_active_source(
	const recast_scene_model_t *model);

/* Find a scene index by name. Returns -1 if not found. */
int recast_scene_model_find(const recast_scene_model_t *model,
			    const char *name);

#ifdef __cplusplus
}
#endif
