/*
 * recast-service.c — Custom RTMP service for Recast multi-output.
 *
 * Stores an RTMP URL + stream key per output target.  Registered as
 * "recast_rtmp_service" so each output instance gets its own service.
 */

#include "recast-service.h"

/* ---- Private data ---- */

struct recast_service_data {
	char *url;
	char *key;
};

/* ---- Callbacks ---- */

static const char *recast_service_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RecastService.Name");
}

static void *recast_service_create(obs_data_t *settings, obs_service_t *svc)
{
	UNUSED_PARAMETER(svc);

	struct recast_service_data *data =
		bzalloc(sizeof(struct recast_service_data));
	data->url = bstrdup(obs_data_get_string(settings, "url"));
	data->key = bstrdup(obs_data_get_string(settings, "key"));
	return data;
}

static void recast_service_destroy(void *priv)
{
	struct recast_service_data *data = priv;
	if (!data)
		return;
	bfree(data->url);
	bfree(data->key);
	bfree(data);
}

static void recast_service_update(void *priv, obs_data_t *settings)
{
	struct recast_service_data *data = priv;
	bfree(data->url);
	bfree(data->key);
	data->url = bstrdup(obs_data_get_string(settings, "url"));
	data->key = bstrdup(obs_data_get_string(settings, "key"));
}

static obs_properties_t *recast_service_properties(void *priv)
{
	UNUSED_PARAMETER(priv);

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "url",
				obs_module_text("RecastService.URL"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "key",
				obs_module_text("RecastService.Key"),
				OBS_TEXT_PASSWORD);
	return props;
}

static void recast_service_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_string(settings, "key", "");
}

static const char *recast_service_get_url(void *priv)
{
	struct recast_service_data *data = priv;
	return data->url;
}

static const char *recast_service_get_key(void *priv)
{
	struct recast_service_data *data = priv;
	return data->key;
}

/* ---- Registration ---- */

static struct obs_service_info recast_service_info = {
	.id = RECAST_SERVICE_ID,
	.get_name = recast_service_get_name,
	.create = recast_service_create,
	.destroy = recast_service_destroy,
	.update = recast_service_update,
	.get_properties = recast_service_properties,
	.get_defaults = recast_service_defaults,
	.get_url = recast_service_get_url,
	.get_key = recast_service_get_key,
};

void recast_service_register(void)
{
	obs_register_service(&recast_service_info);
}
