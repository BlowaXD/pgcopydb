/*
 * src/bin/pgcopydb/pgcmd.c
 *   API for running PostgreSQL commands such as pg_dump and pg_restore.
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "defaults.h"
#include "env_utils.h"
#include "file_utils.h"
#include "log.h"
#include "parsing.h"
#include "pgcmd.h"
#include "signals.h"
#include "string_utils.h"

#define RUN_PROGRAM_IMPLEMENTATION
#include "runprogram.h"

static void log_program_output(Program *prog,
							   int outLogLevel, int errorLogLevel);

/*
 * Get psql --version output in pgPaths->pg_version.
 */
bool
psql_version(PostgresPaths *pgPaths)
{
	Program prog = run_program(pgPaths->psql, "--version", NULL);
	char pg_version_string[PG_VERSION_STRING_MAX] = { 0 };
	int pg_version = 0;

	if (prog.returnCode != 0)
	{
		errno = prog.error;
		log_error("Failed to run \"psql --version\" using program \"%s\": %m",
				  pgPaths->psql);
		free_program(&prog);
		return false;
	}

	if (!parse_version_number(prog.stdOut,
							  pg_version_string,
							  PG_VERSION_STRING_MAX,
							  &pg_version))
	{
		/* errors have already been logged */
		free_program(&prog);
		return false;
	}
	free_program(&prog);

	strlcpy(pgPaths->pg_version, pg_version_string, PG_VERSION_STRING_MAX);

	return true;
}


/*
 * find_pg_commands finds the Postgres commands to use given either PG_CONFIG
 * in the environment, or finding the first psql entry in the PATH and taking
 * it from there.
 */
void
find_pg_commands(PostgresPaths *pgPaths)
{
	/* first, use PG_CONFIG when it exists in the environment */
	if (set_psql_from_PG_CONFIG(pgPaths))
	{
		(void) set_postgres_commands(pgPaths);
		return;
	}

	/* then, use PATH and fetch the first entry there for the monitor */
	if (search_path_first("psql", pgPaths->psql, LOG_WARN))
	{
		if (!psql_version(pgPaths))
		{
			/* errors have been logged in psql_version */
			exit(EXIT_CODE_PGCTL);
		}

		(void) set_postgres_commands(pgPaths);
		return;
	}

	/* then, use PATH and fetch pg_config --bindir from there */
	if (set_psql_from_pg_config(pgPaths))
	{
		(void) set_postgres_commands(pgPaths);
		return;
	}

	/* at this point we don't have any other ways to find a psql */
	exit(EXIT_CODE_PGCTL);
}


/*
 * set_postgres_commands sets the rest of the Postgres commands that pgcyopdb
 * needs from knowing the pgPaths->psql absolute location already.
 */
void
set_postgres_commands(PostgresPaths *pgPaths)
{
	path_in_same_directory(pgPaths->psql, "pg_dump", pgPaths->pg_dump);
	path_in_same_directory(pgPaths->psql, "pg_restore", pgPaths->pg_restore);
}


/*
 * set_psql_from_PG_CONFIG sets the path to psql following the exported
 * environment variable PG_CONFIG, when it is found in the environment.
 *
 * Postgres developer environments often define PG_CONFIG in the environment to
 * build extensions for a specific version of Postgres. Let's use the hint here
 * too.
 */
