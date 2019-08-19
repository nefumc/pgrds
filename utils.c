/* PostgreSQL Extension WhiteList -- Dimitri Fontaine
 *
 * Author: Dimitri Fontaine <dimitri@2ndQuadrant.fr>
 * Licence: PostgreSQL
 * Copyright Dimitri Fontaine, 2011-2013
 *
 * For a description of the features see the README.md file from the same
 * distribution.
 */

/*
 * Some tools to read a SQL file and execute commands found in there.
 *
 * The following code comes from the PostgreSQL source tree in
 * src/backend/commands/extension.c, with some modifications to run in the
 * context of the Extension Whitelisting Extension.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "postgres.h"

#include "pgrds.h"
#include "utils.h"

#if PG_MAJOR_VERSION >= 903
#include "access/htup_details.h"
#else
#include "access/htup.h"
#endif

#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

/*
 * Parse contents of primary or auxiliary control file, and fill in
 * fields of *control.	We parse primary file if version == NULL,
 * else the optional auxiliary file for that version.
 *
 * Control files are supposed to be very short, half a dozen lines,
 * so we don't worry about memory allocation risks here.  Also we don't
 * worry about what encoding it's in; all values are expected to be ASCII.
 */
static void
parse_default_version_in_control_file(const char *extname,
									  char **version,
									  char **schema)
{
	char		sharepath[MAXPGPATH];
	char	   *filename;
	FILE	   *file;
	ConfigVariable *item,
		*head = NULL,
		*tail = NULL;

	/*
	 * Locate the file to read.
	 */
	get_share_path(my_exec_path, sharepath);
	filename = (char *) palloc(MAXPGPATH);
	snprintf(filename, MAXPGPATH, "%s/extension/%s.control", sharepath, extname);

	if ((file = AllocateFile(filename, "r")) == NULL)
	{
        /* we still need to handle the following error here */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));
	}

	/*
	 * Parse the file content, using GUC's file parsing code.  We need not
	 * check the return value since any errors will be thrown at ERROR level.
	 */
	(void) ParseConfigFp(file, filename, 0, ERROR, &head, &tail);

	FreeFile(file);

	/*
	 * Convert the ConfigVariable list into ExtensionControlFile entries, we
	 * are only interested into the default version.
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (*version == NULL && strcmp(item->name, "default_version") == 0)
		{
			*version = pstrdup(item->value);
		}
		else if (*schema == NULL && strcmp(item->name, "schema") == 0)
		{
			*schema = pstrdup(item->value);
		}
	}

	FreeConfigVariables(head);

	pfree(filename);
}

/*
 * At CREATE EXTENSION UPDATE time, we generally aren't provided with the
 * current version of the extension to upgrade, go fetch it from the catalogs.
 */
char *
get_extension_current_version(const char *extname)
{
	char	   *oldVersionName;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		datum;
	bool		isnull;

    /*
     * Look up the extension --- it must already exist in pg_extension
     */
	extRel = heap_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 SnapshotSelf, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist", extname)));

	/*
	 * Determine the existing version we are updating from
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	systable_endscan(extScan);

	heap_close(extRel, AccessShareLock);

	return oldVersionName;
}

/*
 * Read the statement's option list and set given parameters.
 */
void
fill_in_extension_properties(const char *extname,
							 List *options,
							 char **schema,
							 char **old_version,
							 char **new_version)
{
	ListCell   *lc;
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_old_version = NULL;

	/*
	 * Read the statement option list, taking care not to issue any errors here
	 * ourselves if at all possible: let the core code handle them.
	 */
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
		{
			d_schema = defel;
		}
		else if (strcmp(defel->defname, "new_version") == 0)
		{
			d_new_version = defel;
		}
		else if (strcmp(defel->defname, "old_version") == 0)
		{
			d_old_version = defel;
		}
		else
		{
			/* intentionnaly don't try and catch errors here */
		}
	}

	if (d_schema && d_schema->arg)
		*schema = strVal(d_schema->arg);

	if (d_old_version && d_old_version->arg)
		*old_version = strVal(d_old_version->arg);

	if (d_new_version && d_new_version->arg)
		*new_version = strVal(d_new_version->arg);

	if (*new_version == NULL || *schema == NULL)
		/* fetch the default_version from the extension's control file */
		parse_default_version_in_control_file(extname, new_version, schema);

	/* schema might be given neither in the statement nor the control file */
	if (*schema == NULL)
	{
		/*
		 * Use the current default creation namespace, which is the first
		 * explicit entry in the search_path.
		 */
		Oid         schemaOid;
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL)	/* nothing valid in search_path? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		*schema = get_namespace_name(schemaOid);
		if (*schema == NULL) /* recently-deleted namespace? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}
}


