/*
 * recast-output.c -- Multi-output target management for Recast.
 *
 * Each "target" wraps an OBS output instance paired with a built-in service
 * (rtmp_custom or whip_custom) for URL/key storage. Supports shared encoding
 * (zero-overhead multistream) and custom per-output encoding.
 *
 * When a target uses a scene different from the main output (or a different
 * resolution), a dedicated obs_view_t + encoder pair is spun up.
 */

#include "recast-output.h"
#include "recast-protocol.h"

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

static obs_encoder_t *get_main_audio_encoder_track(int track)
{
	obs_output_t *main_out = obs_frontend_get_streaming_output();
	if (!main_out)
		return NULL;
	obs_encoder_t *enc = obs_output_get_audio_encoder(main_out,
							  (size_t)track);
	obs_output_release(main_out);
	return enc;
}

static char *generate_unique_id(const char *name)
{
	struct dstr id = {0};
	dstr_printf(&id, "recast_out_%s_%llu", name,
		    (unsigned long long)os_gettime_ns());
	return id.array;
}

/* ---- Per-scene view setup ---- */

/*
 * setup_custom_view_with_source: creates a view + encoder pair using the
 * provided scene source directly (instead of looking it up by name).
 */
static bool setup_custom_view_with_source(recast_output_target_t *t,
					  obs_source_t *scene_src)
{
	if (!scene_src) {
		blog(LOG_WARNING,
		     "[Recast] No scene source for view '%s'", t->name);
		return false;
	}

	/* Create a dedicated view rendering this scene */
	t->view = obs_view_create();
	obs_view_set_source(t->view, 0, scene_src);

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

	/* Create a dedicated video encoder for this view */
	const char *enc_id = t->encoder_id && *t->encoder_id
				     ? t->encoder_id
				     : "obs_x264";
	int bitrate = t->custom_bitrate > 0 ? t->custom_bitrate : 4000;

	obs_data_t *enc_settings;
	if (t->encoder_settings) {
		enc_settings = obs_data_create();
		obs_data_apply(enc_settings, t->encoder_settings);
	} else {
		enc_settings = obs_data_create();
	}
	obs_data_set_int(enc_settings, "bitrate", bitrate);
	if (!obs_data_has_user_value(enc_settings, "rate_control"))
		obs_data_set_string(enc_settings, "rate_control", "CBR");

	t->video_encoder = obs_video_encoder_create(
		enc_id, "recast_video_enc", enc_settings, NULL);
	obs_data_release(enc_settings);

	if (!t->video_encoder) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create video encoder '%s' for '%s'",
		     enc_id, t->name);
		obs_view_remove(t->view);
		obs_view_destroy(t->view);
		t->view = NULL;
		t->video = NULL;
		return false;
	}

	obs_encoder_set_video(t->video_encoder, t->video);

	/* Use main audio encoder (audio doesn't depend on scene) */
	if (t->audio_track > 0)
		t->audio_encoder = get_main_audio_encoder_track(t->audio_track);
	else
		t->audio_encoder = get_main_audio_encoder();

	return true;
}

/*
 * setup_custom_encoder_main_video: creates a custom encoder on the main
 * video output (same canvas, different encoder settings).
 */
static bool setup_custom_encoder_main_video(recast_output_target_t *t)
{
	const char *enc_id = t->encoder_id && *t->encoder_id
				     ? t->encoder_id
				     : "obs_x264";
	int bitrate = t->custom_bitrate > 0 ? t->custom_bitrate : 4000;

	obs_data_t *enc_settings;
	if (t->encoder_settings) {
		enc_settings = obs_data_create();
		obs_data_apply(enc_settings, t->encoder_settings);
	} else {
		enc_settings = obs_data_create();
	}
	obs_data_set_int(enc_settings, "bitrate", bitrate);
	if (!obs_data_has_user_value(enc_settings, "rate_control"))
		obs_data_set_string(enc_settings, "rate_control", "CBR");

	t->video_encoder = obs_video_encoder_create(
		enc_id, "recast_custom_venc", enc_settings, NULL);
	obs_data_release(enc_settings);

	if (!t->video_encoder) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create custom encoder '%s' for '%s'",
		     enc_id, t->name);
		return false;
	}

	obs_encoder_set_video(t->video_encoder, obs_get_video());

	if (t->audio_track > 0)
		t->audio_encoder = get_main_audio_encoder_track(t->audio_track);
	else
		t->audio_encoder = get_main_audio_encoder();

	return true;
}

/*
 * Legacy wrapper: resolves scene by name, then delegates.
 */
