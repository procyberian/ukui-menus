/*
 * Copyright (C) 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2017,Tianjin KYLIN Information Technology Co., Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <config.h>

#include "ukuimenu-tree.h"

#include <string.h>
#include <errno.h>
#include <gio/gio.h>

#include "menu-layout.h"
#include "menu-monitor.h"
#include "menu-util.h"
#include "canonicalize.h"

/*
 * FIXME: it might be useful to be able to construct a menu
 * tree from a traditional directory based menu hierarchy
 * too.
 */

typedef enum
{
  UKUIMENU_TREE_ABSOLUTE = 0,
  UKUIMENU_TREE_BASENAME = 1
} UkuiMenuTreeType;

struct UkuiMenuTree
{
  UkuiMenuTreeType type;
  guint         refcount;

  char *basename;
  char *absolute_path;
  char *canonical_path;

  UkuiMenuTreeFlags flags;
  UkuiMenuTreeSortKey sort_key;

  GSList *menu_file_monitors;

  MenuLayoutNode *layout;
  UkuiMenuTreeDirectory *root;

  GSList *monitors;

  gpointer       user_data;
  GDestroyNotify dnotify;

  guint canonical : 1;
};

typedef struct
{
  UkuiMenuTreeChangedFunc callback;
  gpointer             user_data;
} UkuiMenuTreeMonitor;

struct UkuiMenuTreeItem
{
  UkuiMenuTreeItemType type;

  UkuiMenuTreeDirectory *parent;

  gpointer       user_data;
  GDestroyNotify dnotify;

  guint refcount;
};

struct UkuiMenuTreeDirectory
{
	UkuiMenuTreeItem item;

	DesktopEntry *directory_entry;
	char         *name;

	GSList *entries;
	GSList *subdirs;

	MenuLayoutValues  default_layout_values;
	GSList           *default_layout_info;
	GSList           *layout_info;
	GSList           *contents;

	guint only_unallocated : 1;
	guint is_root : 1;
	guint is_nodisplay : 1;
	guint layout_pending_separator : 1;
	guint preprocessed : 1;

	/* 16 bits should be more than enough; G_MAXUINT16 means no inline header */
	guint will_inline_header : 16;
};

typedef struct
{
  UkuiMenuTreeDirectory directory;

  UkuiMenuTree *tree;
} UkuiMenuTreeDirectoryRoot;

struct UkuiMenuTreeEntry
{
  UkuiMenuTreeItem item;

  DesktopEntry *desktop_entry;
  char         *desktop_file_id;

  guint is_excluded : 1;
  guint is_nodisplay : 1;
};

struct UkuiMenuTreeSeparator
{
  UkuiMenuTreeItem item;
};

struct UkuiMenuTreeHeader
{
  UkuiMenuTreeItem item;

  UkuiMenuTreeDirectory *directory;
};

struct UkuiMenuTreeAlias
{
  UkuiMenuTreeItem item;

  UkuiMenuTreeDirectory *directory;
  UkuiMenuTreeItem      *aliased_item;
};

static UkuiMenuTree *ukuimenu_tree_new                 (UkuiMenuTreeType    type,
						  const char      *menu_file,
						  gboolean         canonical,
						  UkuiMenuTreeFlags   flags);
static void      ukuimenu_tree_load_layout          (UkuiMenuTree       *tree);
static void      ukuimenu_tree_force_reload         (UkuiMenuTree       *tree);
static void      ukuimenu_tree_build_from_layout    (UkuiMenuTree       *tree);
static void      ukuimenu_tree_force_rebuild        (UkuiMenuTree       *tree);
static void      ukuimenu_tree_resolve_files        (UkuiMenuTree       *tree,
						  GHashTable      *loaded_menu_files,
						  MenuLayoutNode  *layout);
static void      ukuimenu_tree_force_recanonicalize (UkuiMenuTree       *tree);
static void      ukuimenu_tree_invoke_monitors      (UkuiMenuTree       *tree);

static void ukuimenu_tree_item_unref_and_unset_parent (gpointer itemp);

/*
 * The idea is that we cache the menu tree for either a given
 * menu basename or an absolute menu path.
 * If no files exist in $XDG_DATA_DIRS for the basename or the
 * absolute path doesn't exist we just return (and cache) the
 * empty menu tree.
 * We also add a file monitor for the basename in each dir in
 * $XDG_DATA_DIRS, or the absolute path to the menu file, and
 * re-compute if there are any changes.
 */

static GHashTable *ukuimenu_tree_cache = NULL;

static inline char *
get_cache_key (UkuiMenuTree      *tree,
	       UkuiMenuTreeFlags  flags)
{
  const char *tree_name;

  switch (tree->type)
    {
    case UKUIMENU_TREE_ABSOLUTE:
      tree_name = tree->canonical ? tree->canonical_path : tree->absolute_path;
      break;

    case UKUIMENU_TREE_BASENAME:
      tree_name = tree->basename;
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  return g_strdup_printf ("%s:0x%x", tree_name, flags);
}

static void
ukuimenu_tree_add_to_cache (UkuiMenuTree      *tree,
			 UkuiMenuTreeFlags  flags)
{
  char *cache_key;

  if (ukuimenu_tree_cache == NULL)
    {
      ukuimenu_tree_cache =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }

  cache_key = get_cache_key (tree, flags);

  menu_verbose ("Adding menu tree to cache: %s\n", cache_key);

  g_hash_table_replace (ukuimenu_tree_cache, cache_key, tree);
}

static void
ukuimenu_tree_remove_from_cache (UkuiMenuTree      *tree,
			      UkuiMenuTreeFlags  flags)
{
  char *cache_key;

  cache_key = get_cache_key (tree, flags);

  menu_verbose ("Removing menu tree from cache: %s\n", cache_key);

  g_hash_table_remove (ukuimenu_tree_cache, cache_key);

  g_free (cache_key);

  if (g_hash_table_size (ukuimenu_tree_cache) == 0)
    {
      g_hash_table_destroy (ukuimenu_tree_cache);
      ukuimenu_tree_cache = NULL;

      _entry_directory_list_empty_desktop_cache ();
    }
}

static UkuiMenuTree *
ukuimenu_tree_lookup_from_cache (const char    *tree_name,
			      UkuiMenuTreeFlags  flags)
{
  UkuiMenuTree *retval;
  char     *cache_key;

  if (ukuimenu_tree_cache == NULL)
    return NULL;

  cache_key = g_strdup_printf ("%s:0x%x", tree_name, flags);

  menu_verbose ("Looking up '%s' from menu cache\n", cache_key);

  retval = g_hash_table_lookup (ukuimenu_tree_cache, cache_key);

  g_free (cache_key);

  return retval ? ukuimenu_tree_ref (retval) : NULL;
}

typedef enum
{
  MENU_FILE_MONITOR_INVALID = 0,
  MENU_FILE_MONITOR_FILE,
  MENU_FILE_MONITOR_NONEXISTENT_FILE,
  MENU_FILE_MONITOR_DIRECTORY
} MenuFileMonitorType;

typedef struct
{
  MenuFileMonitorType  type;
  MenuMonitor         *monitor;
} MenuFileMonitor;

static void
handle_nonexistent_menu_file_changed (MenuMonitor      *monitor,
				      MenuMonitorEvent  event,
				      const char       *path,
				      UkuiMenuTree        *tree)
{
  if (event == MENU_MONITOR_EVENT_CHANGED ||
      event == MENU_MONITOR_EVENT_CREATED)
    {
      menu_verbose ("\"%s\" %s, marking tree for recanonicalization\n",
                    path,
                    event == MENU_MONITOR_EVENT_CREATED ? "created" : "changed");

      ukuimenu_tree_force_recanonicalize (tree);
      ukuimenu_tree_invoke_monitors (tree);
    }
}

static void
handle_menu_file_changed (MenuMonitor      *monitor,
			  MenuMonitorEvent  event,
			  const char       *path,
                          UkuiMenuTree        *tree)
{
  menu_verbose ("\"%s\" %s, marking tree for recanicalization\n",
		path,
		event == MENU_MONITOR_EVENT_CREATED ? "created" :
		event == MENU_MONITOR_EVENT_CHANGED ? "changed" : "deleted");

  ukuimenu_tree_force_recanonicalize (tree);
  ukuimenu_tree_invoke_monitors (tree);
}

static void
handle_menu_file_directory_changed (MenuMonitor      *monitor,
				    MenuMonitorEvent  event,
				    const char       *path,
				    UkuiMenuTree        *tree)
{
  if (!g_str_has_suffix (path, ".menu"))
    return;

  menu_verbose ("\"%s\" %s, marking tree for recanicalization\n",
		path,
		event == MENU_MONITOR_EVENT_CREATED ? "created" :
		event == MENU_MONITOR_EVENT_CHANGED ? "changed" : "deleted");

  ukuimenu_tree_force_recanonicalize (tree);
  ukuimenu_tree_invoke_monitors (tree);
}

static void
ukuimenu_tree_add_menu_file_monitor (UkuiMenuTree           *tree,
				  const char          *path,
				  MenuFileMonitorType  type)
{
  MenuFileMonitor *monitor;

  monitor = g_new0 (MenuFileMonitor, 1);

  monitor->type = type;

  switch (type)
    {
    case MENU_FILE_MONITOR_FILE:
      menu_verbose ("Adding a menu file monitor for \"%s\"\n", path);

      monitor->monitor = menu_get_file_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_menu_file_changed,
			       tree);
      break;

    case MENU_FILE_MONITOR_NONEXISTENT_FILE:
      menu_verbose ("Adding a menu file monitor for non-existent \"%s\"\n", path);

      monitor->monitor = menu_get_file_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_nonexistent_menu_file_changed,
			       tree);
      break;

    case MENU_FILE_MONITOR_DIRECTORY:
      menu_verbose ("Adding a menu directory monitor for \"%s\"\n", path);

      monitor->monitor = menu_get_directory_monitor (path);
      menu_monitor_add_notify (monitor->monitor,
			       (MenuMonitorNotifyFunc) handle_menu_file_directory_changed,
			       tree);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  tree->menu_file_monitors = g_slist_prepend (tree->menu_file_monitors, monitor);
}

static void
remove_menu_file_monitor (MenuFileMonitor *monitor,
			  UkuiMenuTree       *tree)
{
  switch (monitor->type)
    {
    case MENU_FILE_MONITOR_FILE:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_menu_file_changed,
				  tree);
      break;

    case MENU_FILE_MONITOR_NONEXISTENT_FILE:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_nonexistent_menu_file_changed,
				  tree);
      break;

    case MENU_FILE_MONITOR_DIRECTORY:
      menu_monitor_remove_notify (monitor->monitor,
				  (MenuMonitorNotifyFunc) handle_menu_file_directory_changed,
				  tree);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  menu_monitor_unref (monitor->monitor);
  monitor->monitor = NULL;

  monitor->type = MENU_FILE_MONITOR_INVALID;

  g_free (monitor);
}

static void
ukuimenu_tree_remove_menu_file_monitors (UkuiMenuTree *tree)
{
  menu_verbose ("Removing all menu file monitors\n");

  g_slist_foreach (tree->menu_file_monitors,
                   (GFunc) remove_menu_file_monitor,
                   tree);
  g_slist_free (tree->menu_file_monitors);
  tree->menu_file_monitors = NULL;
}

static UkuiMenuTree *
ukuimenu_tree_lookup_absolute (const char    *absolute,
			    UkuiMenuTreeFlags  flags)
{
  UkuiMenuTree  *tree;
  gboolean    canonical;
  const char *canonical_path;
  char       *freeme;

  menu_verbose ("Looking up absolute path in tree cache: \"%s\"\n", absolute);

  if ((tree = ukuimenu_tree_lookup_from_cache (absolute, flags)) != NULL)
    return tree;

  canonical = TRUE;
  canonical_path = freeme = menu_canonicalize_file_name (absolute, FALSE);
  if (canonical_path == NULL)
    {
      menu_verbose ("Failed to canonicalize absolute menu path \"%s\": %s\n",
                    absolute, g_strerror (errno));
      canonical = FALSE;
      canonical_path = absolute;
    }

  if ((tree = ukuimenu_tree_lookup_from_cache (canonical_path, flags)) != NULL)
    return tree;

  tree = ukuimenu_tree_new (UKUIMENU_TREE_ABSOLUTE, canonical_path, canonical, flags);

  g_free (freeme);

  return tree;
}

static UkuiMenuTree *
ukuimenu_tree_lookup_basename (const char    *basename,
			    UkuiMenuTreeFlags  flags)
{
  UkuiMenuTree *tree;

  menu_verbose ("Looking up menu file in tree cache: \"%s\"\n", basename);

  if ((tree = ukuimenu_tree_lookup_from_cache (basename, flags)) != NULL)
    return tree;

  return ukuimenu_tree_new (UKUIMENU_TREE_BASENAME, basename, FALSE, flags);
}

static gboolean
canonicalize_basename_with_config_dir (UkuiMenuTree   *tree,
                                       const char *basename,
                                       const char *config_dir)
{
  char *path;
  GSettings *settings = g_settings_new("org.ukui.ukui-menu");
  if (!settings)
	  return FALSE;
  gboolean state = g_settings_get_boolean(settings, "show-category-menu");
  g_object_unref (settings);

  if (!state && strcmp(config_dir, g_get_user_config_dir()) == 0)
	  return FALSE;

  path = g_build_filename (config_dir, "menus",  basename,  NULL);

  tree->canonical_path = menu_canonicalize_file_name (path, FALSE);
  if (tree->canonical_path)
    {
      tree->canonical = TRUE;
      ukuimenu_tree_add_menu_file_monitor (tree,
					tree->canonical_path,
					MENU_FILE_MONITOR_FILE);
    }
  else
    {
      ukuimenu_tree_add_menu_file_monitor (tree,
					path,
					MENU_FILE_MONITOR_NONEXISTENT_FILE);
    }

  g_free (path);

  return tree->canonical;
}

static void
canonicalize_basename (UkuiMenuTree  *tree,
                       const char *basename)
{
  if (!canonicalize_basename_with_config_dir (tree,
                                              basename,
                                              g_get_user_config_dir ()))
    {
      const char * const *system_config_dirs;
      int                 i;

      system_config_dirs = g_get_system_config_dirs ();

      i = 0;
      while (system_config_dirs[i] != NULL)
        {
          if (canonicalize_basename_with_config_dir (tree,
                                                     basename,
                                                     system_config_dirs[i]))
            break;

          ++i;
        }
    }
}

static gboolean ukuimenu_tree_canonicalize_path(UkuiMenuTree* tree)
{
	if (tree->canonical)
		return TRUE;

	g_assert(tree->canonical_path == NULL);

	if (tree->type == UKUIMENU_TREE_BASENAME)
	{
		ukuimenu_tree_remove_menu_file_monitors (tree);

		if (strcmp(tree->basename, "ukui-applications.menu") == 0 && g_getenv("XDG_MENU_PREFIX"))
		{
			char* prefixed_basename;
			prefixed_basename = g_strdup_printf("%s%s", g_getenv("XDG_MENU_PREFIX"), tree->basename);
			canonicalize_basename(tree, prefixed_basename);
			g_free(prefixed_basename);
		}

		if (!tree->canonical)
			canonicalize_basename(tree, tree->basename);

		if (tree->canonical)
			menu_verbose("Successfully looked up menu_file for \"%s\": %s\n", tree->basename, tree->canonical_path);
		else
			menu_verbose("Failed to look up menu_file for \"%s\"\n", tree->basename);
	}
	else /* if (tree->type == UKUIMENU_TREE_ABSOLUTE) */
	{
		tree->canonical_path = menu_canonicalize_file_name(tree->absolute_path, FALSE);

		if (tree->canonical_path != NULL)
		{
			menu_verbose("Successfully looked up menu_file for \"%s\": %s\n", tree->absolute_path, tree->canonical_path);

			/*
			* Replace the cache entry with the canonicalized version
			*/
			ukuimenu_tree_remove_from_cache (tree, tree->flags);

			ukuimenu_tree_remove_menu_file_monitors(tree);
			ukuimenu_tree_add_menu_file_monitor(tree, tree->canonical_path, MENU_FILE_MONITOR_FILE);

			tree->canonical = TRUE;

			ukuimenu_tree_add_to_cache (tree, tree->flags);
		}
		else
		{
			menu_verbose("Failed to look up menu_file for \"%s\"\n", tree->absolute_path);
		}
	}

	return tree->canonical;
}

