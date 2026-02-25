/*
 * recast-output.c -- Output registration placeholder.
 *
 * The actual destination management is now in recast-multistream.cpp.
 * This file only provides the recast_output_register() call for
 * plugin-main.c compatibility.
 */

#include "recast-output.h"

void recast_output_register(void)
{
	/* No custom obs_output_info needed -- we use built-in outputs.
	 * This function is a placeholder for future custom output
	 * registration if protocol-level control is ever needed. */
}
