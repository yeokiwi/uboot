// SPDX-License-Identifier: GPL-2.0+
/*
 * Boot-time hook for the web storage loader.
 *
 * Copyright (C) 2026 U-Boot contributors
 *
 * The loader's steps live in the environment - CIRCLE_WEB_ENV_SETTINGS in
 * include/configs/rpi.h - so that any one of them can be replaced from the
 * prompt without rebuilding.  What must not live only there is the decision
 * to run them at all.
 *
 * "saveenv" writes the *whole* environment to uboot.env, and that file
 * outlives every reflash of u-boot.bin.  An environment saved before the web
 * loader existed has no web_* variables and a circle_netpreboot that never
 * calls web_preboot, so the loader silently never runs however new the binary
 * is; a saved web_enable=0 has the same effect.  Both look identical from the
 * console - nothing is printed at all - and both are indistinguishable from
 * the loader itself being broken.
 *
 * So the hook is compiled in rather than saved.  It runs on EVT_POST_PREBOOT,
 * which fires after the preboot command and therefore after anything preboot
 * brought up, and before the boot delay.  Before running the sequence it puts
 * back any of the network and loader settings the environment is missing -
 * only the missing ones, so a customised web_server or web_file survives
 * untouched - and says so when it does.
 *
 * Between this and web_preboot's own messages, "nothing was printed" is no
 * longer one of the outcomes.
 */

#include <config.h>
#include <command.h>
#include <env.h>
#include <event.h>
#include <linux/string.h>
#include <vsprintf.h>

/* Longest variable name in the blocks below; they are all short */
#define WEB_NAME_MAX	32

/*
 * The same string literal the default environment is built from, so there is
 * one definition of what these settings are, not two.  It is a run of
 * "name=value" strings separated by NULs, and it already contains
 * CIRCLE_WEB_ENV_SETTINGS.  The trailing "" keeps this valid in a
 * configuration where the block expands to nothing.
 */
static const char web_env_defaults[] = CIRCLE_NET_ENV_SETTINGS "";

/**
 * rpi_web_env_restore() - Put back settings the environment has lost
 *
 * Walks the built-in defaults and sets any variable that is not already
 * present.  Variables that are present are left exactly as they are, whatever
 * their value, so this repairs an environment without overriding anyone.
 *
 * Return: number of variables restored
 */
static int rpi_web_env_restore(void)
{
	char name[WEB_NAME_MAX];
	const char *p, *eq;
	size_t len;
	int count = 0;

	for (p = web_env_defaults; *p; p += strlen(p) + 1) {
		eq = strchr(p, '=');
		if (!eq)
			continue;

		len = eq - p;
		if (len >= sizeof(name))
			continue;
		memcpy(name, p, len);
		name[len] = '\0';

		/*
		 * An empty default means the variable is meant to be absent -
		 * env_set() with "" deletes rather than sets - so there is
		 * nothing to put back and nothing to report.
		 */
		if (!eq[1] || env_get(name))
			continue;

		if (!env_set(name, eq + 1))
			count++;
	}

	return count;
}

static int rpi_web_preboot(void)
{
	int restored = rpi_web_env_restore();

	if (restored) {
		printf("Web loader: %d setting(s) were missing from the saved environment\n",
		       restored);
		printf("Web loader: using the built-in defaults; \"env default -a; saveenv\" makes that permanent\n");
	}

	/*
	 * The sequence's own return value is not this hook's to report: it
	 * says whether a server was found, and not finding one is the normal
	 * case.  A non-zero return from an EVT_POST_PREBOOT spy fails the
	 * boot, which is never what a missing server should do.
	 */
	run_command("run web_preboot", 0);

	return 0;
}
EVENT_SPY_SIMPLE(EVT_POST_PREBOOT, rpi_web_preboot);