static void
ukuimenu_tree_force_recanonicalize (UkuiMenuTree *tree)
{
  ukuimenu_tree_remove_menu_file_monitors (tree);

  if (tree->canonical)
    {
      ukuimenu_tree_force_reload (tree);

      g_free (tree->canonical_path);
      tree->canonical_path = NULL;

      tree->canonical = FALSE;
    }
}

UkuiMenuTree* ukuimenu_tree_lookup(const char* menu_file, UkuiMenuTreeFlags flags)
{
  UkuiMenuTree *retval;

  g_return_val_if_fail (menu_file != NULL, NULL);

  flags &= UKUIMENU_TREE_FLAGS_MASK;

  if (g_path_is_absolute (menu_file))
    retval = ukuimenu_tree_lookup_absolute (menu_file, flags);
  else
    retval = ukuimenu_tree_lookup_basename (menu_file, flags);

  g_assert (retval != NULL);

  return retval;
}

static UkuiMenuTree *
ukuimenu_tree_new (UkuiMenuTreeType   type,
		const char     *menu_file,
		gboolean        canonical,
		UkuiMenuTreeFlags  flags)
{
  UkuiMenuTree *tree;

  tree = g_new0 (UkuiMenuTree, 1);

  tree->type     = type;
  tree->flags    = flags;
  tree->refcount = 1;

  tree->sort_key = UKUIMENU_TREE_SORT_NAME;

  if (tree->type == UKUIMENU_TREE_BASENAME)
    {
      g_assert (canonical == FALSE);
      tree->basename = g_strdup (menu_file);
    }
  else
    {
      tree->canonical     = canonical != FALSE;
      tree->absolute_path = g_strdup (menu_file);

      if (tree->canonical)
	{
	  tree->canonical_path = g_strdup (menu_file);
	  ukuimenu_tree_add_menu_file_monitor (tree,
					    tree->canonical_path,
					    MENU_FILE_MONITOR_FILE);
	}
      else
	{
	  ukuimenu_tree_add_menu_file_monitor (tree,
					    tree->absolute_path,
					    MENU_FILE_MONITOR_NONEXISTENT_FILE);
	}
    }

  ukuimenu_tree_add_to_cache (tree, tree->flags);

  return tree;
}

UkuiMenuTree *
ukuimenu_tree_ref (UkuiMenuTree *tree)
{
  g_return_val_if_fail (tree != NULL, NULL);
  g_return_val_if_fail (tree->refcount > 0, NULL);

  tree->refcount++;

  return tree;
}

void
ukuimenu_tree_unref (UkuiMenuTree *tree)
{
  g_return_if_fail (tree != NULL);
  g_return_if_fail (tree->refcount >= 1);

  if (--tree->refcount > 0)
    return;

  if (tree->dnotify)
    tree->dnotify (tree->user_data);
  tree->user_data = NULL;
  tree->dnotify   = NULL;

  ukuimenu_tree_remove_from_cache (tree, tree->flags);

  ukuimenu_tree_force_recanonicalize (tree);

  if (tree->basename != NULL)
    g_free (tree->basename);
  tree->basename = NULL;

  if (tree->absolute_path != NULL)
    g_free (tree->absolute_path);
  tree->absolute_path = NULL;

  g_slist_foreach (tree->monitors, (GFunc) g_free, NULL);
  g_slist_free (tree->monitors);
  tree->monitors = NULL;

  g_free (tree);
}

void
ukuimenu_tree_set_user_data (UkuiMenuTree       *tree,
			  gpointer        user_data,
			  GDestroyNotify  dnotify)
{
  g_return_if_fail (tree != NULL);

  if (tree->dnotify != NULL)
    tree->dnotify (tree->user_data);

  tree->dnotify   = dnotify;
  tree->user_data = user_data;
}

gpointer
ukuimenu_tree_get_user_data (UkuiMenuTree *tree)
{
  g_return_val_if_fail (tree != NULL, NULL);

  return tree->user_data;
}

const char *
ukuimenu_tree_get_menu_file (UkuiMenuTree *tree)
{
  /* FIXME: this is horribly ugly. But it's done to keep the API. Would be bad
   * to break the API only for a "const char *" => "char *" change. The other
   * alternative is to leak the memory, which is bad too. */
  static char *ugly_result_cache = NULL;

  g_return_val_if_fail (tree != NULL, NULL);

  /* we need to canonicalize the path so we actually find out the real menu
   * file that is being used -- and take into account XDG_MENU_PREFIX */
  if (!ukuimenu_tree_canonicalize_path (tree))
    return NULL;

  if (ugly_result_cache != NULL)
    {
      g_free (ugly_result_cache);
      ugly_result_cache = NULL;
    }

  if (tree->type == UKUIMENU_TREE_BASENAME)
    {
      ugly_result_cache = g_path_get_basename (tree->canonical_path);
      return ugly_result_cache;
    }
  else
    return tree->absolute_path;
}

UkuiMenuTreeDirectory *
ukuimenu_tree_get_root_directory (UkuiMenuTree *tree)
{
  g_return_val_if_fail (tree != NULL, NULL);

  if (!tree->root)
    {
      ukuimenu_tree_build_from_layout (tree);

      if (!tree->root)
        return NULL;
    }

  return ukuimenu_tree_item_ref (tree->root);
}

static UkuiMenuTreeDirectory *
find_path (UkuiMenuTreeDirectory *directory,
	   const char         *path)
{
  const char *name;
  char       *slash;
  char       *freeme;
  GSList     *tmp;

  while (path[0] == G_DIR_SEPARATOR) path++;

  if (path[0] == '\0')
    return directory;

  freeme = NULL;
  slash = strchr (path, G_DIR_SEPARATOR);
  if (slash)
    {
      name = freeme = g_strndup (path, slash - path);
      path = slash + 1;
    }
  else
    {
      name = path;
      path = NULL;
    }

  tmp = directory->contents;
  while (tmp != NULL)
    {
      UkuiMenuTreeItem *item = tmp->data;

      if (ukuimenu_tree_item_get_type (item) != UKUIMENU_TREE_ITEM_DIRECTORY)
        {
          tmp = tmp->next;
          continue;
        }

      if (!strcmp (name, UKUIMENU_TREE_DIRECTORY (item)->name))
	{
	  g_free (freeme);

	  if (path)
	    return find_path (UKUIMENU_TREE_DIRECTORY (item), path);
	  else
	    return UKUIMENU_TREE_DIRECTORY (item);
	}

      tmp = tmp->next;
    }

  g_free (freeme);

  return NULL;
}

UkuiMenuTreeDirectory *
ukuimenu_tree_get_directory_from_path (UkuiMenuTree  *tree,
				    const char *path)
{
  UkuiMenuTreeDirectory *root;
  UkuiMenuTreeDirectory *directory;

  g_return_val_if_fail (tree != NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  if (path[0] != G_DIR_SEPARATOR)
    return NULL;

  if (!(root = ukuimenu_tree_get_root_directory (tree)))
    return NULL;

  directory = find_path (root, path);

  ukuimenu_tree_item_unref (root);

  return directory ? ukuimenu_tree_item_ref (directory) : NULL;
}

UkuiMenuTreeSortKey
ukuimenu_tree_get_sort_key (UkuiMenuTree *tree)
{
  g_return_val_if_fail (tree != NULL, UKUIMENU_TREE_SORT_NAME);
  g_return_val_if_fail (tree->refcount > 0, UKUIMENU_TREE_SORT_NAME);

  return tree->sort_key;
}

void
ukuimenu_tree_set_sort_key (UkuiMenuTree        *tree,
			 UkuiMenuTreeSortKey  sort_key)
{
  g_return_if_fail (tree != NULL);
  g_return_if_fail (tree->refcount > 0);
  g_return_if_fail (sort_key >= UKUIMENU_TREE_SORT_FIRST);
  g_return_if_fail (sort_key <= UKUIMENU_TREE_SORT_LAST);

  if (sort_key == tree->sort_key)
    return;

  tree->sort_key = sort_key;
  ukuimenu_tree_force_rebuild (tree);
}

void
ukuimenu_tree_add_monitor (UkuiMenuTree            *tree,
                       UkuiMenuTreeChangedFunc   callback,
                       gpointer               user_data)
{
  UkuiMenuTreeMonitor *monitor;
  GSList           *tmp;

  g_return_if_fail (tree != NULL);
  g_return_if_fail (callback != NULL);

  tmp = tree->monitors;
  while (tmp != NULL)
    {
      monitor = tmp->data;

      if (monitor->callback  == callback &&
          monitor->user_data == user_data)
        break;

      tmp = tmp->next;
    }

  if (tmp == NULL)
    {
      monitor = g_new0 (UkuiMenuTreeMonitor, 1);

      monitor->callback  = callback;
      monitor->user_data = user_data;

      tree->monitors = g_slist_append (tree->monitors, monitor);
    }
}

void
ukuimenu_tree_remove_monitor (UkuiMenuTree            *tree,
			   UkuiMenuTreeChangedFunc  callback,
			   gpointer              user_data)
{
  GSList *tmp;

  g_return_if_fail (tree != NULL);
  g_return_if_fail (callback != NULL);

  tmp = tree->monitors;
  while (tmp != NULL)
    {
      UkuiMenuTreeMonitor *monitor = tmp->data;
      GSList          *next = tmp->next;

      if (monitor->callback  == callback &&
          monitor->user_data == user_data)
        {
          tree->monitors = g_slist_delete_link (tree->monitors, tmp);
          g_free (monitor);
        }

      tmp = next;
    }
}

static void
ukuimenu_tree_invoke_monitors (UkuiMenuTree *tree)
{
  GSList *tmp;

  tmp = tree->monitors;
  while (tmp != NULL)
    {
      UkuiMenuTreeMonitor *monitor = tmp->data;
      GSList           *next    = tmp->next;

      monitor->callback (tree, monitor->user_data);

      tmp = next;
    }
}

UkuiMenuTreeItemType
ukuimenu_tree_item_get_type (UkuiMenuTreeItem *item)
{
  g_return_val_if_fail (item != NULL, 0);

  return item->type;
}

UkuiMenuTreeDirectory *
ukuimenu_tree_item_get_parent (UkuiMenuTreeItem *item)
{
  g_return_val_if_fail (item != NULL, NULL);

  return item->parent ? ukuimenu_tree_item_ref (item->parent) : NULL;
}

static void
ukuimenu_tree_item_set_parent (UkuiMenuTreeItem      *item,
			    UkuiMenuTreeDirectory *parent)
{
  g_return_if_fail (item != NULL);

  item->parent = parent;
}

GSList *
ukuimenu_tree_directory_get_contents (UkuiMenuTreeDirectory *directory)
{
  GSList *retval;
  GSList *tmp;

  g_return_val_if_fail (directory != NULL, NULL);

  retval = NULL;

  tmp = directory->contents;
  while (tmp != NULL)
    {
      retval = g_slist_prepend (retval,
                                ukuimenu_tree_item_ref (tmp->data));

      tmp = tmp->next;
    }

  return g_slist_reverse (retval);
}

const char *
ukuimenu_tree_directory_get_name (UkuiMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return directory->name;

  return desktop_entry_get_name (directory->directory_entry);
}

const char *
ukuimenu_tree_directory_get_comment (UkuiMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return NULL;

  return desktop_entry_get_comment (directory->directory_entry);
}

const char* ukuimenu_tree_directory_get_icon(UkuiMenuTreeDirectory* directory)
{
	g_return_val_if_fail(directory != NULL, NULL);

	if (!directory->directory_entry)
		return NULL;

	return desktop_entry_get_icon(directory->directory_entry);
}

const char *
ukuimenu_tree_directory_get_desktop_file_path (UkuiMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  if (!directory->directory_entry)
    return NULL;

  return desktop_entry_get_path (directory->directory_entry);
}

const char *
ukuimenu_tree_directory_get_menu_id (UkuiMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, NULL);

  return directory->name;
}

static void
ukuimenu_tree_directory_set_tree (UkuiMenuTreeDirectory *directory,
			       UkuiMenuTree          *tree)
{
  UkuiMenuTreeDirectoryRoot *root;

  g_assert (directory != NULL);
  g_assert (directory->is_root);

  root = (UkuiMenuTreeDirectoryRoot *) directory;

  root->tree = tree;
}

UkuiMenuTree *
ukuimenu_tree_directory_get_tree (UkuiMenuTreeDirectory *directory)
{
  UkuiMenuTreeDirectoryRoot *root;

  g_return_val_if_fail (directory != NULL, NULL);

  while (UKUIMENU_TREE_ITEM (directory)->parent != NULL)
    directory = UKUIMENU_TREE_DIRECTORY (UKUIMENU_TREE_ITEM (directory)->parent);

  if (!directory->is_root)
    return NULL;

  root = (UkuiMenuTreeDirectoryRoot *) directory;

  if (root->tree)
    ukuimenu_tree_ref (root->tree);

  return root->tree;
}

gboolean
ukuimenu_tree_directory_get_is_nodisplay (UkuiMenuTreeDirectory *directory)
{
  g_return_val_if_fail (directory != NULL, FALSE);

  return directory->is_nodisplay;
}

static void
append_directory_path (UkuiMenuTreeDirectory *directory,
		       GString            *path)
{

  if (!directory->item.parent)
    {
      g_string_append_c (path, G_DIR_SEPARATOR);
      return;
    }

  append_directory_path (directory->item.parent, path);

  g_string_append (path, directory->name);
  g_string_append_c (path, G_DIR_SEPARATOR);
}

char *
ukuimenu_tree_directory_make_path (UkuiMenuTreeDirectory *directory,
				UkuiMenuTreeEntry     *entry)
{
  GString *path;

  g_return_val_if_fail (directory != NULL, NULL);

  path = g_string_new (NULL);

  append_directory_path (directory, path);

  if (entry != NULL)
    g_string_append (path,
		     desktop_entry_get_basename (entry->desktop_entry));

  return g_string_free (path, FALSE);
}

const char *
ukuimenu_tree_entry_get_name (UkuiMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return desktop_entry_get_name (entry->desktop_entry);
}

const char *
ukuimenu_tree_entry_get_generic_name (UkuiMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return desktop_entry_get_generic_name (entry->desktop_entry);
}

const char *
ukuimenu_tree_entry_get_display_name (UkuiMenuTreeEntry *entry)
{
  const char *display_name;

  g_return_val_if_fail (entry != NULL, NULL);

  display_name = desktop_entry_get_full_name (entry->desktop_entry);
  if (!display_name || display_name[0] == '\0')
    display_name = desktop_entry_get_name (entry->desktop_entry);

  return display_name;
}

