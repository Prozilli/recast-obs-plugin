#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	RECAST_PROTO_RTMP,
	RECAST_PROTO_RTMPS,
	RECAST_PROTO_SRT,
	RECAST_PROTO_RIST,
	RECAST_PROTO_WHIP,
	RECAST_PROTO_FTL,
	RECAST_PROTO_UNKNOWN
} recast_protocol_t;

/* Detect protocol from a URL string */
recast_protocol_t recast_protocol_detect(const char *url);

/* Get the OBS output type ID for a given protocol */
const char *recast_protocol_output_id(recast_protocol_t proto);

/* Get the OBS service type ID for a given protocol */
const char *recast_protocol_service_id(recast_protocol_t proto);

/* Get human-readable protocol name */
const char *recast_protocol_name(recast_protocol_t proto);

#ifdef __cplusplus
}
#endif
