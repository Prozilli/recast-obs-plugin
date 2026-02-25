/*
 * plugin-main.c -- Recast OBS Plugin entry point.
 *
 * Registers the output management layer and Qt dock widget for
 * multi-output streaming with protocol-aware service selection.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "recast-output.h"

/* C-only forward declarations for the dock (C++ implementation) */
void recast_dock_create(void);
void recast_dock_destroy(void);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("recast-obs-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Recast Output — Multi-destination streaming with "
	       "per-scene support and protocol detection";
}

/* Frontend event callback: create the dock once OBS is fully loaded */
static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		recast_dock_create();
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Recast] Loading plugin v%s", "2.0.0");

	/* Register output helpers (currently a no-op; uses built-in
	 * rtmp_output/whip_output via the management layer) */
	recast_output_register();

	/* Defer dock creation until OBS UI is ready */
	obs_frontend_add_event_callback(on_frontend_event, NULL);

	blog(LOG_INFO, "[Recast] Plugin loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	recast_dock_destroy();
	blog(LOG_INFO, "[Recast] Plugin unloaded");
}
