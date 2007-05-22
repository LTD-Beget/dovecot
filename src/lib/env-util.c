/* Copyright (c) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "env-util.h"

#include <stdlib.h>

static pool_t pool = NULL;

void env_put(const char *env)
{
	if (pool == NULL)
		pool = pool_alloconly_create("Environment", 2048);

	if (putenv(p_strdup(pool, env)) != 0)
		i_fatal("putenv(%s) failed: %m", env);
}

void env_clean(void)
{
	extern char **environ;

	if (environ != NULL)
		*environ = NULL;

	if (pool != NULL) {
		pool_unref(pool);
		pool = NULL;
	}
}
