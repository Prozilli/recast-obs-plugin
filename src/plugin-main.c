/*
 * plugin-main.c -- Recast OBS Plugin entry point.
 *
 * Creates 4 always-present docks on plugin load:
 *   - Recast Vertical Preview (9:16 canvas)
 *   - Recast Vertical Scenes
 *   - Recast Vertical Sources
 *   - Recast Multistream (destination list)
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "recast-output.h"

/* C-only forward declarations for the new dock system (C++ implementation) */
void recast_ui_create(void);
void recast_ui_destroy(void);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("recast-obs-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Recast — Standalone vertical canvas + simple multistream";
}

/* Frontend event callback: create docks once OBS is fully loaded */
static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		recast_ui_create();
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Recast] Loading plugin v%s", "3.0.0");

	recast_output_register();

	/* Defer dock creation until OBS UI is ready */
	obs_frontend_add_event_callback(on_frontend_event, NULL);

	blog(LOG_INFO, "[Recast] Plugin loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	recast_ui_destroy();
	blog(LOG_INFO, "[Recast] Plugin unloaded");
}