static bool setup_custom_view(recast_output_target_t *t)
{
	/* If using private scenes, use the active scene from the model */
	if (t->use_private_scenes && t->scene_model) {
		obs_source_t *src =
			recast_scene_model_get_active_source(t->scene_model);
		return setup_custom_view_with_source(t, src);
	}

	/* Legacy path: look up by name */
	obs_source_t *scene_src = obs_get_source_by_name(t->scene_name);
	if (!scene_src) {
		blog(LOG_WARNING,
		     "[Recast] Scene '%s' not found, using main",
		     t->scene_name);
		return false;
	}

	bool ok = setup_custom_view_with_source(t, scene_src);
	obs_source_release(scene_src);
	return ok;
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

/* ---- View-only setup for preview (no encoders) ---- */

static void teardown_view_only(recast_output_target_t *t)
{
	/* Only tear down if there are no encoders (preview-only mode) */
	if (t->video_encoder)
		return;

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

	t->id = generate_unique_id(name);
	t->name = bstrdup(name);
	t->url = bstrdup(url);
	t->key = bstrdup(key);
	t->scene_name = scene_name && *scene_name ? bstrdup(scene_name) : NULL;
	t->enabled = true;
	t->auto_start = false;
	t->auto_stop = false;
	t->width = width;
	t->height = height;
	t->use_private_scenes = false;
	t->scene_model = NULL;

	/* Protocol detection */
	t->protocol = recast_protocol_detect(url);

	/* Encoding defaults */
	t->advanced_encoder = false;
	t->encoder_id = NULL;
	t->encoder_settings = NULL;
	t->audio_track = 0;
	t->custom_bitrate = 0;

	/* Recording defaults */
	t->rec_output = NULL;
	t->rec_video_encoder = NULL;
	t->rec_active = false;
	t->rec_enabled = false;
	t->rec_path = NULL;
	t->rec_format = NULL;
	t->rec_bitrate = 0;

	/* Virtual camera defaults */
	t->virtualcam_output = NULL;
	t->virtualcam_active = false;

	/* Create the service for this target using built-in type */
	const char *service_id = recast_protocol_service_id(t->protocol);

	obs_data_t *svc_settings = obs_data_create();
	if (t->protocol == RECAST_PROTO_WHIP) {
		obs_data_set_string(svc_settings, "server", url);
		obs_data_set_string(svc_settings, "bearer_token", key);
	} else {
		obs_data_set_string(svc_settings, "server", url);
		obs_data_set_string(svc_settings, "key", key);
	}

	struct dstr svc_name = {0};
	dstr_printf(&svc_name, "recast_svc_%s", name);
	t->service = obs_service_create(
		service_id, svc_name.array, svc_settings, NULL);
	dstr_free(&svc_name);
	obs_data_release(svc_settings);

	/* Create the output using protocol-appropriate type */
	const char *output_id = recast_protocol_output_id(t->protocol);

	struct dstr out_name = {0};
	dstr_printf(&out_name, "recast_out_%s", name);
	t->output = obs_output_create(
		output_id, out_name.array, NULL, NULL);
	dstr_free(&out_name);

	if (!t->output) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create %s output for '%s'",
		     output_id, name);
	}

	blog(LOG_INFO,
	     "[Recast] Created output '%s' (protocol=%s, service=%s, output=%s)",
	     name, recast_protocol_name(t->protocol), service_id, output_id);

	return t;
}

void recast_output_target_destroy(recast_output_target_t *t)
{
	if (!t)
		return;

	if (t->active)
		recast_output_target_stop(t);

	if (t->rec_active)
		recast_output_target_stop_recording(t);

	if (t->virtualcam_active)
		recast_output_target_stop_virtualcam(t);

	teardown_custom_view(t);
	teardown_view_only(t);

	if (t->scene_model) {
		recast_scene_model_destroy(t->scene_model);
		t->scene_model = NULL;
	}

	if (t->output)
		obs_output_release(t->output);
	if (t->service)
		obs_service_release(t->service);

	if (t->rec_output)
		obs_output_release(t->rec_output);
	if (t->rec_video_encoder)
		obs_encoder_release(t->rec_video_encoder);
	if (t->virtualcam_output)
		obs_output_release(t->virtualcam_output);

	if (t->encoder_settings)
		obs_data_release(t->encoder_settings);

	bfree(t->id);
	bfree(t->name);
	bfree(t->url);
	bfree(t->key);
	bfree(t->scene_name);
	bfree(t->encoder_id);
	bfree(t->rec_path);
	bfree(t->rec_format);
	bfree(t);
}