const char *
ukuimenu_tree_entry_get_comment (UkuiMenuTreeEntry *entry)
{
  g_return_val_if_fail (entry != NULL, NULL);

  return desktop_entry_get_comment (entry->desktop_entry);
}

const char* ukuimenu_tree_entry_get_icon(UkuiMenuTreeEntry *entry)
{
	g_return_val_if_fail (entry != NULL, NULL);

	return desktop_entry_get_icon(entry->desktop_entry);
}

const char* ukuimenu_tree_entry_get_exec(UkuiMenuTreeEntry* entry)
{
	g_return_val_if_fail(entry != NULL, NULL);

	return desktop_entry_get_exec(entry->desktop_entry);
}

gboolean ukuimenu_tree_entry_get_launch_in_terminal(UkuiMenuTreeEntry* entry)
{
  g_return_val_if_fail(entry != NULL, FALSE);

  return desktop_entry_get_launch_in_terminal(entry->desktop_entry);
}

const char* ukuimenu_tree_entry_get_desktop_file_path(UkuiMenuTreeEntry* entry)
{
	g_return_val_if_fail(entry != NULL, NULL);

	return desktop_entry_get_path(entry->desktop_entry);
}

const char* ukuimenu_tree_entry_get_desktop_file_id(UkuiMenuTreeEntry* entry)
{
	g_return_val_if_fail(entry != NULL, NULL);

	return entry->desktop_file_id;
}

gboolean ukuimenu_tree_entry_get_is_excluded(UkuiMenuTreeEntry* entry)
{
	g_return_val_if_fail(entry != NULL, FALSE);

	return entry->is_excluded;
}

gboolean ukuimenu_tree_entry_get_is_nodisplay(UkuiMenuTreeEntry* entry)
{
	g_return_val_if_fail(entry != NULL, FALSE);

	return entry->is_nodisplay;
}

UkuiMenuTreeDirectory* ukuimenu_tree_header_get_directory(UkuiMenuTreeHeader* header)
{
	g_return_val_if_fail (header != NULL, NULL);

	return ukuimenu_tree_item_ref(header->directory);
}

UkuiMenuTreeDirectory* ukuimenu_tree_alias_get_directory(UkuiMenuTreeAlias* alias)
{
	g_return_val_if_fail (alias != NULL, NULL);

	return ukuimenu_tree_item_ref(alias->directory);
}

UkuiMenuTreeItem *
ukuimenu_tree_alias_get_item (UkuiMenuTreeAlias *alias)
{
  g_return_val_if_fail (alias != NULL, NULL);

  return ukuimenu_tree_item_ref (alias->aliased_item);
}

static UkuiMenuTreeDirectory *
ukuimenu_tree_directory_new (UkuiMenuTreeDirectory *parent,
			  const char         *name,
			  gboolean            is_root)
{
  UkuiMenuTreeDirectory *retval;

  if (!is_root)
    {
      retval = g_new0 (UkuiMenuTreeDirectory, 1);
    }
  else
    {
      UkuiMenuTreeDirectoryRoot *root;

      root = g_new0 (UkuiMenuTreeDirectoryRoot, 1);

      retval = UKUIMENU_TREE_DIRECTORY (root);

      retval->is_root = TRUE;
    }


  retval->item.type     = UKUIMENU_TREE_ITEM_DIRECTORY;
  retval->item.parent   = parent;
  retval->item.refcount = 1;

  retval->name                = g_strdup (name);
  retval->directory_entry     = NULL;
  retval->entries             = NULL;
  retval->subdirs             = NULL;
  retval->default_layout_info = NULL;
  retval->layout_info         = NULL;
  retval->contents            = NULL;
  retval->only_unallocated    = FALSE;
  retval->is_nodisplay        = FALSE;
  retval->layout_pending_separator = FALSE;
  retval->preprocessed        = FALSE;
  retval->will_inline_header  = G_MAXUINT16;

  retval->default_layout_values.mask          = MENU_LAYOUT_VALUES_NONE;
  retval->default_layout_values.show_empty    = FALSE;
  retval->default_layout_values.inline_menus  = FALSE;
  retval->default_layout_values.inline_limit  = 4;
  retval->default_layout_values.inline_header = FALSE;
  retval->default_layout_values.inline_alias  = FALSE;

  return retval;
}

static void
ukuimenu_tree_directory_finalize (UkuiMenuTreeDirectory *directory)
{
  g_assert (directory->item.refcount == 0);

  g_slist_foreach (directory->contents,
		   (GFunc) ukuimenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->contents);
  directory->contents = NULL;

  g_slist_foreach (directory->default_layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->default_layout_info);
  directory->default_layout_info = NULL;

  g_slist_foreach (directory->layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->layout_info);
  directory->layout_info = NULL;

  g_slist_foreach (directory->subdirs,
		   (GFunc) ukuimenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->subdirs);
  directory->subdirs = NULL;

  g_slist_foreach (directory->entries,
		   (GFunc) ukuimenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->entries);
  directory->entries = NULL;

  if (directory->directory_entry)
    desktop_entry_unref (directory->directory_entry);
  directory->directory_entry = NULL;

  g_free (directory->name);
  directory->name = NULL;
}

static UkuiMenuTreeSeparator *
ukuimenu_tree_separator_new (UkuiMenuTreeDirectory *parent)
{
  UkuiMenuTreeSeparator *retval;

  retval = g_new0 (UkuiMenuTreeSeparator, 1);

  retval->item.type     = UKUIMENU_TREE_ITEM_SEPARATOR;
  retval->item.parent   = parent;
  retval->item.refcount = 1;

  return retval;
}

static UkuiMenuTreeHeader *
ukuimenu_tree_header_new (UkuiMenuTreeDirectory *parent,
		       UkuiMenuTreeDirectory *directory)
{
  UkuiMenuTreeHeader *retval;

  retval = g_new0 (UkuiMenuTreeHeader, 1);

  retval->item.type     = UKUIMENU_TREE_ITEM_HEADER;
  retval->item.parent   = parent;
  retval->item.refcount = 1;

  retval->directory = ukuimenu_tree_item_ref (directory);

  ukuimenu_tree_item_set_parent (UKUIMENU_TREE_ITEM (retval->directory), NULL);

  return retval;
}

static void
ukuimenu_tree_header_finalize (UkuiMenuTreeHeader *header)
{
  g_assert (header->item.refcount == 0);

  if (header->directory != NULL)
    ukuimenu_tree_item_unref (header->directory);
  header->directory = NULL;
}

static UkuiMenuTreeAlias *
ukuimenu_tree_alias_new (UkuiMenuTreeDirectory *parent,
		      UkuiMenuTreeDirectory *directory,
		      UkuiMenuTreeItem      *item)
{
  UkuiMenuTreeAlias *retval;

  retval = g_new0 (UkuiMenuTreeAlias, 1);

  retval->item.type     = UKUIMENU_TREE_ITEM_ALIAS;
  retval->item.parent   = parent;
  retval->item.refcount = 1;

  retval->directory    = ukuimenu_tree_item_ref (directory);
  if (item->type != UKUIMENU_TREE_ITEM_ALIAS)
    retval->aliased_item = ukuimenu_tree_item_ref (item);
  else
    retval->aliased_item = ukuimenu_tree_item_ref (ukuimenu_tree_alias_get_item (UKUIMENU_TREE_ALIAS (item)));

  ukuimenu_tree_item_set_parent (UKUIMENU_TREE_ITEM (retval->directory), NULL);
  ukuimenu_tree_item_set_parent (retval->aliased_item, NULL);

  return retval;
}

static void
ukuimenu_tree_alias_finalize (UkuiMenuTreeAlias *alias)
{
  g_assert (alias->item.refcount == 0);

  if (alias->directory != NULL)
    ukuimenu_tree_item_unref (alias->directory);
  alias->directory = NULL;

  if (alias->aliased_item != NULL)
    ukuimenu_tree_item_unref (alias->aliased_item);
  alias->aliased_item = NULL;
}

static UkuiMenuTreeEntry *
ukuimenu_tree_entry_new (UkuiMenuTreeDirectory *parent,
		      DesktopEntry       *desktop_entry,
		      const char         *desktop_file_id,
		      gboolean            is_excluded,
                      gboolean            is_nodisplay)
{
  UkuiMenuTreeEntry *retval;

  retval = g_new0 (UkuiMenuTreeEntry, 1);

  retval->item.type     = UKUIMENU_TREE_ITEM_ENTRY;
  retval->item.parent   = parent;
  retval->item.refcount = 1;

  retval->desktop_entry   = desktop_entry_ref (desktop_entry);
  retval->desktop_file_id = g_strdup (desktop_file_id);
  retval->is_excluded     = is_excluded != FALSE;
  retval->is_nodisplay    = is_nodisplay != FALSE;

  return retval;
}

static void
ukuimenu_tree_entry_finalize (UkuiMenuTreeEntry *entry)
{
  g_assert (entry->item.refcount == 0);

  g_free (entry->desktop_file_id);
  entry->desktop_file_id = NULL;

  if (entry->desktop_entry)
    desktop_entry_unref (entry->desktop_entry);
  entry->desktop_entry = NULL;
}

static int
ukuimenu_tree_entry_compare_by_id (UkuiMenuTreeItem *a,
				UkuiMenuTreeItem *b)
{
  if (a->type == UKUIMENU_TREE_ITEM_ALIAS)
    a = UKUIMENU_TREE_ALIAS (a)->aliased_item;

  if (b->type == UKUIMENU_TREE_ITEM_ALIAS)
    b = UKUIMENU_TREE_ALIAS (b)->aliased_item;

  return strcmp (UKUIMENU_TREE_ENTRY (a)->desktop_file_id,
                 UKUIMENU_TREE_ENTRY (b)->desktop_file_id);
}

gpointer ukuimenu_tree_item_ref(gpointer itemp)
{
	UkuiMenuTreeItem* item = (UkuiMenuTreeItem*) itemp;

	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(item->refcount > 0, NULL);

	item->refcount++;

	return item;
}

void
ukuimenu_tree_item_unref (gpointer itemp)
{
  UkuiMenuTreeItem *item;

  item = (UkuiMenuTreeItem *) itemp;

  g_return_if_fail (item != NULL);
  g_return_if_fail (item->refcount > 0);

  if (--item->refcount == 0)
    {
      switch (item->type)
	{
	case UKUIMENU_TREE_ITEM_DIRECTORY:
	  ukuimenu_tree_directory_finalize (UKUIMENU_TREE_DIRECTORY (item));
	  break;

	case UKUIMENU_TREE_ITEM_ENTRY:
	  ukuimenu_tree_entry_finalize (UKUIMENU_TREE_ENTRY (item));
	  break;

	case UKUIMENU_TREE_ITEM_SEPARATOR:
	  break;

	case UKUIMENU_TREE_ITEM_HEADER:
	  ukuimenu_tree_header_finalize (UKUIMENU_TREE_HEADER (item));
	  break;

	case UKUIMENU_TREE_ITEM_ALIAS:
	  ukuimenu_tree_alias_finalize (UKUIMENU_TREE_ALIAS (item));
	  break;

	default:
	  g_assert_not_reached ();
	  break;
	}

      if (item->dnotify)
	item->dnotify (item->user_data);
      item->user_data = NULL;
      item->dnotify   = NULL;

      item->parent = NULL;

      g_free (item);
    }
}

static void
ukuimenu_tree_item_unref_and_unset_parent (gpointer itemp)
{
  UkuiMenuTreeItem *item;

  item = (UkuiMenuTreeItem *) itemp;

  g_return_if_fail (item != NULL);

  ukuimenu_tree_item_set_parent (item, NULL);
  ukuimenu_tree_item_unref (item);
}

void
ukuimenu_tree_item_set_user_data (UkuiMenuTreeItem  *item,
			       gpointer        user_data,
			       GDestroyNotify  dnotify)
{
  g_return_if_fail (item != NULL);

  if (item->dnotify != NULL)
    item->dnotify (item->user_data);

  item->dnotify   = dnotify;
  item->user_data = user_data;
}

gpointer
ukuimenu_tree_item_get_user_data (UkuiMenuTreeItem *item)
{
  g_return_val_if_fail (item != NULL, NULL);

  return item->user_data;
}

static inline const char *
ukuimenu_tree_item_compare_get_name_helper (UkuiMenuTreeItem    *item,
					 UkuiMenuTreeSortKey  sort_key)
{
  const char *name;

  name = NULL;

  switch (item->type)
    {
    case UKUIMENU_TREE_ITEM_DIRECTORY:
      if (UKUIMENU_TREE_DIRECTORY (item)->directory_entry)
	name = desktop_entry_get_name (UKUIMENU_TREE_DIRECTORY (item)->directory_entry);
      else
	name = UKUIMENU_TREE_DIRECTORY (item)->name;
      break;

    case UKUIMENU_TREE_ITEM_ENTRY:
      switch (sort_key)
	{
	case UKUIMENU_TREE_SORT_NAME:
	  name = desktop_entry_get_name (UKUIMENU_TREE_ENTRY (item)->desktop_entry);
	  break;
	case UKUIMENU_TREE_SORT_DISPLAY_NAME:
	  name = ukuimenu_tree_entry_get_display_name (UKUIMENU_TREE_ENTRY (item));
	  break;
	default:
	  g_assert_not_reached ();
	  break;
	}
      break;

    case UKUIMENU_TREE_ITEM_ALIAS:
      {
        UkuiMenuTreeItem *dir;
        dir = UKUIMENU_TREE_ITEM (UKUIMENU_TREE_ALIAS (item)->directory);
        name = ukuimenu_tree_item_compare_get_name_helper (dir, sort_key);
      }
      break;

    case UKUIMENU_TREE_ITEM_SEPARATOR:
    case UKUIMENU_TREE_ITEM_HEADER:
    default:
      g_assert_not_reached ();
      break;
    }

  return name;
}

static int
ukuimenu_tree_item_compare (UkuiMenuTreeItem *a,
			 UkuiMenuTreeItem *b,
			 gpointer       sort_key_p)
{
  const char       *name_a;
  const char       *name_b;
  UkuiMenuTreeSortKey  sort_key;

  sort_key = GPOINTER_TO_INT (sort_key_p);

  name_a = ukuimenu_tree_item_compare_get_name_helper (a, sort_key);
  name_b = ukuimenu_tree_item_compare_get_name_helper (b, sort_key);

  return g_utf8_collate (name_a, name_b);
}

static MenuLayoutNode *
find_menu_child (MenuLayoutNode *layout)
{
  MenuLayoutNode *child;

  child = menu_layout_node_get_children (layout);
  while (child && menu_layout_node_get_type (child) != MENU_LAYOUT_NODE_MENU)
    child = menu_layout_node_get_next (child);

  return child;
}

