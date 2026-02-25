/*
 * recast-output.c — Multi-output target management for Recast.
 *
 * Each "target" wraps an instance of OBS's built-in "rtmp_output" paired with
 * a "recast_rtmp_service" for URL/key storage.  When a target uses a scene
 * different from the main output (or a different resolution), a dedicated
 * obs_view_t + encoder pair is spun up.
 */

#include "recast-output.h"
#include "recast-service.h"

#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>

/* ---- Helpers ---- */

static obs_encoder_t *get_main_video_encoder(void)
{
	obs_output_t *main_out = obs_frontend_get_streaming_output();
	if (!main_out)
		return NULL;
	obs_encoder_t *enc = obs_output_get_video_encoder(main_out);
	obs_output_release(main_out);
	return enc;
}

static obs_encoder_t *get_main_audio_encoder(void)
{
	obs_output_t *main_out = obs_frontend_get_streaming_output();
	if (!main_out)
		return NULL;
	obs_encoder_t *enc = obs_output_get_audio_encoder(main_out, 0);
	obs_output_release(main_out);
	return enc;
}

/* ---- Per-scene view setup (Phase 3) ---- */

static bool setup_custom_view(recast_output_target_t *t)
{
	obs_source_t *scene_src =
		obs_get_source_by_name(t->scene_name);
	if (!scene_src) {
		blog(LOG_WARNING,
		     "[Recast] Scene '%s' not found, using main",
		     t->scene_name);
		return false;
	}

	/* Create a dedicated view rendering this scene */
	t->view = obs_view_create();
	obs_view_set_source(t->view, 0, scene_src);
	obs_source_release(scene_src);

	/* Set up a custom video output at the target resolution */
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	int w = t->width > 0 ? t->width : (int)ovi.base_width;
	int h = t->height > 0 ? t->height : (int)ovi.base_height;

	struct obs_video_info custom_ovi = ovi;
	custom_ovi.base_width = (uint32_t)w;
	custom_ovi.base_height = (uint32_t)h;
	custom_ovi.output_width = (uint32_t)w;
	custom_ovi.output_height = (uint32_t)h;

	t->video = obs_view_add2(t->view, &custom_ovi);
	if (!t->video) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create video for view '%s'",
		     t->name);
		obs_view_destroy(t->view);
		t->view = NULL;
		return false;
	}

	/* Create a dedicated x264 encoder for this view */
	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_int(enc_settings, "bitrate", 4000);
	obs_data_set_string(enc_settings, "rate_control", "CBR");

	t->video_encoder = obs_video_encoder_create(
		"obs_x264", "recast_video_enc", enc_settings, NULL);
	obs_data_release(enc_settings);

	if (!t->video_encoder) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create video encoder for '%s'",
		     t->name);
		obs_view_remove(t->view);
		obs_view_destroy(t->view);
		t->view = NULL;
		t->video = NULL;
		return false;
	}

	obs_encoder_set_video(t->video_encoder, t->video);

	/* Use main audio encoder (audio doesn't depend on scene) */
	t->audio_encoder = get_main_audio_encoder();

	return true;
}

static void teardown_custom_view(recast_output_target_t *t)
{
	if (t->video_encoder) {
		obs_encoder_release(t->video_encoder);
		t->video_encoder = NULL;
	}
	t->audio_encoder = NULL; /* not owned by us */

	if (t->view && t->video) {
		obs_view_remove(t->view);
		t->video = NULL;
	}
	if (t->view) {
		obs_view_set_source(t->view, 0, NULL);
		obs_view_destroy(t->view);
		t->view = NULL;
	}
}

/* ---- Public API ---- */