bool
set_psql_from_PG_CONFIG(PostgresPaths *pgPaths)
{
	char PG_CONFIG[MAXPGPATH] = { 0 };

	if (!env_exists("PG_CONFIG"))
	{
		/* then we don't use PG_CONFIG to find psql */
		return false;
	}

	if (!get_env_copy("PG_CONFIG", PG_CONFIG, sizeof(PG_CONFIG)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!file_exists(PG_CONFIG))
	{
		log_error("Failed to find a file for PG_CONFIG environment value \"%s\"",
				  PG_CONFIG);
		return false;
	}

	if (!set_psql_from_config_bindir(pgPaths, PG_CONFIG))
	{
		/* errors have already been logged */
		return false;
	}

	if (!psql_version(pgPaths))
	{
		log_fatal("Failed to get version info from %s --version",
				  pgPaths->psql);
		return false;
	}

	log_debug("Found psql for PostgreSQL %s at %s following PG_CONFIG",
			  pgPaths->pg_version, pgPaths->psql);

	return true;
}


/*
 * set_psql_from_PG_CONFIG sets given pgPaths->psql to the psql binary
 * installed in the bindir of the target Postgres installation:
 *
 *  $(${PG_CONFIG} --bindir)/psql
 */
bool
set_psql_from_config_bindir(PostgresPaths *pgPaths, const char *pg_config)
{
	char psql[MAXPGPATH] = { 0 };

	if (!file_exists(pg_config))
	{
		log_debug("set_psql_from_config_bindir: file not found: \"%s\"",
				  pg_config);
		return false;
	}

	Program prog = run_program(pg_config, "--bindir", NULL);

	char *lines[1];

	if (prog.returnCode != 0)
	{
		errno = prog.error;
		log_error("Failed to run \"pg_config --bindir\" using program \"%s\": %m",
				  pg_config);
		free_program(&prog);
		return false;
	}

	if (splitLines(prog.stdOut, lines, 1) != 1)
	{
		log_error("Unable to parse output from pg_config --bindir");
		free_program(&prog);
		return false;
	}

	char *bindir = lines[0];
	join_path_components(psql, bindir, "psql");

	/* we're now done with the Program and its output */
	free_program(&prog);

	if (!file_exists(psql))
	{
		log_error("Failed to find psql at \"%s\" from PG_CONFIG at \"%s\"",
				  pgPaths->psql,
				  pg_config);
		return false;
	}

	strlcpy(pgPaths->psql, psql, sizeof(pgPaths->psql));

	return true;
}


/*
 * set_psql_from_pg_config sets the path to psql by using pg_config
 * --bindir when there is a single pg_config found in the PATH.
 *
 * When using debian/ubuntu packaging then pg_config is installed as part as
 * the postgresql-common package in /usr/bin, whereas psql is installed in a
 * major version dependent location such as /usr/lib/postgresql/12/bin, and
 * those locations are not included in the PATH.
 *
 * So when we can't find psql anywhere in the PATH, we look for pg_config
 * instead, and then use pg_config --bindir to discover the psql we can use.
 */
bool
set_psql_from_pg_config(PostgresPaths *pgPaths)
{
	SearchPath all_pg_configs = { 0 };
	SearchPath pg_configs = { 0 };

	if (!search_path("pg_config", &all_pg_configs))
	{
		return false;
	}

	if (!search_path_deduplicate_symlinks(&all_pg_configs, &pg_configs))
	{
		log_error("Failed to resolve symlinks found in PATH entries, "
				  "see above for details");
		return false;
	}

	switch (pg_configs.found)
	{
		case 0:
		{
			log_warn("Failed to find either psql or pg_config in PATH");
			return false;
		}

		case 1:
		{
			if (!set_psql_from_config_bindir(pgPaths, pg_configs.matches[0]))
			{
				/* errors have already been logged */
				return false;
			}

			if (!psql_version(pgPaths))
			{
				log_fatal("Failed to get version info from %s --version",
						  pgPaths->psql);
				return false;
			}

			log_debug("Found psql for PostgreSQL %s at %s from pg_config "
					  "found in PATH at \"%s\"",
					  pgPaths->pg_version,
					  pgPaths->psql,
					  pg_configs.matches[0]);

			return true;
		}

		default:
		{
			log_info("Found more than one pg_config entry in current PATH:");

			for (int i = 0; i < pg_configs.found; i++)
			{
				PostgresPaths currentPgPaths = { 0 };

				strlcpy(currentPgPaths.psql,
						pg_configs.matches[i],
						sizeof(currentPgPaths.psql));

				if (!psql_version(&currentPgPaths))
				{
					/*
					 * Because of this it's possible that there's now only a
					 * single working version of psql found in PATH. If
					 * that's the case we will still not use that by default,
					 * since the users intention is unclear. They might have
					 * wanted to use the version of psql that we could not
					 * parse the version string for. So we warn and continue,
					 * the user should make their intention clear by using the
					 * --psql option (or changing PATH).
					 */
					log_warn("Failed to get version info from %s --version",
							 currentPgPaths.psql);
					continue;
				}

				log_info("Found \"%s\" for pg version %s",
						 currentPgPaths.psql,
						 currentPgPaths.pg_version);
			}

			log_info("HINT: export PG_CONFIG to a specific pg_config entry");

			return false;
		}
	}

	return false;
}


/*
 * Call pg_dump and get the given section of the dump into the target file.
 */
bool
pg_dump_db(PostgresPaths *pgPaths,
		   const char *pguri,
		   const char *section,
		   const char *filename)
{
	char *args[16];
	int argsIndex = 0;

	char command[BUFSIZE] = { 0 };

	setenv("PGCONNECT_TIMEOUT", POSTGRES_CONNECT_TIMEOUT, 1);

	args[argsIndex++] = (char *) pgPaths->pg_dump;
	args[argsIndex++] = "-Fc";
	args[argsIndex++] = "-d";
	args[argsIndex++] = (char *) pguri;
	args[argsIndex++] = "--section";
	args[argsIndex++] = (char *) section;
	args[argsIndex++] = "--file";
	args[argsIndex++] = (char *) filename;

	args[argsIndex] = NULL;

	/*
	 * We do not want to call setsid() when running pg_dump.
	 */
	Program program = { 0 };

	(void) initialize_program(&program, args, false);
	program.processBuffer = &processBufferCallback;

	/* log the exact command line we're using */
	int commandSize = snprintf_program_command_line(&program, command, BUFSIZE);

	if (commandSize >= BUFSIZE)
	{
		/* we only display the first BUFSIZE bytes of the real command */
		log_info("%s...", command);
	}
	else
	{
		log_info("%s", command);
	}

	(void) execute_subprogram(&program);

	if (program.returnCode != 0)
	{
		log_error("Failed to run pg_dump: exit code %d", program.returnCode);
		(void) log_program_output(&program, LOG_ERROR, LOG_ERROR);

		free_program(&program);

		return false;
	}

	free_program(&program);
	return true;
}


/*
 * log_program_output logs the output of the given program.
 */
static void
log_program_output(Program *prog, int outLogLevel, int errorLogLevel)
{
	if (prog->stdOut != NULL)
	{
		char *outLines[BUFSIZE];
		int lineCount = splitLines(prog->stdOut, outLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_level(outLogLevel, "%s", outLines[lineNumber]);
		}
	}

	if (prog->stdErr != NULL)
	{
		char *errorLines[BUFSIZE];
		int lineCount = splitLines(prog->stdErr, errorLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_level(errorLogLevel, "%s", errorLines[lineNumber]);
		}
	}
}