bool recast_output_target_start(recast_output_target_t *t)
{
	if (!t || !t->output || !t->service)
		return false;
	if (t->active)
		return true;

	obs_output_set_service(t->output, t->service);

	/* Determine encoding mode */
	if (t->advanced_encoder) {
		/* Custom encoding mode */
		bool needs_custom_view = false;

		if (t->use_private_scenes && t->scene_model) {
			needs_custom_view = true;
		} else {
			bool uses_custom_scene =
				t->scene_name && *t->scene_name;
			bool uses_custom_resolution =
				t->width > 0 && t->height > 0;
			needs_custom_view =
				uses_custom_scene || uses_custom_resolution;
		}

		if (needs_custom_view) {
			/* Tear down any preview-only view first */
			teardown_view_only(t);

			if (!setup_custom_view(t)) {
				/* Fallback: custom encoder on main video */
				if (!setup_custom_encoder_main_video(t))
					return false;
			}
		} else {
			/* Same canvas but custom encoder */
			if (!setup_custom_encoder_main_video(t))
				return false;
		}

		obs_output_set_video_encoder(t->output, t->video_encoder);

		if (t->audio_encoder) {
			obs_output_set_audio_encoder(t->output,
						     t->audio_encoder, 0);
		} else {
			obs_encoder_t *main_aenc = get_main_audio_encoder();
			if (!main_aenc) {
				blog(LOG_ERROR,
				     "[Recast] No main audio encoder");
				teardown_custom_view(t);
				return false;
			}
			obs_output_set_audio_encoder(t->output,
						     main_aenc, 0);
		}
	} else {
		/* Shared encoding mode: zero additional CPU */
		bool needs_custom = false;

		if (t->use_private_scenes && t->scene_model) {
			needs_custom = true;
		} else {
			bool uses_custom_scene =
				t->scene_name && *t->scene_name;
			bool uses_custom_resolution =
				t->width > 0 && t->height > 0;
			needs_custom =
				uses_custom_scene || uses_custom_resolution;
		}

		if (needs_custom) {
			/* Tear down any preview-only view first */
			teardown_view_only(t);

			if (!setup_custom_view(t)) {
				needs_custom = false;
			}
		}

		if (t->video_encoder) {
			obs_output_set_video_encoder(t->output,
						     t->video_encoder);
		} else {
			/* Share the main encoder -- zero extra CPU */
			obs_encoder_t *main_venc = get_main_video_encoder();
			if (!main_venc) {
				blog(LOG_ERROR,
				     "[Recast] No main video encoder "
				     "(is main stream running?)");
				return false;
			}
			obs_output_set_video_encoder(t->output, main_venc);
		}

		if (t->audio_encoder) {
			obs_output_set_audio_encoder(t->output,
						     t->audio_encoder, 0);
		} else {
			obs_encoder_t *main_aenc = get_main_audio_encoder();
			if (!main_aenc) {
				blog(LOG_ERROR,
				     "[Recast] No main audio encoder");
				return false;
			}
			obs_output_set_audio_encoder(t->output,
						     main_aenc, 0);
		}
	}

	bool ok = obs_output_start(t->output);
	if (ok) {
		t->active = true;
		t->start_time_ns = os_gettime_ns();
		blog(LOG_INFO, "[Recast] Started output '%s' -> %s (%s mode)",
		     t->name, t->url,
		     t->advanced_encoder ? "custom" : "shared");
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

void recast_output_target_bind_active_scene(recast_output_target_t *t)
{
	if (!t || !t->scene_model || !t->view)
		return;

	obs_source_t *src =
		recast_scene_model_get_active_source(t->scene_model);
	if (src)
		obs_view_set_source(t->view, 0, src);
}

bool recast_output_target_ensure_view(recast_output_target_t *t,
				      obs_source_t *scene_src)
{
	if (!t || !scene_src)
		return false;

	/* Already have a view? Just update the source. */
	if (t->view) {
		obs_view_set_source(t->view, 0, scene_src);
		return true;
	}

	/* Create a view without encoders (preview-only) */
	t->view = obs_view_create();
	obs_view_set_source(t->view, 0, scene_src);

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
		     "[Recast] Failed to create preview video for '%s'",
		     t->name);
		obs_view_destroy(t->view);
		t->view = NULL;
		return false;
	}

	return true;
}

/* ---- Recording ---- */

