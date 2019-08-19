/* PostgreSQL Extension WhiteList -- Dimitri Fontaine
 *
 * Author: Dimitri Fontaine <dimitri@2ndQuadrant.fr>
 * Licence: PostgreSQL
 * Copyright Dimitri Fontaine, 2011-2013
 *
 * For a description of the features see the README.md file from the same
 * distribution.
 */

#ifndef __UTILS_H__
#define __UTILS_H__

#include "utils/builtins.h"
#include "nodes/pg_list.h"

#define MAXPGPATH 1024

extern char *extwlist_extensions;

char *get_extension_current_version(const char *extname);
void fill_in_extension_properties(const char *extname,
								  List *options,
								  char **schema,
								  char **old_version,
								  char **new_version);

#endif