static void
merge_resolved_children (UkuiMenuTree      *tree,
			 GHashTable     *loaded_menu_files,
                         MenuLayoutNode *where,
                         MenuLayoutNode *from)
{
  MenuLayoutNode *insert_after;
  MenuLayoutNode *menu_child;
  MenuLayoutNode *from_child;

  ukuimenu_tree_resolve_files (tree, loaded_menu_files, from);

  insert_after = where;
  g_assert (menu_layout_node_get_type (insert_after) != MENU_LAYOUT_NODE_ROOT);
  g_assert (menu_layout_node_get_parent (insert_after) != NULL);

  /* skip root node */
  menu_child = find_menu_child (from);
  g_assert (menu_child != NULL);
  g_assert (menu_layout_node_get_type (menu_child) == MENU_LAYOUT_NODE_MENU);

  /* merge children of toplevel <Menu> */
  from_child = menu_layout_node_get_children (menu_child);
  while (from_child != NULL)
    {
      MenuLayoutNode *next;

      next = menu_layout_node_get_next (from_child);

      menu_verbose ("Merging ");
      menu_debug_print_layout (from_child, FALSE);
      menu_verbose (" after ");
      menu_debug_print_layout (insert_after, FALSE);

      switch (menu_layout_node_get_type (from_child))
        {
        case MENU_LAYOUT_NODE_NAME:
          menu_layout_node_unlink (from_child); /* delete this */
          break;

        default:
          menu_layout_node_steal (from_child);
          menu_layout_node_insert_after (insert_after, from_child);
          menu_layout_node_unref (from_child);

          insert_after = from_child;
          break;
        }

      from_child = next;
    }
}

static gboolean
load_merge_file (UkuiMenuTree      *tree,
		 GHashTable     *loaded_menu_files,
                 const char     *filename,
                 gboolean        is_canonical,
		 gboolean        add_monitor,
                 MenuLayoutNode *where)
{
  MenuLayoutNode *to_merge;
  const char     *canonical;
  char           *freeme;
  gboolean        retval;

  freeme = NULL;
  retval = FALSE;

  if (!is_canonical)
    {
      canonical = freeme = menu_canonicalize_file_name (filename, FALSE);
      if (canonical == NULL)
        {
	  if (add_monitor)
	    ukuimenu_tree_add_menu_file_monitor (tree,
					     filename,
					     MENU_FILE_MONITOR_NONEXISTENT_FILE);

          menu_verbose ("Failed to canonicalize merge file path \"%s\": %s\n",
                        filename, g_strerror (errno));
	  goto out;
        }
    }
  else
    {
      canonical = filename;
    }

  if (g_hash_table_lookup (loaded_menu_files, canonical) != NULL)
    {
      g_warning ("Not loading \"%s\": recursive loop detected in .menu files",
		 canonical);
      retval = TRUE;
      goto out;
    }

  menu_verbose ("Merging file \"%s\"\n", canonical);

  to_merge = menu_layout_load (canonical, NULL, NULL);
  if (to_merge == NULL)
    {
      menu_verbose ("No menu for file \"%s\" found when merging\n",
                    canonical);
      goto out;
    }

  retval = TRUE;

  g_hash_table_insert (loaded_menu_files, (char *) canonical, GUINT_TO_POINTER (TRUE));

  if (add_monitor)
    ukuimenu_tree_add_menu_file_monitor (tree,
				      canonical,
				      MENU_FILE_MONITOR_FILE);

  merge_resolved_children (tree, loaded_menu_files, where, to_merge);

  g_hash_table_remove (loaded_menu_files, canonical);

  menu_layout_node_unref (to_merge);

 out:
  if (freeme)
    g_free (freeme);

  return retval;
}

static gboolean
load_merge_file_with_config_dir (UkuiMenuTree      *tree,
				 GHashTable     *loaded_menu_files,
				 const char     *menu_file,
				 const char     *config_dir,
				 MenuLayoutNode *where)
{
  char     *merge_file;
  gboolean  loaded;

  loaded = FALSE;

  merge_file = g_build_filename (config_dir, "menus", menu_file, NULL);

  if (load_merge_file (tree, loaded_menu_files, merge_file, FALSE, TRUE, where))
    loaded = TRUE;

  g_free (merge_file);

  return loaded;
}

static gboolean
compare_basedir_to_config_dir (const char *canonical_basedir,
			       const char *config_dir)
{
  char     *dirname;
  char     *canonical_menus_dir;
  gboolean  retval;

  menu_verbose ("Checking to see if basedir '%s' is in '%s'\n",
		canonical_basedir, config_dir);

  dirname = g_build_filename (config_dir, "menus", NULL);

  retval = FALSE;

  canonical_menus_dir = menu_canonicalize_file_name (dirname, FALSE);
  if (canonical_menus_dir != NULL &&
      strcmp (canonical_basedir, canonical_menus_dir) == 0)
    {
      retval = TRUE;
    }

  g_free (canonical_menus_dir);
  g_free (dirname);

  return retval;
}

static gboolean
load_parent_merge_file_from_basename (UkuiMenuTree      *tree,
                                      GHashTable     *loaded_menu_files,
			              MenuLayoutNode *layout,
                                      const char     *menu_file,
                                      const char     *canonical_basedir)
{
  gboolean            found_basedir;
  const char * const *system_config_dirs;
  int                 i;

  /* We're not interested in menu files that are in directories which are not a
   * parent of the base directory of this menu file */
  found_basedir = compare_basedir_to_config_dir (canonical_basedir,
						 g_get_user_config_dir ());

  system_config_dirs = g_get_system_config_dirs ();

  i = 0;
  while (system_config_dirs[i] != NULL)
    {
      if (!found_basedir)
	{
	  found_basedir = compare_basedir_to_config_dir (canonical_basedir,
							 system_config_dirs[i]);
	}
      else
	{
	  menu_verbose ("Looking for parent menu file '%s' in '%s'\n",
			menu_file, system_config_dirs[i]);

	  if (load_merge_file_with_config_dir (tree,
					       loaded_menu_files,
					       menu_file,
					       system_config_dirs[i],
					       layout))
	    {
	      break;
	    }
	}

      ++i;
    }

  return system_config_dirs[i] != NULL;
}

static gboolean load_parent_merge_file(UkuiMenuTree* tree, GHashTable* loaded_menu_files, MenuLayoutNode* layout)
{
	MenuLayoutNode* root;
	const char* basedir;
	const char* menu_name;
	char* canonical_basedir;
	char* menu_file;
	gboolean found;

	root = menu_layout_node_get_root(layout);

	basedir   = menu_layout_node_root_get_basedir(root);
	menu_name = menu_layout_node_root_get_name(root);

	canonical_basedir = menu_canonicalize_file_name(basedir, FALSE);

	if (canonical_basedir == NULL)
	{
		menu_verbose("Menu basedir '%s' no longer exists, not merging parent\n", basedir);
		return FALSE;
	}

	found = FALSE;
	menu_file = g_strconcat(menu_name, ".menu", NULL);

	if (strcmp(menu_file, "ukui-applications.menu") == 0 && g_getenv("XDG_MENU_PREFIX"))
	{
		char* prefixed_basename;
		prefixed_basename = g_strdup_printf("%s%s", g_getenv("XDG_MENU_PREFIX"), menu_file);
		found = load_parent_merge_file_from_basename(tree, loaded_menu_files, layout, prefixed_basename, canonical_basedir);
		g_free(prefixed_basename);
	}

	if (!found)
	{
		found = load_parent_merge_file_from_basename(tree, loaded_menu_files, layout, menu_file, canonical_basedir);
	}

	g_free(menu_file);
	g_free(canonical_basedir);

	return found;
}

static void
load_merge_dir (UkuiMenuTree      *tree,
		GHashTable     *loaded_menu_files,
                const char     *dirname,
                MenuLayoutNode *where)
{
  GDir       *dir;
  const char *menu_file;

  menu_verbose ("Loading merge dir \"%s\"\n", dirname);

  ukuimenu_tree_add_menu_file_monitor (tree,
				    dirname,
				    MENU_FILE_MONITOR_DIRECTORY);

  if ((dir = g_dir_open (dirname, 0, NULL)) == NULL)
    return;

  while ((menu_file = g_dir_read_name (dir)))
    {
      if (g_str_has_suffix (menu_file, ".menu"))
        {
          char *full_path;

          full_path = g_build_filename (dirname, menu_file, NULL);

          load_merge_file (tree, loaded_menu_files, full_path, TRUE, FALSE, where);

          g_free (full_path);
        }
    }

  g_dir_close (dir);
}

static void
load_merge_dir_with_config_dir (UkuiMenuTree      *tree,
				GHashTable     *loaded_menu_files,
                                const char     *config_dir,
                                const char     *dirname,
                                MenuLayoutNode *where)
{
  char *path;

  path = g_build_filename (config_dir, "menus", dirname, NULL);

  load_merge_dir (tree, loaded_menu_files, path, where);

  g_free (path);
}