recast_output_target_t *recast_output_target_create(const char *name,
						    const char *url,
						    const char *key,
						    const char *scene_name,
						    int width, int height)
{
	recast_output_target_t *t =
		bzalloc(sizeof(recast_output_target_t));

	t->name = bstrdup(name);
	t->url = bstrdup(url);
	t->key = bstrdup(key);
	t->scene_name = scene_name && *scene_name ? bstrdup(scene_name) : NULL;
	t->enabled = true;
	t->auto_start = false;
	t->width = width;
	t->height = height;

	/* Create the RTMP service for this target */
	obs_data_t *svc_settings = obs_data_create();
	obs_data_set_string(svc_settings, "url", url);
	obs_data_set_string(svc_settings, "key", key);

	struct dstr svc_name = {0};
	dstr_printf(&svc_name, "recast_svc_%s", name);
	t->service = obs_service_create(
		RECAST_SERVICE_ID, svc_name.array, svc_settings, NULL);
	dstr_free(&svc_name);
	obs_data_release(svc_settings);

	/* Create the RTMP output (built-in) */
	struct dstr out_name = {0};
	dstr_printf(&out_name, "recast_out_%s", name);
	t->output = obs_output_create(
		"rtmp_output", out_name.array, NULL, NULL);
	dstr_free(&out_name);

	if (!t->output) {
		blog(LOG_ERROR, "[Recast] Failed to create rtmp_output for '%s'",
		     name);
	}

	return t;
}

void recast_output_target_destroy(recast_output_target_t *t)
{
	if (!t)
		return;

	if (t->active)
		recast_output_target_stop(t);

	teardown_custom_view(t);

	if (t->output)
		obs_output_release(t->output);
	if (t->service)
		obs_service_release(t->service);

	bfree(t->name);
	bfree(t->url);
	bfree(t->key);
	bfree(t->scene_name);
	bfree(t);
}

bool recast_output_target_start(recast_output_target_t *t)
{
	if (!t || !t->output || !t->service)
		return false;
	if (t->active)
		return true;

	obs_output_set_service(t->output, t->service);

	bool uses_custom_scene = t->scene_name && *t->scene_name;
	bool uses_custom_resolution = t->width > 0 && t->height > 0;

	if (uses_custom_scene || uses_custom_resolution) {
		/* Phase 3: per-scene rendering with optional custom resolution */
		if (!setup_custom_view(t)) {
			/* Fall back to main encoder */
			uses_custom_scene = false;
		}
	}

	if (t->video_encoder) {
		obs_output_set_video_encoder(t->output, t->video_encoder);
	} else {
		/* Share the main encoder — zero extra CPU */
		obs_encoder_t *main_venc = get_main_video_encoder();
		if (!main_venc) {
			blog(LOG_ERROR,
			     "[Recast] No main video encoder available");
			return false;
		}
		obs_output_set_video_encoder(t->output, main_venc);
	}

	if (t->audio_encoder) {
		obs_output_set_audio_encoder(t->output, t->audio_encoder, 0);
	} else {
		obs_encoder_t *main_aenc = get_main_audio_encoder();
		if (!main_aenc) {
			blog(LOG_ERROR,
			     "[Recast] No main audio encoder available");
			return false;
		}
		obs_output_set_audio_encoder(t->output, main_aenc, 0);
	}

	bool ok = obs_output_start(t->output);
	if (ok) {
		t->active = true;
		t->start_time_ns = os_gettime_ns();
		blog(LOG_INFO, "[Recast] Started output '%s' -> %s",
		     t->name, t->url);
	} else {
		blog(LOG_ERROR, "[Recast] Failed to start output '%s'",
		     t->name);
		teardown_custom_view(t);
	}

	return ok;
}

void recast_output_target_stop(recast_output_target_t *t)
{
	if (!t || !t->active)
		return;

	obs_output_stop(t->output);
	t->active = false;
	t->start_time_ns = 0;

	teardown_custom_view(t);

	blog(LOG_INFO, "[Recast] Stopped output '%s'", t->name);
}

uint64_t recast_output_target_elapsed_sec(const recast_output_target_t *t)
{
	if (!t || !t->active || t->start_time_ns == 0)
		return 0;
	return (os_gettime_ns() - t->start_time_ns) / 1000000000ULL;
}

void recast_output_register(void)
{
	/* No custom obs_output_info needed — we use the built-in rtmp_output.
	 * This function is a placeholder for future custom output registration
	 * if protocol-level control is ever needed. */
}
