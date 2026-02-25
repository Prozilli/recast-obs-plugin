/*
 * recast-protocol.c -- URL-based protocol detection for Recast outputs.
 *
 * Detects streaming protocol from URL prefix and maps to the appropriate
 * OBS output and service type IDs.
 */

#include "recast-protocol.h"

#include <string.h>
#include <ctype.h>

static bool starts_with_ci(const char *str, const char *prefix)
{
	if (!str || !prefix)
		return false;
	while (*prefix) {
		if (tolower((unsigned char)*str) !=
		    tolower((unsigned char)*prefix))
			return false;
		str++;
		prefix++;
	}
	return true;
}

recast_protocol_t recast_protocol_detect(const char *url)
{
	if (!url || !*url)
		return RECAST_PROTO_RTMP;

	if (starts_with_ci(url, "rtmps://"))
		return RECAST_PROTO_RTMPS;
	if (starts_with_ci(url, "rtmp://"))
		return RECAST_PROTO_RTMP;
	if (starts_with_ci(url, "srt://"))
		return RECAST_PROTO_SRT;
	if (starts_with_ci(url, "rist://"))
		return RECAST_PROTO_RIST;
	if (starts_with_ci(url, "https://") || starts_with_ci(url, "http://"))
		return RECAST_PROTO_WHIP;

	return RECAST_PROTO_UNKNOWN;
}

const char *recast_protocol_output_id(recast_protocol_t proto)
{
	switch (proto) {
	case RECAST_PROTO_RTMP:
	case RECAST_PROTO_RTMPS:
		return "rtmp_output";
	case RECAST_PROTO_SRT:
	case RECAST_PROTO_RIST:
		return "ffmpeg_mpegts_muxer";
	case RECAST_PROTO_WHIP:
		return "whip_output";
	case RECAST_PROTO_UNKNOWN:
	default:
		return "rtmp_output";
	}
}

const char *recast_protocol_service_id(recast_protocol_t proto)
{
	switch (proto) {
	case RECAST_PROTO_WHIP:
		return "whip_custom";
	case RECAST_PROTO_RTMP:
	case RECAST_PROTO_RTMPS:
	case RECAST_PROTO_SRT:
	case RECAST_PROTO_RIST:
	case RECAST_PROTO_UNKNOWN:
	default:
		return "rtmp_custom";
	}
}

const char *recast_protocol_name(recast_protocol_t proto)
{
	switch (proto) {
	case RECAST_PROTO_RTMP:  return "RTMP";
	case RECAST_PROTO_RTMPS: return "RTMPS";
	case RECAST_PROTO_SRT:   return "SRT";
	case RECAST_PROTO_RIST:  return "RIST";
	case RECAST_PROTO_WHIP:  return "WHIP";
	case RECAST_PROTO_UNKNOWN:
	default:
		return "Unknown";
	}
}