bool recast_output_target_start_recording(recast_output_target_t *t)
{
	if (!t || t->rec_active)
		return false;

	if (!t->rec_path || !*t->rec_path) {
		blog(LOG_WARNING,
		     "[Recast] No recording path set for '%s'", t->name);
		return false;
	}

	/* Build filename with timestamp */
	struct dstr filename = {0};
	const char *ext = "mkv";
	if (t->rec_format && *t->rec_format)
		ext = t->rec_format;

	char time_buf[64];
	{
		time_t now = time(NULL);
		struct tm tm_storage;
#ifdef _MSC_VER
		_localtime64_s(&tm_storage, &now);
#else
		localtime_r(&now, &tm_storage);
#endif
		strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%M-%S",
			 &tm_storage);
	}

	dstr_printf(&filename, "%s/%s_%s.%s",
		    t->rec_path, t->name, time_buf, ext);

	/* Create recording output */
	obs_data_t *rec_settings = obs_data_create();
	obs_data_set_string(rec_settings, "path", filename.array);
	dstr_free(&filename);

	t->rec_output = obs_output_create(
		"ffmpeg_muxer", "recast_rec_output", rec_settings, NULL);
	obs_data_release(rec_settings);

	if (!t->rec_output) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create recording output for '%s'",
		     t->name);
		return false;
	}

	/* Create recording video encoder */
	int bitrate = t->rec_bitrate > 0 ? t->rec_bitrate
					  : (t->custom_bitrate > 0
						     ? t->custom_bitrate
						     : 4000);
	const char *enc_id = t->encoder_id && *t->encoder_id
				     ? t->encoder_id
				     : "obs_x264";

	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_int(enc_settings, "bitrate", bitrate);
	obs_data_set_string(enc_settings, "rate_control", "CBR");

	t->rec_video_encoder = obs_video_encoder_create(
		enc_id, "recast_rec_venc", enc_settings, NULL);
	obs_data_release(enc_settings);

	if (!t->rec_video_encoder) {
		obs_output_release(t->rec_output);
		t->rec_output = NULL;
		return false;
	}

	/* Use the output's video if available, else main */
	if (t->video)
		obs_encoder_set_video(t->rec_video_encoder, t->video);
	else
		obs_encoder_set_video(t->rec_video_encoder, obs_get_video());

	obs_output_set_video_encoder(t->rec_output, t->rec_video_encoder);

	obs_encoder_t *aenc = get_main_audio_encoder();
	if (aenc)
		obs_output_set_audio_encoder(t->rec_output, aenc, 0);

	bool ok = obs_output_start(t->rec_output);
	if (ok) {
		t->rec_active = true;
		blog(LOG_INFO, "[Recast] Started recording for '%s'",
		     t->name);
	} else {
		blog(LOG_ERROR,
		     "[Recast] Failed to start recording for '%s'",
		     t->name);
		obs_encoder_release(t->rec_video_encoder);
		t->rec_video_encoder = NULL;
		obs_output_release(t->rec_output);
		t->rec_output = NULL;
	}

	return ok;
}

void recast_output_target_stop_recording(recast_output_target_t *t)
{
	if (!t || !t->rec_active)
		return;

	if (t->rec_output)
		obs_output_stop(t->rec_output);

	t->rec_active = false;

	if (t->rec_video_encoder) {
		obs_encoder_release(t->rec_video_encoder);
		t->rec_video_encoder = NULL;
	}
	if (t->rec_output) {
		obs_output_release(t->rec_output);
		t->rec_output = NULL;
	}

	blog(LOG_INFO, "[Recast] Stopped recording for '%s'", t->name);
}

/* ---- Virtual Camera ---- */

bool recast_output_target_start_virtualcam(recast_output_target_t *t)
{
	if (!t || t->virtualcam_active)
		return false;

	obs_data_t *vcam_settings = obs_data_create();
	t->virtualcam_output = obs_output_create(
		"virtualcam_output", "recast_vcam", vcam_settings, NULL);
	obs_data_release(vcam_settings);

	if (!t->virtualcam_output) {
		blog(LOG_ERROR,
		     "[Recast] Failed to create virtual camera for '%s'",
		     t->name);
		return false;
	}

	/* Use the output's video if available */
	if (t->video)
		obs_output_set_media(t->virtualcam_output, t->video,
				     obs_get_audio());
	else
		obs_output_set_media(t->virtualcam_output, obs_get_video(),
				     obs_get_audio());

	bool ok = obs_output_start(t->virtualcam_output);
	if (ok) {
		t->virtualcam_active = true;
		blog(LOG_INFO, "[Recast] Started virtual camera for '%s'",
		     t->name);
	} else {
		blog(LOG_ERROR,
		     "[Recast] Failed to start virtual camera for '%s'",
		     t->name);
		obs_output_release(t->virtualcam_output);
		t->virtualcam_output = NULL;
	}

	return ok;
}

void recast_output_target_stop_virtualcam(recast_output_target_t *t)
{
	if (!t || !t->virtualcam_active)
		return;

	if (t->virtualcam_output)
		obs_output_stop(t->virtualcam_output);

	t->virtualcam_active = false;

	if (t->virtualcam_output) {
		obs_output_release(t->virtualcam_output);
		t->virtualcam_output = NULL;
	}

	blog(LOG_INFO, "[Recast] Stopped virtual camera for '%s'", t->name);
}

void recast_output_register(void)
{
	/* No custom obs_output_info needed -- we use built-in outputs.
	 * This function is a placeholder for future custom output
	 * registration if protocol-level control is ever needed. */
}
