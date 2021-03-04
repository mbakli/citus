#ifndef __POSTGIS_H__
#define __POSTGIS_H__

#define ACCEPT_USE_OF_DEPRECATED_PROJ_API_H 1

#include <liblwgeom.h>

/* PostGIS functions called by Citus  */

extern Datum intersects(PG_FUNCTION_ARGS); /* For 2D */

#endif