static void
resolve_merge_file (UkuiMenuTree      *tree,
		    GHashTable     *loaded_menu_files,
                    MenuLayoutNode *layout)
{
  char *filename;

  if (menu_layout_node_merge_file_get_type (layout) == MENU_MERGE_FILE_TYPE_PARENT)
    {
      if (load_parent_merge_file (tree, loaded_menu_files, layout))
        return;
    }

  filename = menu_layout_node_get_content_as_path (layout);
  if (filename == NULL)
    {
      menu_verbose ("didn't get node content as a path, not merging file\n");
    }
  else
    {
      load_merge_file (tree, loaded_menu_files, filename, FALSE, TRUE, layout);

      g_free (filename);
    }

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
resolve_merge_dir (UkuiMenuTree      *tree,
		   GHashTable     *loaded_menu_files,
                   MenuLayoutNode *layout)
{
  char *path;

  path = menu_layout_node_get_content_as_path (layout);
  if (path == NULL)
    {
      menu_verbose ("didn't get layout node content as a path, not merging dir\n");
    }
  else
    {
      load_merge_dir (tree, loaded_menu_files, path, layout);

      g_free (path);
    }

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static MenuLayoutNode *
add_app_dir (UkuiMenuTree      *tree,
             MenuLayoutNode *before,
             const char     *data_dir)
{
  MenuLayoutNode *tmp;
  char           *dirname;

  tmp = menu_layout_node_new (MENU_LAYOUT_NODE_APP_DIR);
  dirname = g_build_filename (data_dir, "applications", NULL);
  menu_layout_node_set_content (tmp, dirname);
  menu_layout_node_insert_before (before, tmp);
  menu_layout_node_unref (before);

  menu_verbose ("Adding <AppDir>%s</AppDir> in <DefaultAppDirs/>\n",
                dirname);

  g_free (dirname);

  return tmp;
}

static void
resolve_default_app_dirs (UkuiMenuTree      *tree,
                          MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();

  before = add_app_dir (tree,
			menu_layout_node_ref (layout),
			g_get_user_data_dir ());

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
      before = add_app_dir (tree, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static MenuLayoutNode* add_directory_dir(UkuiMenuTree* tree, MenuLayoutNode* before, const char* data_dir)
{
	MenuLayoutNode* tmp;
	char* dirname;

	tmp = menu_layout_node_new(MENU_LAYOUT_NODE_DIRECTORY_DIR);
	dirname = g_build_filename(data_dir, "desktop-directories", NULL);
	menu_layout_node_set_content(tmp, dirname);
	menu_layout_node_insert_before(before, tmp);
	menu_layout_node_unref(before);

	menu_verbose("Adding <DirectoryDir>%s</DirectoryDir> in <DefaultDirectoryDirs/>\n", dirname);

	g_free(dirname);

	return tmp;
}

static void
resolve_default_directory_dirs (UkuiMenuTree      *tree,
                                MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();
  GSettings *settings = g_settings_new("org.ukui.ukui-menu");
  if (!settings)
	  return;
  gboolean state = g_settings_get_boolean(settings, "show-category-menu");
  g_object_unref (settings);

  if (state)
	before = add_directory_dir (tree,
			      	    menu_layout_node_ref (layout),
			      	    g_get_user_data_dir ());
  else
  	before = menu_layout_node_ref (layout);

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
		/* Parche para tomar las carpetas /ukui/ */
		char* path = g_build_filename(system_data_dirs[i], "ukui", NULL);
		before = add_directory_dir(tree, before, path);
		g_free(path);
		/* /fin parche */
      before = add_directory_dir (tree, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
resolve_default_merge_dirs (UkuiMenuTree      *tree,
			    GHashTable     *loaded_menu_files,
                            MenuLayoutNode *layout)
{
  MenuLayoutNode     *root;
  const char         *menu_name;
  char               *merge_name;
  const char * const *system_config_dirs;
  int                 i;

  root = menu_layout_node_get_root (layout);
  menu_name = menu_layout_node_root_get_name (root);

  merge_name = g_strconcat (menu_name, "-merged", NULL);

  system_config_dirs = g_get_system_config_dirs ();

  /* Merge in reverse order */
  i = 0;
  while (system_config_dirs[i] != NULL) i++;
  while (i > 0)
    {
      i--;
      load_merge_dir_with_config_dir (tree,
				      loaded_menu_files,
                                      system_config_dirs[i],
                                      merge_name,
                                      layout);
    }

  load_merge_dir_with_config_dir (tree,
				  loaded_menu_files,
                                  g_get_user_config_dir (),
                                  merge_name,
                                  layout);

  g_free (merge_name);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
add_filename_include (const char     *desktop_file_id,
                      DesktopEntry   *entry,
                      MenuLayoutNode *include)
{
  if (!desktop_entry_has_categories (entry))
    {
      MenuLayoutNode *node;

      node = menu_layout_node_new (MENU_LAYOUT_NODE_FILENAME);
      menu_layout_node_set_content (node, desktop_file_id);

      menu_layout_node_append_child (include, node);
      menu_layout_node_unref (node);
    }
}

static void
is_dot_directory (const char   *basename,
		  DesktopEntry *entry,
		  gboolean     *has_dot_directory)
{
  if (!strcmp (basename, ".directory"))
    *has_dot_directory = TRUE;
}

static gboolean
add_menu_for_legacy_dir (MenuLayoutNode *parent,
                         const char     *legacy_dir,
                	 const char     *relative_path,
                         const char     *legacy_prefix,
                         const char     *menu_name)
{
  EntryDirectory  *ed;
  DesktopEntrySet *desktop_entries;
  DesktopEntrySet *directory_entries;
  GSList          *subdirs;
  gboolean         menu_added;
  gboolean         has_dot_directory;

  ed = entry_directory_new_legacy (DESKTOP_ENTRY_INVALID, legacy_dir, legacy_prefix);
  if (!ed)
    return FALSE;

  subdirs = NULL;
  desktop_entries   = desktop_entry_set_new ();
  directory_entries = desktop_entry_set_new ();

  entry_directory_get_flat_contents (ed,
                                     desktop_entries,
                                     directory_entries,
                                     &subdirs);
  entry_directory_unref (ed);

  has_dot_directory = FALSE;
  desktop_entry_set_foreach (directory_entries,
			     (DesktopEntrySetForeachFunc) is_dot_directory,
			     &has_dot_directory);
  desktop_entry_set_unref (directory_entries);

  menu_added = FALSE;
  if (desktop_entry_set_get_count (desktop_entries) > 0 || subdirs)
    {
      MenuLayoutNode *menu;
      MenuLayoutNode *node;
      GString        *subdir_path;
      GString        *subdir_relative;
      GSList         *tmp;
      int             legacy_dir_len;
      int             relative_path_len;

      menu = menu_layout_node_new (MENU_LAYOUT_NODE_MENU);
      menu_layout_node_append_child (parent, menu);

      menu_added = TRUE;

      g_assert (menu_name != NULL);

      node = menu_layout_node_new (MENU_LAYOUT_NODE_NAME);
      menu_layout_node_set_content (node, menu_name);
      menu_layout_node_append_child (menu, node);
      menu_layout_node_unref (node);

      if (has_dot_directory)
	{
	  node = menu_layout_node_new (MENU_LAYOUT_NODE_DIRECTORY);
	  if (relative_path != NULL)
	    {
	      char *directory_entry_path;

	      directory_entry_path = g_strdup_printf ("%s/.directory", relative_path);
	      menu_layout_node_set_content (node, directory_entry_path);
	      g_free (directory_entry_path);
	    }
	  else
	    {
	      menu_layout_node_set_content (node, ".directory");
	    }
	  menu_layout_node_append_child (menu, node);
	  menu_layout_node_unref (node);
	}

      if (desktop_entry_set_get_count (desktop_entries) > 0)
	{
	  MenuLayoutNode *include;

	  include = menu_layout_node_new (MENU_LAYOUT_NODE_INCLUDE);
	  menu_layout_node_append_child (menu, include);

	  desktop_entry_set_foreach (desktop_entries,
				     (DesktopEntrySetForeachFunc) add_filename_include,
				     include);

	  menu_layout_node_unref (include);
	}

      subdir_path = g_string_new (legacy_dir);
      legacy_dir_len = strlen (legacy_dir);

      subdir_relative = g_string_new (relative_path);
      relative_path_len = relative_path ? strlen (relative_path) : 0;

      tmp = subdirs;
      while (tmp != NULL)
        {
          const char *subdir = tmp->data;

          g_string_append_c (subdir_path, G_DIR_SEPARATOR);
          g_string_append (subdir_path, subdir);

	  if (relative_path_len)
	    {
	      g_string_append_c (subdir_relative, G_DIR_SEPARATOR);
	    }
          g_string_append (subdir_relative, subdir);

          add_menu_for_legacy_dir (menu,
                                   subdir_path->str,
				   subdir_relative->str,
                                   legacy_prefix,
                                   subdir);

          g_string_truncate (subdir_relative, relative_path_len);
          g_string_truncate (subdir_path, legacy_dir_len);

          tmp = tmp->next;
        }

      g_string_free (subdir_path, TRUE);
      g_string_free (subdir_relative, TRUE);

      menu_layout_node_unref (menu);
    }

  desktop_entry_set_unref (desktop_entries);

  g_slist_foreach (subdirs, (GFunc) g_free, NULL);
  g_slist_free (subdirs);

  return menu_added;
}

static void
resolve_legacy_dir (UkuiMenuTree      *tree,
		    GHashTable     *loaded_menu_files,
                    MenuLayoutNode *legacy)
{
  MenuLayoutNode *to_merge;
  MenuLayoutNode *menu;

  to_merge = menu_layout_node_new (MENU_LAYOUT_NODE_ROOT);

  menu = menu_layout_node_get_parent (legacy);
  g_assert (menu_layout_node_get_type (menu) == MENU_LAYOUT_NODE_MENU);

  if (add_menu_for_legacy_dir (to_merge,
                               menu_layout_node_get_content (legacy),
			       NULL,
                               menu_layout_node_legacy_dir_get_prefix (legacy),
                               menu_layout_node_menu_get_name (menu)))
    {
      merge_resolved_children (tree, loaded_menu_files, legacy, to_merge);
    }

  menu_layout_node_unref (to_merge);
}

static MenuLayoutNode *
add_legacy_dir (UkuiMenuTree      *tree,
		GHashTable     *loaded_menu_files,
                MenuLayoutNode *before,
                const char     *data_dir)
{
  MenuLayoutNode *legacy;
  char           *dirname;

  dirname = g_build_filename (data_dir, "applnk", NULL);

  legacy = menu_layout_node_new (MENU_LAYOUT_NODE_LEGACY_DIR);
  menu_layout_node_set_content (legacy, dirname);
  menu_layout_node_legacy_dir_set_prefix (legacy, "kde");
  menu_layout_node_insert_before (before, legacy);
  menu_layout_node_unref (before);

  menu_verbose ("Adding <LegacyDir>%s</LegacyDir> in <KDELegacyDirs/>\n",
                dirname);

  resolve_legacy_dir (tree, loaded_menu_files, legacy);

  g_free (dirname);

  return legacy;
}

static void
resolve_kde_legacy_dirs (UkuiMenuTree      *tree,
			 GHashTable     *loaded_menu_files,
                         MenuLayoutNode *layout)
{
  MenuLayoutNode     *before;
  const char * const *system_data_dirs;
  int                 i;

  system_data_dirs = g_get_system_data_dirs ();

  before = add_legacy_dir (tree,
			   loaded_menu_files,
			   menu_layout_node_ref (layout),
			   g_get_user_data_dir ());

  i = 0;
  while (system_data_dirs[i] != NULL)
    {
      before = add_legacy_dir (tree, loaded_menu_files, before, system_data_dirs[i]);

      ++i;
    }

  menu_layout_node_unref (before);

  /* remove the now-replaced node */
  menu_layout_node_unlink (layout);
}

static void
ukuimenu_tree_resolve_files (UkuiMenuTree      *tree,
			  GHashTable     *loaded_menu_files,
			  MenuLayoutNode *layout)
{
  MenuLayoutNode *child;

  menu_verbose ("Resolving files in: ");
  menu_debug_print_layout (layout, TRUE);

  switch (menu_layout_node_get_type (layout))
    {
    case MENU_LAYOUT_NODE_MERGE_FILE:
      resolve_merge_file (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_MERGE_DIR:
      resolve_merge_dir (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_APP_DIRS:
      resolve_default_app_dirs (tree, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_DIRECTORY_DIRS:
      resolve_default_directory_dirs (tree, layout);
      break;

    case MENU_LAYOUT_NODE_DEFAULT_MERGE_DIRS:
      resolve_default_merge_dirs (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_LEGACY_DIR:
      resolve_legacy_dir (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_KDE_LEGACY_DIRS:
      resolve_kde_legacy_dirs (tree, loaded_menu_files, layout);
      break;

    case MENU_LAYOUT_NODE_PASSTHROUGH:
      /* Just get rid of these, we don't need the memory usage */
      menu_layout_node_unlink (layout);
      break;

    default:
      /* Recurse */
      child = menu_layout_node_get_children (layout);
      while (child != NULL)
        {
          MenuLayoutNode *next = menu_layout_node_get_next (child);

          ukuimenu_tree_resolve_files (tree, loaded_menu_files, child);

          child = next;
        }
      break;
    }
}

static void
move_children (MenuLayoutNode *from,
               MenuLayoutNode *to)
{
  MenuLayoutNode *from_child;
  MenuLayoutNode *insert_before;

  insert_before = menu_layout_node_get_children (to);
  from_child    = menu_layout_node_get_children (from);

  while (from_child != NULL)
    {
      MenuLayoutNode *next;

      next = menu_layout_node_get_next (from_child);

      menu_layout_node_steal (from_child);

      if (menu_layout_node_get_type (from_child) == MENU_LAYOUT_NODE_NAME)
        {
          ; /* just drop the Name in the old <Menu> */
        }
      else if (insert_before)
        {
          menu_layout_node_insert_before (insert_before, from_child);
          g_assert (menu_layout_node_get_next (from_child) == insert_before);
        }
      else
        {
          menu_layout_node_append_child (to, from_child);
        }

      menu_layout_node_unref (from_child);

      from_child = next;
    }
}

static int
null_safe_strcmp (const char *a,
                  const char *b)
{
  if (a == NULL && b == NULL)
    return 0;
  else if (a == NULL)
    return -1;
  else if (b == NULL)
    return 1;
  else
    return strcmp (a, b);
}

static int
node_compare_func (const void *a,
                   const void *b)
{
  MenuLayoutNode *node_a = (MenuLayoutNode*) a;
  MenuLayoutNode *node_b = (MenuLayoutNode*) b;
  MenuLayoutNodeType t_a = menu_layout_node_get_type (node_a);
  MenuLayoutNodeType t_b = menu_layout_node_get_type (node_b);

  if (t_a < t_b)
    return -1;
  else if (t_a > t_b)
    return 1;
  else
    {
      const char *c_a = menu_layout_node_get_content (node_a);
      const char *c_b = menu_layout_node_get_content (node_b);

      return null_safe_strcmp (c_a, c_b);
    }
}

static int
node_menu_compare_func (const void *a,
                        const void *b)
{
  MenuLayoutNode *node_a = (MenuLayoutNode*) a;
  MenuLayoutNode *node_b = (MenuLayoutNode*) b;
  MenuLayoutNode *parent_a = menu_layout_node_get_parent (node_a);
  MenuLayoutNode *parent_b = menu_layout_node_get_parent (node_b);

  if (parent_a < parent_b)
    return -1;
  else if (parent_a > parent_b)
    return 1;
  else
    return null_safe_strcmp (menu_layout_node_menu_get_name (node_a),
                             menu_layout_node_menu_get_name (node_b));
}

static void
ukuimenu_tree_strip_duplicate_children (UkuiMenuTree      *tree,
				     MenuLayoutNode *layout)
{
  MenuLayoutNode *child;
  GSList         *simple_nodes;
  GSList         *menu_layout_nodes;
  GSList         *prev;
  GSList         *tmp;

  /* to strip dups, we find all the child nodes where
   * we want to kill dups, sort them,
   * then nuke the adjacent nodes that are equal
   */

  simple_nodes = NULL;
  menu_layout_nodes = NULL;

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
          /* These are dups if their content is the same */
        case MENU_LAYOUT_NODE_APP_DIR:
        case MENU_LAYOUT_NODE_DIRECTORY_DIR:
        case MENU_LAYOUT_NODE_DIRECTORY:
          simple_nodes = g_slist_prepend (simple_nodes, child);
          break;

          /* These have to be merged in a more complicated way,
           * and then recursed
           */
        case MENU_LAYOUT_NODE_MENU:
          menu_layout_nodes = g_slist_prepend (menu_layout_nodes, child);
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  /* Note that the lists are all backward. So we want to keep
   * the items that are earlier in the list, because they were
   * later in the file
   */

  /* stable sort the simple nodes */
  simple_nodes = g_slist_sort (simple_nodes,
                               node_compare_func);

  prev = NULL;
  tmp = simple_nodes;
  while (tmp != NULL)
    {
      GSList *next = tmp->next;

      if (prev)
        {
          MenuLayoutNode *p = prev->data;
          MenuLayoutNode *n = tmp->data;

          if (node_compare_func (p, n) == 0)
            {
              /* nuke it! */
              menu_layout_node_unlink (n);
	      simple_nodes = g_slist_delete_link (simple_nodes, tmp);
	      tmp = prev;
            }
        }

      prev = tmp;
      tmp = next;
    }

  g_slist_free (simple_nodes);
  simple_nodes = NULL;

  /* stable sort the menu nodes (the sort includes the
   * parents of the nodes in the comparison). Remember
   * the list is backward.
   */
  menu_layout_nodes = g_slist_sort (menu_layout_nodes,
				    node_menu_compare_func);

  prev = NULL;
  tmp = menu_layout_nodes;
  while (tmp != NULL)
    {
      GSList *next = tmp->next;

      if (prev)
        {
          MenuLayoutNode *p = prev->data;
          MenuLayoutNode *n = tmp->data;

          if (node_menu_compare_func (p, n) == 0)
            {
              /* Move children of first menu to the start of second
               * menu and nuke the first menu
               */
              move_children (n, p);
              menu_layout_node_unlink (n);
	      menu_layout_nodes = g_slist_delete_link (menu_layout_nodes, tmp);
	      tmp = prev;
            }
        }

      prev = tmp;
      tmp = next;
    }

  g_slist_free (menu_layout_nodes);
  menu_layout_nodes = NULL;

  /* Recursively clean up all children */
  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      if (menu_layout_node_get_type (child) == MENU_LAYOUT_NODE_MENU)
        ukuimenu_tree_strip_duplicate_children (tree, child);

      child = menu_layout_node_get_next (child);
    }
}

static MenuLayoutNode *
find_submenu (MenuLayoutNode *layout,
              const char     *path,
              gboolean        create_if_not_found)
{
  MenuLayoutNode *child;
  const char     *slash;
  const char     *next_path;
  char           *name;

  menu_verbose (" (splitting \"%s\")\n", path);

  if (path[0] == '\0' || path[0] == G_DIR_SEPARATOR)
    return NULL;

  slash = strchr (path, G_DIR_SEPARATOR);
  if (slash != NULL)
    {
      name = g_strndup (path, slash - path);
      next_path = slash + 1;
      if (*next_path == '\0')
        next_path = NULL;
    }
  else
    {
      name = g_strdup (path);
      next_path = NULL;
    }

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
        case MENU_LAYOUT_NODE_MENU:
          {
            if (strcmp (name, menu_layout_node_menu_get_name (child)) == 0)
              {
                menu_verbose ("MenuNode %p found for path component \"%s\"\n",
                              child, name);

                g_free (name);

                if (!next_path)
                  {
                    menu_verbose (" Found menu node %p parent is %p\n",
                                  child, layout);
                    return child;
                  }

                return find_submenu (child, next_path, create_if_not_found);
              }
          }
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  if (create_if_not_found)
    {
      MenuLayoutNode *name_node;

      child = menu_layout_node_new (MENU_LAYOUT_NODE_MENU);
      menu_layout_node_append_child (layout, child);

      name_node = menu_layout_node_new (MENU_LAYOUT_NODE_NAME);
      menu_layout_node_set_content (name_node, name);
      menu_layout_node_append_child (child, name_node);
      menu_layout_node_unref (name_node);

      menu_verbose (" Created menu node %p parent is %p\n",
                    child, layout);

      menu_layout_node_unref (child);
      g_free (name);

      if (!next_path)
        return child;

      return find_submenu (child, next_path, create_if_not_found);
    }
  else
    {
      g_free (name);
      return NULL;
    }
}

/* To call this you first have to strip duplicate children once,
 * otherwise when you move a menu Foo to Bar then you may only
 * move one of Foo, not all the merged Foo.
 */
static void
ukuimenu_tree_execute_moves (UkuiMenuTree      *tree,
			  MenuLayoutNode *layout,
			  gboolean       *need_remove_dups_p)
{
  MenuLayoutNode *child;
  gboolean        need_remove_dups;
  GSList         *move_nodes;
  GSList         *tmp;

  need_remove_dups = FALSE;

  move_nodes = NULL;

  child = menu_layout_node_get_children (layout);
  while (child != NULL)
    {
      switch (menu_layout_node_get_type (child))
        {
        case MENU_LAYOUT_NODE_MENU:
          /* Recurse - we recurse first and process the current node
           * second, as the spec dictates.
           */
          ukuimenu_tree_execute_moves (tree, child, &need_remove_dups);
          break;

        case MENU_LAYOUT_NODE_MOVE:
          move_nodes = g_slist_prepend (move_nodes, child);
          break;

        default:
          break;
        }

      child = menu_layout_node_get_next (child);
    }

  /* We need to execute the move operations in the order that they appear */
  move_nodes = g_slist_reverse (move_nodes);

  tmp = move_nodes;
  while (tmp != NULL)
    {
      MenuLayoutNode *move_node = tmp->data;
      MenuLayoutNode *old_node;
      GSList         *next = tmp->next;
      const char     *old;
      const char     *new;

      old = menu_layout_node_move_get_old (move_node);
      new = menu_layout_node_move_get_new (move_node);
      g_assert (old != NULL && new != NULL);

      menu_verbose ("executing <Move> old = \"%s\" new = \"%s\"\n",
                    old, new);

      old_node = find_submenu (layout, old, FALSE);
      if (old_node != NULL)
        {
          MenuLayoutNode *new_node;

          /* here we can create duplicates anywhere below the
           * node
           */
          need_remove_dups = TRUE;

          /* look up new node creating it and its parents if
           * required
           */
          new_node = find_submenu (layout, new, TRUE);
          g_assert (new_node != NULL);

          move_children (old_node, new_node);

          menu_layout_node_unlink (old_node);
        }

      menu_layout_node_unlink (move_node);

      tmp = next;
    }

  g_slist_free (move_nodes);

  /* This oddness is to ensure we only remove dups once,
   * at the root, instead of recursing the tree over
   * and over.
   */
  if (need_remove_dups_p)
    *need_remove_dups_p = need_remove_dups;
  else if (need_remove_dups)
    ukuimenu_tree_strip_duplicate_children (tree, layout);
}

static void
ukuimenu_tree_load_layout (UkuiMenuTree *tree)
{
  GHashTable *loaded_menu_files;
  GError     *error;

  if (tree->layout)
    return;

  if (!ukuimenu_tree_canonicalize_path (tree))
    return;

  menu_verbose ("Loading menu layout from \"%s\"\n",
                tree->canonical_path);

  error = NULL;
  tree->layout = menu_layout_load (tree->canonical_path,
                                   tree->type == UKUIMENU_TREE_BASENAME ?
                                        tree->basename : NULL,
                                   &error);
  if (tree->layout == NULL)
    {
      g_warning ("Error loading menu layout from \"%s\": %s",
                 tree->canonical_path, error->message);
      g_error_free (error);
      return;
    }

  loaded_menu_files = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (loaded_menu_files, tree->canonical_path, GUINT_TO_POINTER (TRUE));
  ukuimenu_tree_resolve_files (tree, loaded_menu_files, tree->layout);
  g_hash_table_destroy (loaded_menu_files);

  ukuimenu_tree_strip_duplicate_children (tree, tree->layout);
  ukuimenu_tree_execute_moves (tree, tree->layout, NULL);
}

static void
ukuimenu_tree_force_reload (UkuiMenuTree *tree)
{
  ukuimenu_tree_force_rebuild (tree);

  if (tree->layout)
    menu_layout_node_unref (tree->layout);
  tree->layout = NULL;
}

typedef struct
{
  DesktopEntrySet *set;
  const char      *category;
} GetByCategoryForeachData;

static void
get_by_category_foreach (const char               *file_id,
			 DesktopEntry             *entry,
			 GetByCategoryForeachData *data)
{
  if (desktop_entry_has_category (entry, data->category))
    desktop_entry_set_add_entry (data->set, entry, file_id);
}

static void
get_by_category (DesktopEntrySet *entry_pool,
		 DesktopEntrySet *set,
		 const char      *category)
{
  GetByCategoryForeachData data;

  data.set      = set;
  data.category = category;

  desktop_entry_set_foreach (entry_pool,
			     (DesktopEntrySetForeachFunc) get_by_category_foreach,
			     &data);
}

static DesktopEntrySet *
process_include_rules (MenuLayoutNode  *layout,
		       DesktopEntrySet *entry_pool)
{
  DesktopEntrySet *set = NULL;

  switch (menu_layout_node_get_type (layout))
    {
    case MENU_LAYOUT_NODE_AND:
      {
        MenuLayoutNode *child;

	menu_verbose ("Processing <And>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_intersection (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            /* as soon as we get empty results, we can bail,
             * because it's an AND
             */
            if (desktop_entry_set_get_count (set) == 0)
              break;

            child = menu_layout_node_get_next (child);
          }
	menu_verbose ("Processed <And>\n");
      }
      break;

    case MENU_LAYOUT_NODE_OR:
      {
        MenuLayoutNode *child;

	menu_verbose ("Processing <Or>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_union (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            child = menu_layout_node_get_next (child);
          }
	menu_verbose ("Processed <Or>\n");
      }
      break;

    case MENU_LAYOUT_NODE_NOT:
      {
        /* First get the OR of all the rules */
        MenuLayoutNode *child;

	menu_verbose ("Processing <Not>\n");

        child = menu_layout_node_get_children (layout);
        while (child != NULL)
          {
            DesktopEntrySet *child_set;

            child_set = process_include_rules (child, entry_pool);

            if (set == NULL)
              {
                set = child_set;
              }
            else
              {
                desktop_entry_set_union (set, child_set);
                desktop_entry_set_unref (child_set);
              }

            child = menu_layout_node_get_next (child);
          }

        if (set != NULL)
          {
	    DesktopEntrySet *inverted;

	    /* Now invert the result */
	    inverted = desktop_entry_set_new ();
	    desktop_entry_set_union (inverted, entry_pool);
	    desktop_entry_set_subtract (inverted, set);
	    desktop_entry_set_unref (set);
	    set = inverted;
          }
	menu_verbose ("Processed <Not>\n");
      }
      break;

    case MENU_LAYOUT_NODE_ALL:
      menu_verbose ("Processing <All>\n");
      set = desktop_entry_set_new ();
      desktop_entry_set_union (set, entry_pool);
      menu_verbose ("Processed <All>\n");
      break;

    case MENU_LAYOUT_NODE_FILENAME:
      {
        DesktopEntry *entry;

	menu_verbose ("Processing <Filename>%s</Filename>\n",
		      menu_layout_node_get_content (layout));

        entry = desktop_entry_set_lookup (entry_pool,
					  menu_layout_node_get_content (layout));
        if (entry != NULL)
          {
            set = desktop_entry_set_new ();
            desktop_entry_set_add_entry (set,
                                         entry,
                                         menu_layout_node_get_content (layout));
          }
	menu_verbose ("Processed <Filename>%s</Filename>\n",
		      menu_layout_node_get_content (layout));
      }
      break;

    case MENU_LAYOUT_NODE_CATEGORY:
      menu_verbose ("Processing <Category>%s</Category>\n",
		    menu_layout_node_get_content (layout));
      set = desktop_entry_set_new ();
      get_by_category (entry_pool, set, menu_layout_node_get_content (layout));
      menu_verbose ("Processed <Category>%s</Category>\n",
		    menu_layout_node_get_content (layout));
      break;

    default:
      break;
    }

  if (set == NULL)
    set = desktop_entry_set_new (); /* create an empty set */

  menu_verbose ("Matched %d entries\n", desktop_entry_set_get_count (set));

  return set;
}

static void
collect_layout_info (MenuLayoutNode  *layout,
		     GSList         **layout_info)
{
  MenuLayoutNode *iter;

  g_slist_foreach (*layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (*layout_info);
  *layout_info = NULL;

  iter = menu_layout_node_get_children (layout);
  while (iter != NULL)
    {
      switch (menu_layout_node_get_type (iter))
	{
	case MENU_LAYOUT_NODE_MENUNAME:
	case MENU_LAYOUT_NODE_FILENAME:
	case MENU_LAYOUT_NODE_SEPARATOR:
	case MENU_LAYOUT_NODE_MERGE:
	  *layout_info = g_slist_prepend (*layout_info,
					  menu_layout_node_ref (iter));
	  break;

	default:
	  break;
	}

      iter = menu_layout_node_get_next (iter);
    }

  *layout_info = g_slist_reverse (*layout_info);
}

static void
entries_listify_foreach (const char         *desktop_file_id,
                         DesktopEntry       *desktop_entry,
                         UkuiMenuTreeDirectory *directory)
{
  directory->entries =
    g_slist_prepend (directory->entries,
		     ukuimenu_tree_entry_new (directory,
                                           desktop_entry,
                                           desktop_file_id,
                                           FALSE,
                                           desktop_entry_get_no_display (desktop_entry)));
}

static void
excluded_entries_listify_foreach (const char         *desktop_file_id,
				  DesktopEntry       *desktop_entry,
				  UkuiMenuTreeDirectory *directory)
{
  directory->entries =
    g_slist_prepend (directory->entries,
		     ukuimenu_tree_entry_new (directory,
					   desktop_entry,
					   desktop_file_id,
					   TRUE,
                                           desktop_entry_get_no_display (desktop_entry)));
}

static void
set_default_layout_values (UkuiMenuTreeDirectory *parent,
                           UkuiMenuTreeDirectory *child)
{
  GSList *tmp;

  /* if the child has a defined default layout, we don't want to override its
   * values. The parent might have a non-defined layout info (ie, no child of
   * the DefaultLayout node) but it doesn't meant the default layout values
   * (ie, DefaultLayout attributes) aren't different from the global defaults.
   */
  if (child->default_layout_info != NULL ||
      child->default_layout_values.mask != MENU_LAYOUT_VALUES_NONE)
    return;

  child->default_layout_values = parent->default_layout_values;

  tmp = child->subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;

      set_default_layout_values (child, subdir);

      tmp = tmp->next;
   }
}

static UkuiMenuTreeDirectory *
process_layout (UkuiMenuTree          *tree,
                UkuiMenuTreeDirectory *parent,
                MenuLayoutNode     *layout,
                DesktopEntrySet    *allocated)
{
  MenuLayoutNode     *layout_iter;
  UkuiMenuTreeDirectory *directory;
  DesktopEntrySet    *entry_pool;
  DesktopEntrySet    *entries;
  DesktopEntrySet    *allocated_set;
  DesktopEntrySet    *excluded_set;
  gboolean            deleted;
  gboolean            only_unallocated;
  GSList             *tmp;

  g_assert (menu_layout_node_get_type (layout) == MENU_LAYOUT_NODE_MENU);
  g_assert (menu_layout_node_menu_get_name (layout) != NULL);

  directory = ukuimenu_tree_directory_new (parent,
					menu_layout_node_menu_get_name (layout),
					parent == NULL);

  menu_verbose ("=== Menu name = %s ===\n", directory->name);


  deleted = FALSE;
  only_unallocated = FALSE;

  entries = desktop_entry_set_new ();
  allocated_set = desktop_entry_set_new ();

  if (tree->flags & UKUIMENU_TREE_FLAGS_INCLUDE_EXCLUDED)
    excluded_set = desktop_entry_set_new ();
  else
    excluded_set = NULL;

  entry_pool = _entry_directory_list_get_all_desktops (menu_layout_node_menu_get_app_dirs (layout));

  layout_iter = menu_layout_node_get_children (layout);
  while (layout_iter != NULL)
    {
      switch (menu_layout_node_get_type (layout_iter))
        {
        case MENU_LAYOUT_NODE_MENU:
          /* recurse */
          {
            UkuiMenuTreeDirectory *child_dir;

	    menu_verbose ("Processing <Menu>\n");

            child_dir = process_layout (tree,
                                        directory,
                                        layout_iter,
                                        allocated);
            if (child_dir)
              directory->subdirs = g_slist_prepend (directory->subdirs,
                                                    child_dir);

	    menu_verbose ("Processed <Menu>\n");
          }
          break;

        case MENU_LAYOUT_NODE_INCLUDE:
          {
            /* The match rule children of the <Include> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuLayoutNode *rule;

	    menu_verbose ("Processing <Include> (%d entries)\n",
			  desktop_entry_set_get_count (entries));

            rule = menu_layout_node_get_children (layout_iter);
            while (rule != NULL)
              {
                DesktopEntrySet *rule_set;

                rule_set = process_include_rules (rule, entry_pool);
                if (rule_set != NULL)
                  {
                    desktop_entry_set_union (entries, rule_set);
                    desktop_entry_set_union (allocated_set, rule_set);
		    if (excluded_set != NULL)
		      desktop_entry_set_subtract (excluded_set, rule_set);
                    desktop_entry_set_unref (rule_set);
                  }

                rule = menu_layout_node_get_next (rule);
              }

	    menu_verbose ("Processed <Include> (%d entries)\n",
			  desktop_entry_set_get_count (entries));
          }
          break;

        case MENU_LAYOUT_NODE_EXCLUDE:
          {
            /* The match rule children of the <Exclude> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuLayoutNode *rule;

	    menu_verbose ("Processing <Exclude> (%d entries)\n",
			  desktop_entry_set_get_count (entries));

            rule = menu_layout_node_get_children (layout_iter);
            while (rule != NULL)
              {
                DesktopEntrySet *rule_set;

                rule_set = process_include_rules (rule, entry_pool);
                if (rule_set != NULL)
                  {
		    if (excluded_set != NULL)
		      desktop_entry_set_union (excluded_set, rule_set);
		    desktop_entry_set_subtract (entries, rule_set);
		    desktop_entry_set_unref (rule_set);
                  }

                rule = menu_layout_node_get_next (rule);
              }

	    menu_verbose ("Processed <Exclude> (%d entries)\n",
			  desktop_entry_set_get_count (entries));
          }
          break;

        case MENU_LAYOUT_NODE_DIRECTORY:
          {
            DesktopEntry *entry;

	    menu_verbose ("Processing <Directory>%s</Directory>\n",
			  menu_layout_node_get_content (layout_iter));

	    /*
             * The last <Directory> to exist wins, so we always try overwriting
             */
            entry = entry_directory_list_get_directory (menu_layout_node_menu_get_directory_dirs (layout),
                                                        menu_layout_node_get_content (layout_iter));

            if (entry != NULL)
              {
                if (!desktop_entry_get_hidden (entry))
                  {
                    if (directory->directory_entry)
                      desktop_entry_unref (directory->directory_entry);
                    directory->directory_entry = entry; /* pass ref ownership */
                  }
                else
                  {
                    desktop_entry_unref (entry);
                  }
              }

            menu_verbose ("Processed <Directory> new directory entry = %p (%s)\n",
                          directory->directory_entry,
			  directory->directory_entry? desktop_entry_get_path (directory->directory_entry) : "null");
          }
          break;

        case MENU_LAYOUT_NODE_DELETED:
	  menu_verbose ("Processed <Deleted/>\n");
          deleted = TRUE;
          break;

        case MENU_LAYOUT_NODE_NOT_DELETED:
	  menu_verbose ("Processed <NotDeleted/>\n");
          deleted = FALSE;
          break;

        case MENU_LAYOUT_NODE_ONLY_UNALLOCATED:
	  menu_verbose ("Processed <OnlyUnallocated/>\n");
          only_unallocated = TRUE;
          break;

        case MENU_LAYOUT_NODE_NOT_ONLY_UNALLOCATED:
	  menu_verbose ("Processed <NotOnlyUnallocated/>\n");
          only_unallocated = FALSE;
          break;

	case MENU_LAYOUT_NODE_DEFAULT_LAYOUT:
	  menu_layout_node_default_layout_get_values (layout_iter,
						      &directory->default_layout_values);
	  collect_layout_info (layout_iter, &directory->default_layout_info);
	  menu_verbose ("Processed <DefaultLayout/>\n");
	  break;

	case MENU_LAYOUT_NODE_LAYOUT:
	  collect_layout_info (layout_iter, &directory->layout_info);
	  menu_verbose ("Processed <Layout/>\n");
	  break;

        default:
          break;
        }

      layout_iter = menu_layout_node_get_next (layout_iter);
    }

  desktop_entry_set_unref (entry_pool);

  directory->only_unallocated = only_unallocated;

  if (!directory->only_unallocated)
    desktop_entry_set_union (allocated, allocated_set);

  desktop_entry_set_unref (allocated_set);

  if (directory->directory_entry)
    {
      if (desktop_entry_get_no_display (directory->directory_entry))
        {
          directory->is_nodisplay = TRUE;

          if (!(tree->flags & UKUIMENU_TREE_FLAGS_INCLUDE_NODISPLAY))
            {
              menu_verbose ("Not showing menu %s because NoDisplay=true\n",
                        desktop_entry_get_name (directory->directory_entry));
              deleted = TRUE;
            }
        }

      if (!desktop_entry_get_show_in_ukui (directory->directory_entry))
        {
          menu_verbose ("Not showing menu %s because OnlyShowIn!=UKUI or NotShowIn=UKUI\n",
                        desktop_entry_get_name (directory->directory_entry));
          deleted = TRUE;
        }
    }

  if (deleted)
    {
      if (excluded_set != NULL)
	desktop_entry_set_unref (excluded_set);
      desktop_entry_set_unref (entries);
      ukuimenu_tree_item_unref (directory);
      return NULL;
    }

  desktop_entry_set_foreach (entries,
                             (DesktopEntrySetForeachFunc) entries_listify_foreach,
                             directory);
  desktop_entry_set_unref (entries);

  if (excluded_set != NULL)
    {
      desktop_entry_set_foreach (excluded_set,
				 (DesktopEntrySetForeachFunc) excluded_entries_listify_foreach,
				 directory);
      desktop_entry_set_unref (excluded_set);
    }

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;

      set_default_layout_values (directory, subdir);

      tmp = tmp->next;
   }

  tmp = directory->entries;
  while (tmp != NULL)
    {
      UkuiMenuTreeEntry *entry = tmp->data;
      GSList         *next  = tmp->next;
      gboolean        delete = FALSE;

      if (desktop_entry_get_hidden (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because Hidden=true\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (!(tree->flags & UKUIMENU_TREE_FLAGS_INCLUDE_NODISPLAY) &&
          desktop_entry_get_no_display (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because NoDisplay=true\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (!desktop_entry_get_show_in_ukui (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because OnlyShowIn!=UKUI or NotShowIn=UKUI\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (desktop_entry_get_tryexec_failed (entry->desktop_entry))
        {
          menu_verbose ("Deleting %s because TryExec failed\n",
                        desktop_entry_get_name (entry->desktop_entry));
          delete = TRUE;
        }

      if (delete)
        {
          directory->entries = g_slist_delete_link (directory->entries,
                                                   tmp);
          ukuimenu_tree_item_unref_and_unset_parent (entry);
        }

      tmp = next;
    }

  g_assert (directory->name != NULL);

  return directory;
}

static void
process_only_unallocated (UkuiMenuTree          *tree,
			  UkuiMenuTreeDirectory *directory,
			  DesktopEntrySet    *allocated)
{
  GSList *tmp;

  /* For any directory marked only_unallocated, we have to remove any
   * entries that were in fact allocated.
   */

  if (directory->only_unallocated)
    {
      tmp = directory->entries;
      while (tmp != NULL)
        {
          UkuiMenuTreeEntry *entry = tmp->data;
          GSList         *next  = tmp->next;

          if (desktop_entry_set_lookup (allocated, entry->desktop_file_id))
            {
              directory->entries = g_slist_delete_link (directory->entries,
                                                        tmp);
              ukuimenu_tree_item_unref_and_unset_parent (entry);
            }

          tmp = next;
        }
    }

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;

      process_only_unallocated (tree, subdir, allocated);

      tmp = tmp->next;
   }
}

static void preprocess_layout_info (UkuiMenuTree          *tree,
                                    UkuiMenuTreeDirectory *directory);

static GSList *
get_layout_info (UkuiMenuTreeDirectory *directory,
                 gboolean           *is_default_layout)
{
  UkuiMenuTreeDirectory *iter;

  if (directory->layout_info != NULL)
    {
      if (is_default_layout)
        {
          *is_default_layout = FALSE;
        }
      return directory->layout_info;
    }

  /* Even if there's no layout information at all, the result will be an
   * implicit default layout */
  if (is_default_layout)
    {
      *is_default_layout = TRUE;
    }

  iter = directory;
  while (iter != NULL)
    {
      /* FIXME: this is broken: we might skip real parent in the
       * XML structure, that are hidden because of inlining. */
      if (iter->default_layout_info != NULL)
	{
	  return iter->default_layout_info;
	}

      iter = UKUIMENU_TREE_ITEM (iter)->parent;
    }

  return NULL;
}

static void
get_values_with_defaults (MenuLayoutNode   *node,
			  MenuLayoutValues *layout_values,
			  MenuLayoutValues *default_layout_values)
{
  menu_layout_node_menuname_get_values (node, layout_values);

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_SHOW_EMPTY))
    layout_values->show_empty = default_layout_values->show_empty;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_MENUS))
    layout_values->inline_menus = default_layout_values->inline_menus;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_LIMIT))
    layout_values->inline_limit = default_layout_values->inline_limit;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_HEADER))
    layout_values->inline_header = default_layout_values->inline_header;

  if (!(layout_values->mask & MENU_LAYOUT_VALUES_INLINE_ALIAS))
    layout_values->inline_alias = default_layout_values->inline_alias;
}

static guint
get_real_subdirs_len (UkuiMenuTreeDirectory *directory)
{
  guint   len;
  GSList *tmp;

  len = 0;

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;

      tmp = tmp->next;

      if (subdir->will_inline_header != G_MAXUINT16)
        {
          len += get_real_subdirs_len (subdir) + g_slist_length (subdir->entries) + 1;
        }
      else
        len += 1;
    }

  return len;
}

static void
preprocess_layout_info_subdir_helper (UkuiMenuTree          *tree,
                                      UkuiMenuTreeDirectory *directory,
                                      UkuiMenuTreeDirectory *subdir,
                                      MenuLayoutValues   *layout_values,
                                      gboolean           *contents_added,
                                      gboolean           *should_remove)
{
  preprocess_layout_info (tree, subdir);

  *should_remove = FALSE;
  *contents_added = FALSE;

  if (subdir->subdirs == NULL && subdir->entries == NULL)
    {
      if (!(tree->flags & UKUIMENU_TREE_FLAGS_SHOW_EMPTY) &&
          !layout_values->show_empty)
	{
	  menu_verbose ("Not showing empty menu '%s'\n", subdir->name);
	  *should_remove = TRUE;
	}
    }

  else if (layout_values->inline_menus)
    {
      guint real_subdirs_len;

      real_subdirs_len = get_real_subdirs_len (subdir);

      if (layout_values->inline_alias &&
          real_subdirs_len + g_slist_length (subdir->entries) == 1)
        {
          UkuiMenuTreeAlias *alias;
          UkuiMenuTreeItem  *item;
          GSList         *list;

          if (subdir->subdirs != NULL)
            list = subdir->subdirs;
          else
            list = subdir->entries;

          item = UKUIMENU_TREE_ITEM (list->data);

          menu_verbose ("Inline aliasing '%s' to '%s'\n",
                        item->type == UKUIMENU_TREE_ITEM_ENTRY ?
                          ukuimenu_tree_entry_get_name (UKUIMENU_TREE_ENTRY (item)) :
                          (item->type == UKUIMENU_TREE_ITEM_DIRECTORY ?
                             ukuimenu_tree_directory_get_name (UKUIMENU_TREE_DIRECTORY (item)) :
                             ukuimenu_tree_directory_get_name (UKUIMENU_TREE_ALIAS (item)->directory)),
                        subdir->name);

          alias = ukuimenu_tree_alias_new (directory, subdir, item);

          g_slist_foreach (list,
                           (GFunc) ukuimenu_tree_item_unref_and_unset_parent,
                           NULL);
          g_slist_free (list);
          subdir->subdirs = NULL;
          subdir->entries = NULL;

          if (item->type == UKUIMENU_TREE_ITEM_DIRECTORY)
            directory->subdirs = g_slist_append (directory->subdirs, alias);
          else
            directory->entries = g_slist_append (directory->entries, alias);

          *contents_added = TRUE;
          *should_remove = TRUE;
        }

      else if (layout_values->inline_limit == 0 ||
               layout_values->inline_limit >= real_subdirs_len + g_slist_length (subdir->entries))
        {
          if (layout_values->inline_header)
            {
              menu_verbose ("Creating inline header with name '%s'\n", subdir->name);
              /* we're limited to 16-bits to spare some memory; if the limit is
               * higher than that (would be crazy), we just consider it's
               * unlimited */
              if (layout_values->inline_limit < G_MAXUINT16)
                subdir->will_inline_header = layout_values->inline_limit;
              else
                subdir->will_inline_header = 0;
            }
          else
            {
              g_slist_foreach (subdir->subdirs,
                               (GFunc) ukuimenu_tree_item_set_parent,
                               directory);
              directory->subdirs = g_slist_concat (directory->subdirs,
                                                   subdir->subdirs);
              subdir->subdirs = NULL;

              g_slist_foreach (subdir->entries,
                               (GFunc) ukuimenu_tree_item_set_parent,
                               directory);
              directory->entries = g_slist_concat (directory->entries,
                                                   subdir->entries);
              subdir->entries = NULL;

              *contents_added = TRUE;
              *should_remove = TRUE;
            }

          menu_verbose ("Inlining directory contents of '%s' to '%s'\n",
                        subdir->name, directory->name);
        }
    }
}

static void
preprocess_layout_info (UkuiMenuTree          *tree,
                        UkuiMenuTreeDirectory *directory)
{
  GSList   *tmp;
  GSList   *layout_info;
  gboolean  using_default_layout;
  GSList   *last_subdir;
  gboolean  strip_duplicates;
  gboolean  contents_added;
  gboolean  should_remove;
  GSList   *subdirs_sentinel;

  /* Note: we need to preprocess all menus, even if the layout mask for a menu
   * is MENU_LAYOUT_VALUES_NONE: in this case, we need to remove empty menus;
   * and the layout mask can be different for a submenu anyway */

  menu_verbose ("Processing menu layout inline hints for %s\n", directory->name);
  g_assert (!directory->preprocessed);

  strip_duplicates = FALSE;
  /* we use last_subdir to track the last non-inlined subdirectory */
  last_subdir = g_slist_last (directory->subdirs);

  /*
   * First process subdirectories with explicit layout
   */
  layout_info = get_layout_info (directory, &using_default_layout);
  tmp = layout_info;
  /* see comment below about Menuname to understand why we leave the loop if
   * last_subdir is NULL */
  while (tmp != NULL && last_subdir != NULL)
    {
      MenuLayoutNode     *node = tmp->data;
      MenuLayoutValues    layout_values;
      const char         *name;
      UkuiMenuTreeDirectory *subdir;
      GSList             *subdir_l;

      tmp = tmp->next;

      /* only Menuname nodes are relevant here */
      if (menu_layout_node_get_type (node) != MENU_LAYOUT_NODE_MENUNAME)
        continue;

      get_values_with_defaults (node,
                                &layout_values,
                                &directory->default_layout_values);

      /* find the subdirectory that is affected by those attributes */
      name = menu_layout_node_get_content (node);
      subdir = NULL;
      subdir_l = directory->subdirs;
      while (subdir_l != NULL)
        {
          subdir = subdir_l->data;

          if (!strcmp (subdir->name, name))
            break;

          subdir = NULL;
          subdir_l = subdir_l->next;

          /* We do not want to use Menuname on a menu that appeared via
           * inlining: without inlining, the Menuname wouldn't have matched
           * anything, and we want to keep the same behavior.
           * Unless the layout is a default layout, in which case the Menuname
           * does match the subdirectory. */
          if (!using_default_layout && subdir_l == last_subdir)
            {
              subdir_l = NULL;
              break;
            }
        }

      if (subdir == NULL)
        continue;

      preprocess_layout_info_subdir_helper (tree, directory,
                                            subdir, &layout_values,
                                            &contents_added, &should_remove);
      strip_duplicates = strip_duplicates || contents_added;
      if (should_remove)
        {
          if (last_subdir == subdir_l)
            {
              /* we need to recompute last_subdir since we'll remove it from
               * the list */
              GSList *buf;

              if (subdir_l == directory->subdirs)
                last_subdir = NULL;
              else
                {
                  buf = directory->subdirs;
                  while (buf != NULL && buf->next != subdir_l)
                    buf = buf->next;
                  last_subdir = buf;
                }
            }

          directory->subdirs = g_slist_remove (directory->subdirs, subdir);
          ukuimenu_tree_item_unref_and_unset_parent (UKUIMENU_TREE_ITEM (subdir));
        }
    }

  /*
   * Now process the subdirectories with no explicit layout
   */
  /* this is bogus data, but we just need the pointer anyway */
  subdirs_sentinel = g_slist_prepend (directory->subdirs, PACKAGE);
  directory->subdirs = subdirs_sentinel;

  tmp = directory->subdirs;
  while (tmp->next != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->next->data;

      if (subdir->preprocessed)
        {
          tmp = tmp->next;
          continue;
        }

      preprocess_layout_info_subdir_helper (tree, directory,
                                            subdir, &directory->default_layout_values,
                                            &contents_added, &should_remove);
      strip_duplicates = strip_duplicates || contents_added;
      if (should_remove)
        {
          tmp = g_slist_delete_link (tmp, tmp->next);
          ukuimenu_tree_item_unref_and_unset_parent (UKUIMENU_TREE_ITEM (subdir));
        }
      else
        tmp = tmp->next;
    }

  /* remove the sentinel */
  directory->subdirs = g_slist_delete_link (directory->subdirs,
                                            directory->subdirs);

  /*
   * Finally, remove duplicates if needed
   */
  if (strip_duplicates)
    {
      /* strip duplicate entries; there should be no duplicate directories */
      directory->entries = g_slist_sort (directory->entries,
                                         (GCompareFunc) ukuimenu_tree_entry_compare_by_id);
      tmp = directory->entries;
      while (tmp != NULL && tmp->next != NULL)
        {
          UkuiMenuTreeItem *a = tmp->data;
          UkuiMenuTreeItem *b = tmp->next->data;

          if (a->type == UKUIMENU_TREE_ITEM_ALIAS)
            a = UKUIMENU_TREE_ALIAS (a)->aliased_item;

          if (b->type == UKUIMENU_TREE_ITEM_ALIAS)
            b = UKUIMENU_TREE_ALIAS (b)->aliased_item;

          if (strcmp (UKUIMENU_TREE_ENTRY (a)->desktop_file_id,
                      UKUIMENU_TREE_ENTRY (b)->desktop_file_id) == 0)
            {
              tmp = g_slist_delete_link (tmp, tmp->next);
              ukuimenu_tree_item_unref (b);
            }
          else
            tmp = tmp->next;
        }
    }

  directory->preprocessed = TRUE;
}

static void process_layout_info (UkuiMenuTree          *tree,
				 UkuiMenuTreeDirectory *directory);

static void
check_pending_separator (UkuiMenuTreeDirectory *directory)
{
  if (directory->layout_pending_separator)
    {
      menu_verbose ("Adding pending separator in '%s'\n", directory->name);

      directory->contents = g_slist_append (directory->contents,
					    ukuimenu_tree_separator_new (directory));
      directory->layout_pending_separator = FALSE;
    }
}

static void
merge_alias (UkuiMenuTree          *tree,
	     UkuiMenuTreeDirectory *directory,
	     UkuiMenuTreeAlias     *alias)
{
  menu_verbose ("Merging alias '%s' in directory '%s'\n",
		alias->directory->name, directory->name);

  if (alias->aliased_item->type == UKUIMENU_TREE_ITEM_DIRECTORY)
    {
      process_layout_info (tree, UKUIMENU_TREE_DIRECTORY (alias->aliased_item));
    }

  check_pending_separator (directory);

  directory->contents = g_slist_append (directory->contents,
					ukuimenu_tree_item_ref (alias));
}

static void
merge_subdir (UkuiMenuTree          *tree,
	      UkuiMenuTreeDirectory *directory,
	      UkuiMenuTreeDirectory *subdir)
{
  menu_verbose ("Merging subdir '%s' in directory '%s'\n",
		subdir->name, directory->name);

  process_layout_info (tree, subdir);

  check_pending_separator (directory);

  if (subdir->will_inline_header == 0 ||
      (subdir->will_inline_header != G_MAXUINT16 &&
       g_slist_length (subdir->contents) <= subdir->will_inline_header))
    {
      UkuiMenuTreeHeader *header;

      header = ukuimenu_tree_header_new (directory, subdir);
      directory->contents = g_slist_append (directory->contents, header);

      g_slist_foreach (subdir->contents,
                       (GFunc) ukuimenu_tree_item_set_parent,
                       directory);
      directory->contents = g_slist_concat (directory->contents,
                                            subdir->contents);
      subdir->contents = NULL;
      subdir->will_inline_header = G_MAXUINT16;

      ukuimenu_tree_item_set_parent (UKUIMENU_TREE_ITEM (subdir), NULL);
    }
  else
    {
      directory->contents = g_slist_append (directory->contents,
					    ukuimenu_tree_item_ref (subdir));
    }
}

static void
merge_subdir_by_name (UkuiMenuTree          *tree,
		      UkuiMenuTreeDirectory *directory,
		      const char         *subdir_name)
{
  GSList *tmp;

  menu_verbose ("Attempting to merge subdir '%s' in directory '%s'\n",
		subdir_name, directory->name);

  tmp = directory->subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;
      GSList             *next = tmp->next;

      /* if it's an alias, then it cannot be affected by
       * the Merge nodes in the layout */
      if (UKUIMENU_TREE_ITEM (subdir)->type == UKUIMENU_TREE_ITEM_ALIAS)
        continue;

      if (!strcmp (subdir->name, subdir_name))
	{
	  directory->subdirs = g_slist_delete_link (directory->subdirs, tmp);
	  merge_subdir (tree, directory, subdir);
	  ukuimenu_tree_item_unref (subdir);
	}

      tmp = next;
    }
}

static void
merge_entry (UkuiMenuTree          *tree,
	     UkuiMenuTreeDirectory *directory,
	     UkuiMenuTreeEntry     *entry)
{
  menu_verbose ("Merging entry '%s' in directory '%s'\n",
		entry->desktop_file_id, directory->name);

  check_pending_separator (directory);
  directory->contents = g_slist_append (directory->contents,
					ukuimenu_tree_item_ref (entry));
}

static void
merge_entry_by_id (UkuiMenuTree          *tree,
		   UkuiMenuTreeDirectory *directory,
		   const char         *file_id)
{
  GSList *tmp;

  menu_verbose ("Attempting to merge entry '%s' in directory '%s'\n",
		file_id, directory->name);

  tmp = directory->entries;
  while (tmp != NULL)
    {
      UkuiMenuTreeEntry *entry = tmp->data;
      GSList         *next = tmp->next;

      /* if it's an alias, then it cannot be affected by
       * the Merge nodes in the layout */
      if (UKUIMENU_TREE_ITEM (entry)->type == UKUIMENU_TREE_ITEM_ALIAS)
        continue;

      if (!strcmp (entry->desktop_file_id, file_id))
	{
	  directory->entries = g_slist_delete_link (directory->entries, tmp);
	  merge_entry (tree, directory, entry);
	  ukuimenu_tree_item_unref (entry);
	}

      tmp = next;
    }
}

static inline gboolean
find_name_in_list (const char *name,
		   GSList     *list)
{
  while (list != NULL)
    {
      if (!strcmp (name, list->data))
	return TRUE;

      list = list->next;
    }

  return FALSE;
}

static void
merge_subdirs (UkuiMenuTree          *tree,
	       UkuiMenuTreeDirectory *directory,
	       GSList             *except)
{
  GSList *subdirs;
  GSList *tmp;

  menu_verbose ("Merging subdirs in directory '%s'\n", directory->name);

  subdirs = directory->subdirs;
  directory->subdirs = NULL;

  subdirs = g_slist_sort_with_data (subdirs,
				    (GCompareDataFunc) ukuimenu_tree_item_compare,
				     GINT_TO_POINTER (UKUIMENU_TREE_SORT_NAME));

  tmp = subdirs;
  while (tmp != NULL)
    {
      UkuiMenuTreeDirectory *subdir = tmp->data;

      if (UKUIMENU_TREE_ITEM (subdir)->type == UKUIMENU_TREE_ITEM_ALIAS)
        {
	  merge_alias (tree, directory, UKUIMENU_TREE_ALIAS (subdir));
	  ukuimenu_tree_item_unref (subdir);
        }
      else if (!find_name_in_list (subdir->name, except))
	{
	  merge_subdir (tree, directory, subdir);
	  ukuimenu_tree_item_unref (subdir);
	}
      else
	{
	  menu_verbose ("Not merging directory '%s' yet\n", subdir->name);
	  directory->subdirs = g_slist_append (directory->subdirs, subdir);
	}

      tmp = tmp->next;
    }

  g_slist_free (subdirs);
  g_slist_free (except);
}

static void
merge_entries (UkuiMenuTree          *tree,
	       UkuiMenuTreeDirectory *directory,
	       GSList             *except)
{
  GSList *entries;
  GSList *tmp;

  menu_verbose ("Merging entries in directory '%s'\n", directory->name);

  entries = directory->entries;
  directory->entries = NULL;

  entries = g_slist_sort_with_data (entries,
				    (GCompareDataFunc) ukuimenu_tree_item_compare,
				    GINT_TO_POINTER (tree->sort_key));

  tmp = entries;
  while (tmp != NULL)
    {
      UkuiMenuTreeEntry *entry = tmp->data;

      if (UKUIMENU_TREE_ITEM (entry)->type == UKUIMENU_TREE_ITEM_ALIAS)
        {
	  merge_alias (tree, directory, UKUIMENU_TREE_ALIAS (entry));
	  ukuimenu_tree_item_unref (entry);
        }
      else if (!find_name_in_list (entry->desktop_file_id, except))
	{
	  merge_entry (tree, directory, entry);
	  ukuimenu_tree_item_unref (entry);
	}
      else
	{
	  menu_verbose ("Not merging entry '%s' yet\n", entry->desktop_file_id);
	  directory->entries = g_slist_append (directory->entries, entry);
	}

      tmp = tmp->next;
    }

  g_slist_free (entries);
  g_slist_free (except);
}

static void
merge_subdirs_and_entries (UkuiMenuTree          *tree,
			   UkuiMenuTreeDirectory *directory,
			   GSList             *except_subdirs,
			   GSList             *except_entries)
{
  GSList *items;
  GSList *tmp;

  menu_verbose ("Merging subdirs and entries together in directory %s\n",
		directory->name);

  items = g_slist_concat (directory->subdirs, directory->entries);

  directory->subdirs = NULL;
  directory->entries = NULL;

  items = g_slist_sort_with_data (items,
				  (GCompareDataFunc) ukuimenu_tree_item_compare,
				  GINT_TO_POINTER (tree->sort_key));

  tmp = items;
  while (tmp != NULL)
    {
      UkuiMenuTreeItem     *item = tmp->data;
      UkuiMenuTreeItemType  type;

      type = ukuimenu_tree_item_get_type (item);

      if (type == UKUIMENU_TREE_ITEM_ALIAS)
        {
          merge_alias (tree, directory, UKUIMENU_TREE_ALIAS (item));
          ukuimenu_tree_item_unref (item);
        }
      else if (type == UKUIMENU_TREE_ITEM_DIRECTORY)
	{
	  if (!find_name_in_list (UKUIMENU_TREE_DIRECTORY (item)->name, except_subdirs))
	    {
	      merge_subdir (tree,
			    directory,
			    UKUIMENU_TREE_DIRECTORY (item));
	      ukuimenu_tree_item_unref (item);
	    }
	  else
	    {
	      menu_verbose ("Not merging directory '%s' yet\n",
			    UKUIMENU_TREE_DIRECTORY (item)->name);
	      directory->subdirs = g_slist_append (directory->subdirs, item);
	    }
	}
      else if (type == UKUIMENU_TREE_ITEM_ENTRY)
	{
	  if (!find_name_in_list (UKUIMENU_TREE_ENTRY (item)->desktop_file_id, except_entries))
	    {
	      merge_entry (tree, directory, UKUIMENU_TREE_ENTRY (item));
	      ukuimenu_tree_item_unref (item);
	    }
	  else
	    {
	      menu_verbose ("Not merging entry '%s' yet\n",
			    UKUIMENU_TREE_ENTRY (item)->desktop_file_id);
	      directory->entries = g_slist_append (directory->entries, item);
	    }
	}
      else
        {
          g_assert_not_reached ();
        }

      tmp = tmp->next;
    }

  g_slist_free (items);
  g_slist_free (except_subdirs);
  g_slist_free (except_entries);
}

static GSList *
get_subdirs_from_layout_info (GSList *layout_info)
{
  GSList *subdirs;
  GSList *tmp;

  subdirs = NULL;

  tmp = layout_info;
  while (tmp != NULL)
    {
      MenuLayoutNode *node = tmp->data;

      if (menu_layout_node_get_type (node) == MENU_LAYOUT_NODE_MENUNAME)
	{
	  subdirs = g_slist_append (subdirs,
				    (char *) menu_layout_node_get_content (node));
	}

      tmp = tmp->next;
    }

  return subdirs;
}

static GSList *
get_entries_from_layout_info (GSList *layout_info)
{
  GSList *entries;
  GSList *tmp;

  entries = NULL;

  tmp = layout_info;
  while (tmp != NULL)
    {
      MenuLayoutNode *node = tmp->data;

      if (menu_layout_node_get_type (node) == MENU_LAYOUT_NODE_FILENAME)
	{
	  entries = g_slist_append (entries,
				    (char *) menu_layout_node_get_content (node));
	}

      tmp = tmp->next;
    }

  return entries;
}

static void
process_layout_info (UkuiMenuTree          *tree,
		     UkuiMenuTreeDirectory *directory)
{
  GSList *layout_info;

  menu_verbose ("Processing menu layout hints for %s\n", directory->name);

  g_slist_foreach (directory->contents,
		   (GFunc) ukuimenu_tree_item_unref_and_unset_parent,
		   NULL);
  g_slist_free (directory->contents);
  directory->contents = NULL;
  directory->layout_pending_separator = FALSE;

  layout_info = get_layout_info (directory, NULL);

  if (layout_info == NULL)
    {
      merge_subdirs (tree, directory, NULL);
      merge_entries (tree, directory, NULL);
    }
  else
    {
      GSList *tmp;

      tmp = layout_info;
      while (tmp != NULL)
	{
	  MenuLayoutNode *node = tmp->data;

	  switch (menu_layout_node_get_type (node))
	    {
	    case MENU_LAYOUT_NODE_MENUNAME:
              merge_subdir_by_name (tree,
                                    directory,
                                    menu_layout_node_get_content (node));
	      break;

	    case MENU_LAYOUT_NODE_FILENAME:
	      merge_entry_by_id (tree,
				 directory,
				 menu_layout_node_get_content (node));
	      break;

	    case MENU_LAYOUT_NODE_SEPARATOR:
	      /* Unless explicitly told to show all separators, do not show a
	       * separator at the beginning of a menu. Note that we don't add
	       * the separators now, and instead make it pending. This way, we
	       * won't show two consecutive separators nor will we show a
	       * separator at the end of a menu. */
              if (tree->flags & UKUIMENU_TREE_FLAGS_SHOW_ALL_SEPARATORS)
		{
		  directory->layout_pending_separator = TRUE;
		  check_pending_separator (directory);
		}
	      else if (directory->contents)
		{
		  menu_verbose ("Adding a potential separator in '%s'\n",
				directory->name);

		  directory->layout_pending_separator = TRUE;
		}
	      else
		{
		  menu_verbose ("Skipping separator at the beginning of '%s'\n",
				directory->name);
		}
	      break;

	    case MENU_LAYOUT_NODE_MERGE:
	      switch (menu_layout_node_merge_get_type (node))
		{
		case MENU_LAYOUT_MERGE_NONE:
		  break;

		case MENU_LAYOUT_MERGE_MENUS:
		  merge_subdirs (tree,
				 directory,
				 get_subdirs_from_layout_info (tmp->next));
		  break;

		case MENU_LAYOUT_MERGE_FILES:
		  merge_entries (tree,
				 directory,
				 get_entries_from_layout_info (tmp->next));
		  break;

		case MENU_LAYOUT_MERGE_ALL:
		  merge_subdirs_and_entries (tree,
					     directory,
					     get_subdirs_from_layout_info (tmp->next),
					     get_entries_from_layout_info (tmp->next));
		  break;

		default:
		  g_assert_not_reached ();
		  break;
		}
	      break;

	    default:
	      g_assert_not_reached ();
	      break;
	    }

	  tmp = tmp->next;
	}
    }

  g_slist_foreach (directory->subdirs,
		   (GFunc) ukuimenu_tree_item_unref,
		   NULL);
  g_slist_free (directory->subdirs);
  directory->subdirs = NULL;

  g_slist_foreach (directory->entries,
		   (GFunc) ukuimenu_tree_item_unref,
		   NULL);
  g_slist_free (directory->entries);
  directory->entries = NULL;

  g_slist_foreach (directory->default_layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->default_layout_info);
  directory->default_layout_info = NULL;

  g_slist_foreach (directory->layout_info,
		   (GFunc) menu_layout_node_unref,
		   NULL);
  g_slist_free (directory->layout_info);
  directory->layout_info = NULL;
}

static void
handle_entries_changed (MenuLayoutNode *layout,
                        UkuiMenuTree       *tree)
{
  if (tree->layout == layout)
    {
      ukuimenu_tree_force_rebuild (tree);
      ukuimenu_tree_invoke_monitors (tree);
    }
}

static void
ukuimenu_tree_build_from_layout (UkuiMenuTree *tree)
{
  DesktopEntrySet *allocated;

  if (tree->root)
    return;

  ukuimenu_tree_load_layout (tree);
  if (!tree->layout)
    return;

  menu_verbose ("Building menu tree from layout\n");

  allocated = desktop_entry_set_new ();

  /* create the menu structure */
  tree->root = process_layout (tree,
                               NULL,
                               find_menu_child (tree->layout),
                               allocated);
  if (tree->root)
    {
      ukuimenu_tree_directory_set_tree (tree->root, tree);

      process_only_unallocated (tree, tree->root, allocated);

      /* process the layout info part that can move/remove items:
       * inline, show_empty, etc. */
      preprocess_layout_info (tree, tree->root);
      /* populate the menu structure that we got with the items, and order it
       * according to the layout info */
      process_layout_info (tree, tree->root);

      menu_layout_node_root_add_entries_monitor (tree->layout,
                                                 (MenuLayoutNodeEntriesChangedFunc) handle_entries_changed,
                                                 tree);
    }

  desktop_entry_set_unref (allocated);
}

static void
ukuimenu_tree_force_rebuild (UkuiMenuTree *tree)
{
  if (tree->root)
    {
      ukuimenu_tree_directory_set_tree (tree->root, NULL);
      ukuimenu_tree_item_unref (tree->root);
      tree->root = NULL;

      g_assert (tree->layout != NULL);

      menu_layout_node_root_remove_entries_monitor (tree->layout,
                                                    (MenuLayoutNodeEntriesChangedFunc) handle_entries_changed,
                                                    tree);
    }
}
