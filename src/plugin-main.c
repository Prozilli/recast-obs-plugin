/*
 * plugin-main.c — Recast OBS Plugin entry point.
 *
 * Registers the custom RTMP service, output management layer,
 * and Qt dock widget for multi-output streaming.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "recast-output.h"
#include "recast-service.h"
#include "recast-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("recast-obs-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Recast Output — Multi-destination RTMP streaming with "
	       "per-scene support";
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
	blog(LOG_INFO, "[Recast] Loading plugin v%s", "1.0.0");

	/* Register custom RTMP service type */
	recast_service_register();

	/* Register output helpers (currently a no-op; uses built-in
	 * rtmp_output via the management layer) */
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
