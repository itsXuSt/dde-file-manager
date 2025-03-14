/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <assert.h>
#include <glib/gstdio.h>
#include <fnmatch.h>
#include <regex.h>
#include <fstab.h>

#include "database.h"
#include "fsearch_config.h"
#include "fsearch.h"
#include "fsearch_pinyin.h"

//#define WS_FOLLOWLINK	(1 << 1)	/* follow symlinks */
#define WS_DOTFILES (1 << 2) /* per unix convention, .file is hidden */

// add py index,update database version to 1.0
#define DATABASE_MAJOR_VERSION 1
#define DATABASE_MINOR_VERSION 0
#define MAX_DIR_DEPTH 100

#define DATABASE_FILTER_PATH "^((/boot)|(/dev)|(/proc)|(/sys)|(/root)|(/run)).*$"

struct _DatabaseLocation
{
    // B+ tree of entry nodes
    BTreeNode *entries;
    uint32_t num_items;
};

enum {
    WALK_OK = 0,
    WALK_BADPATTERN,
    WALK_NAMETOOLONG,
    WALK_BADIO,
};

// Forward declarations
static void
db_entries_clear(Database *db);

static DatabaseLocation *
db_location_get_for_path(Database *db, const char *path);

static DatabaseLocation *
db_location_build_tree(const char *dname, DatabaseConfig *db_config, bool *is_stop, void (*callback)(const char *));

static DatabaseLocation *
db_location_new(void);

static void
db_list_add_location(Database *db, DatabaseLocation *location);

// Implemenation

static void
db_update_timestamp(Database *db)
{
    assert(db != NULL);
    db->timestamp = time(NULL);
}

static GList *
get_fstable_bindinfo(void)
{
    static GList *list = NULL;
    if (list != NULL)
        return list;

    struct fstab *fs;
    setfsent();
    while ((fs = getfsent()) != NULL) {
        char *mntops = fs->fs_mntops;
        if (strstr(mntops, "bind") != NULL)
            list = g_list_append(list, strdup(fs->fs_spec));
    }
    endfsent();

    return list;
}

DatabaseLocation *
db_location_load_from_file(const char *fname)
{
    assert(fname != NULL);

    FILE *fp = fopen(fname, "rb");
    if (!fp) {
        return NULL;
    }

    BTreeNode *root = NULL;

    char magic[4];
    if (fread(magic, 1, 4, fp) != 4) {
        printf("failed to read magic\n");
        goto load_fail;
    }
    if (strncmp(magic, "FSDB", 4)) {
        printf("bad signature\n");
        goto load_fail;
    }

    uint8_t majorver = 0;
    if (fread(&majorver, 1, 1, fp) != 1) {
        goto load_fail;
    }

    if (majorver != DATABASE_MAJOR_VERSION) {
        printf("bad majorver=%d\n", majorver);
        goto load_fail;
    }

    uint8_t minorver = 0;
    if (fread(&minorver, 1, 1, fp) != 1) {
        goto load_fail;
    }

    if (minorver != DATABASE_MINOR_VERSION) {
        printf("bad minorver=%d\n", minorver);
        goto load_fail;
    }
    printf("database version=%d.%d\n", majorver, minorver);

    uint32_t num_items = 0;
    if (fread(&num_items, 1, 4, fp) != 4) {
        goto load_fail;
    }

    uint32_t num_items_read = 0;
    BTreeNode *prev = NULL;
    while (true) {
        uint16_t name_len = 0;
        if (fread(&name_len, 1, 2, fp) != 2) {
            printf("failed to read name length\n");
            goto load_fail;
        }

        if (name_len == 0) {
            // reached end of child marker
            if (!prev) {
                goto load_fail;
            }
            prev = prev->parent;
            if (!prev) {
                // prev was root node, we're done
                printf("reached root node. done\n");
                break;
            }
            continue;
        }

        // read name
        char name[name_len + 1];
        if (fread(&name, 1, name_len, fp) != name_len) {
            printf("failed to read name\n");
            goto load_fail;
        }
        name[name_len] = '\0';

        // read full pinyin
        uint16_t full_py_len = 0;
        if (fread(&full_py_len, 1, 2, fp) != 2) {
            printf("failed to read full pinyin name length\n");
            goto load_fail;
        }
        char full_py_name[full_py_len + 1];
        if (full_py_len && fread(&full_py_name, 1, full_py_len, fp) != full_py_len) {
            printf("failed to read full pinyin name\n");
            goto load_fail;
        }
        full_py_name[full_py_len] = '\0';

        // read first pinyin name
        uint16_t first_py_len = 0;
        if (fread(&first_py_len, 1, 2, fp) != 2) {
            printf("failed to read first pinyin name length\n");
            goto load_fail;
        }
        char first_py_name[first_py_len + 1];
        if (first_py_len && fread(&first_py_name, 1, first_py_len, fp) != first_py_len) {
            printf("failed to read first pinyin name\n");
            goto load_fail;
        }
        first_py_name[first_py_len] = '\0';

        // read is_dir
        uint8_t is_dir = 0;
        if (fread(&is_dir, 1, 1, fp) != 1) {
            printf("failed to read is_dir\n");
            goto load_fail;
        }

        // read size
        uint64_t size = 0;
        if (fread(&size, 1, 8, fp) != 8) {
            printf("failed to read size\n");
            goto load_fail;
        }

        // read mtime
        uint64_t mtime = 0;
        if (fread(&mtime, 1, 8, fp) != 8) {
            printf("failed to read mtime\n");
            goto load_fail;
        }

        // read mtime
        uint32_t pos = 0;
        if (fread(&pos, 1, 4, fp) != 4) {
            printf("failed to read sort position\n");
            goto load_fail;
        }

        int is_root = !strcmp(name, "/");
        BTreeNode *new = btree_node_new(is_root ? "" : name,
                                        is_root ? "" : full_py_name,
                                        is_root ? "" : first_py_name,
                                        mtime,
                                        size,
                                        pos,
                                        is_dir);
        if (!prev) {
            prev = new;
            root = new;
            continue;
        }
        prev = btree_node_prepend(prev, new);
        num_items_read++;
    }
    //    trace ("read database: %d/%d\n", num_items_read, num_items);

    DatabaseLocation *location = db_location_new();
    location->num_items = num_items_read;
    location->entries = root;

    fclose(fp);

    return location;

load_fail:
    fprintf(stderr, "database load fail (%s)!\n", fname);
    if (fp) {
        fclose(fp);
    }
    if (root) {
        btree_node_free(root);
    }
    return NULL;
}

bool db_location_write_to_file(DatabaseLocation *location, const char *path)
{
    assert(path != NULL);
    assert(location != NULL);

    if (!location->entries) {
        return false;
    }
    g_mkdir_with_parents(path, 0700);

    gchar tempfile[PATH_MAX] = "";
    snprintf(tempfile, sizeof(tempfile), "%s/database.db", path);

    FILE *fp = fopen(tempfile, "w+b");
    if (!fp) {
        return false;
    }

    const char magic[] = "FSDB";
    if (fwrite(magic, 1, 4, fp) != 4) {
        goto save_fail;
    }

    const uint8_t majorver = DATABASE_MAJOR_VERSION;
    if (fwrite(&majorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    const uint8_t minorver = DATABASE_MINOR_VERSION;
    if (fwrite(&minorver, 1, 1, fp) != 1) {
        goto save_fail;
    }

    uint32_t num_items = btree_node_n_nodes(location->entries);
    if (fwrite(&num_items, 1, 4, fp) != 4) {
        goto save_fail;
    }

    const uint16_t del = 0;

    BTreeNode *root = location->entries;
    BTreeNode *node = root;
    uint32_t is_root = !strcmp(root->name, "");

    while (node) {
        const char *name = is_root ? "/" : node->name;
        is_root = 0;
        uint16_t len = (uint16_t)strlen(name);
        uint16_t full_py_len = (uint16_t)strlen(node->full_py_name);
        uint16_t first_py_len = (uint16_t)strlen(node->first_py_name);
        if (len) {
            // write length of node name
            if (fwrite(&len, 1, 2, fp) != 2) {
                goto save_fail;
            }
            // write node name
            if (fwrite(name, 1, len, fp) != len) {
                goto save_fail;
            }
            // write length of node full pinyin name
            if (fwrite(&full_py_len, 1, 2, fp) != 2) {
                goto save_fail;
            }
            // write node full pinyin name
            if (full_py_len && fwrite(node->full_py_name, 1, full_py_len, fp) != full_py_len) {
                goto save_fail;
            }
            // write length of node first pinyin name
            if (fwrite(&first_py_len, 1, 2, fp) != 2) {
                goto save_fail;
            }
            // write node first pinyin name
            if (first_py_len && fwrite(node->first_py_name, 1, first_py_len, fp) != first_py_len) {
                goto save_fail;
            }

            // write node name
            uint8_t is_dir = node->is_dir;
            if (fwrite(&is_dir, 1, 1, fp) != 1) {
                goto save_fail;
            }

            // write node name
            uint64_t size = node->size;
            if (fwrite(&size, 1, 8, fp) != 8) {
                goto save_fail;
            }

            // write node name
            uint64_t mtime = node->mtime;
            if (fwrite(&mtime, 1, 8, fp) != 8) {
                goto save_fail;
            }

            // write node name
            uint32_t pos = node->pos;
            if (fwrite(&pos, 1, 4, fp) != 4) {
                goto save_fail;
            }

            BTreeNode *temp = node->children;
            if (!temp) {
                // reached end of children, write delimiter
                if (fwrite(&del, 1, 2, fp) != 2) {
                    goto save_fail;
                }
                BTreeNode *current = node;
                while (true) {
                    temp = current->next;
                    if (temp) {
                        // found next, sibling add that
                        node = temp;
                        break;
                    }

                    if (fwrite(&del, 1, 2, fp) != 2) {
                        goto save_fail;
                    }
                    temp = current->parent;
                    if (!temp) {
                        // reached last node, abort
                        node = NULL;
                        break;
                    } else {
                        current = temp;
                    }
                }
            } else {
                node = temp;
            }
        } else {
            goto save_fail;
        }
    }

    fclose(fp);
    return true;

save_fail:

    fclose(fp);
    unlink(tempfile);
    return false;
}

static bool
file_is_excluded(const char *name, char **exclude_files)
{
    if (exclude_files) {
        for (int i = 0; exclude_files[i]; ++i) {
            if (!fnmatch(exclude_files[i], name, 0)) {
                return true;
            }
        }
    }
    return false;
}

static bool
directory_is_excluded(const char *name, GList *excludes)
{
    while (excludes) {
        if (!strcmp(name, excludes->data)) {
            return true;
        }
        excludes = excludes->next;
    }
    return false;
}

static int
db_location_walk_tree_recursive(DatabaseLocation *location,
                                DatabaseConfig *db_config,
                                GList *excludes,
                                char **exclude_files,
                                const char *dname,
                                GTimer *timer,
                                void (*callback)(const char *),
                                BTreeNode *parent,
                                int spec,
                                bool *is_stop,
                                bool has_data_prefix,
                                const int depth)
{
    if (*is_stop)
        return WALK_OK;

    if (!db_support(dname, has_data_prefix))
        return WALK_BADPATTERN;

    size_t len = strlen(dname);
    if (len >= FILENAME_MAX - 1) {
        //trace ("filename too long: %s\n", dname);
        return WALK_NAMETOOLONG;
    }

    char fn[FILENAME_MAX] = "";
    strcpy(fn, dname);
    if (strcmp(dname, "/")) {
        // TODO: use a more performant fix to handle root directory
        fn[len++] = '/';
    }

    DIR *dir = NULL;
    if (!(dir = opendir(dname))) {
        //trace ("can't open: %s\n", dname);
        return WALK_BADIO;
    }
    gulong duration = 0;
    g_timer_elapsed(timer, &duration);

    if (duration > 100000) {
        if (callback) {
            callback(dname);
        }
        g_timer_reset(timer);
    }

    struct dirent *dent = NULL;
    while ((dent = readdir(dir))) {
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }

        if (db_config->filter_hidden_file && dent->d_name[0] == '.')
            continue;

        if (file_is_excluded(dent->d_name, exclude_files)) {
            //trace ("excluded file: %s\n", dent->d_name);
            continue;
        }

        struct stat st;
        strncpy(fn + len, dent->d_name, FILENAME_MAX - len);
        if (lstat(fn, &st) == -1) {
            //warn("Can't stat %s", fn);
            continue;
        }

        if (directory_is_excluded(fn, excludes)) {
            //            trace ("excluded directory: %s\n", fn);
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        char full_py_name[FILENAME_MAX] = "";
        char first_py_name[FILENAME_MAX] = "";
        if (db_config->enable_py)
            convert_all_pinyin(dent->d_name, first_py_name, full_py_name);

        BTreeNode *node = btree_node_new(dent->d_name,
                                         full_py_name,
                                         first_py_name,
                                         st.st_mtime,
                                         st.st_size,
                                         0,
                                         is_dir);
        btree_node_prepend(parent, node);
        location->num_items++;
        if (is_dir && depth <= MAX_DIR_DEPTH) {
            db_location_walk_tree_recursive(location,
                                            db_config,
                                            excludes,
                                            exclude_files,
                                            fn,
                                            timer,
                                            callback,
                                            node,
                                            spec,
                                            is_stop,
                                            has_data_prefix,
                                            depth + 1);
        }
    }

    if (dir) {
        closedir(dir);
    }
    return WALK_OK;
}

static DatabaseLocation *
db_location_build_tree(const char *dname, DatabaseConfig *db_config, bool *is_stop, void (*callback)(const char *))
{
    const char *root_name = NULL;
    if (!strcmp(dname, "/")) {
        root_name = "";
    } else {
        root_name = dname;
    }
    BTreeNode *root = btree_node_new(root_name, "", "", 0, 0, 0, true);
    DatabaseLocation *location = db_location_new();
    location->entries = root;
    FsearchConfig *config = (FsearchConfig *)(calloc(1, sizeof(FsearchConfig)));
    config_load_default(config);

    int spec = 0;
    if (!config->exclude_hidden_items) {
        spec |= WS_DOTFILES;
    }
    GTimer *timer = g_timer_new();
    g_timer_start(timer);

    bool has_data_prefix = false;
    GList *info = get_fstable_bindinfo();
    for (info = g_list_first(info); info != NULL; info = g_list_next(info)) {
        char *data = info->data;
        if (strncmp(data, dname, strlen(data)) == 0) {
            has_data_prefix = true;
            break;
        }
    }

    uint32_t res = db_location_walk_tree_recursive(location,
                                                   db_config,
                                                   config->exclude_locations,
                                                   config->exclude_files,
                                                   dname,
                                                   timer,
                                                   callback,
                                                   root,
                                                   spec,
                                                   is_stop,
                                                   has_data_prefix,
                                                   0);
    config_free(config);
    g_timer_destroy(timer);
    if (res == WALK_OK) {
        return location;
    } else {
        //        trace ("walk error: %d", res);
        db_location_free(location);
    }
    return NULL;
}

static DatabaseLocation *
db_location_new(void)
{
    DatabaseLocation *location = g_new0(DatabaseLocation, 1);
    return location;
}

bool db_list_insert_node(BTreeNode *node, void *data)
{
    Database *db = data;
    darray_set_item(db->entries, node, node->pos);
    db->num_entries++;
    return true;
}

static void
db_traverse_tree_insert(BTreeNode *node, void *data)
{
    btree_node_traverse(node, db_list_insert_node, data);
}

static uint32_t temp_index = 0;

bool db_list_add_node(BTreeNode *node, void *data)
{
    Database *db = data;
    darray_set_item(db->entries, node, temp_index++);
    db->num_entries++;
    return true;
}

static void
db_traverse_tree_add(BTreeNode *node, void *data)
{
    btree_node_traverse(node, db_list_add_node, data);
}

static void
db_list_insert_location(Database *db, DatabaseLocation *location)
{
    assert(db != NULL);
    assert(location != NULL);
    assert(location->entries != NULL);

    btree_node_children_foreach(location->entries, db_traverse_tree_insert, db);
}

static void
db_list_add_location(Database *db, DatabaseLocation *location)
{
    assert(db != NULL);
    assert(location != NULL);
    assert(location->entries != NULL);

    btree_node_children_foreach(location->entries, db_traverse_tree_add, db);
}

static DatabaseLocation *
db_location_get_for_path(Database *db, const char *path)
{
    assert(db != NULL);
    assert(path != NULL);

    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        DatabaseLocation *location = (DatabaseLocation *)l->data;
        BTreeNode *root = btree_node_get_root(location->entries);
        const char *location_path = root->name;
        if (!strcmp(location_path, path)) {
            return location;
        }
    }
    return NULL;
}

void db_location_free(DatabaseLocation *location)
{
    assert(location != NULL);

    if (location->entries) {
        btree_node_free(location->entries);
        location->entries = NULL;
    }
    g_free(location);
    location = NULL;
}

bool db_location_remove(Database *db, const char *path)
{
    assert(db != NULL);
    assert(path != NULL);

    DatabaseLocation *location = db_location_get_for_path(db, path);
    if (location) {
        db->locations = g_list_remove(db->locations, location);
        db_location_free(location);
        db_sort(db);
    }

    return true;
}

static void
location_build_path(char *path, size_t path_len, const char *location_name)
{
    assert(path != NULL);
    assert(location_name != NULL);

    //    char const *location = !strcmp (location_name, "") ? "/" : location_name;

    //    gchar *path_checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
    //                                                          location,
    //                                                          -1);
    //    assert (path_checksum != NULL);

    //    gchar config_dir[PATH_MAX] = "";
    //    config_build_dir (config_dir, sizeof (config_dir));
    //    database_build_dir(config_dir, sizeof (config_dir));

    assert(0 <= snprintf(path, path_len, "%s/database", location_name));
    //    g_free (path_checksum);
    return;
}

void db_location_delete(DatabaseLocation *location, const char *location_name)
{
    assert(location != NULL);
    assert(location_name != NULL);

    gchar database_path[PATH_MAX] = "";
    location_build_path(database_path,
                        sizeof(database_path),
                        location_name);

    gchar database_file_path[PATH_MAX] = "";
    assert(0 <= snprintf(database_file_path, sizeof(database_file_path), "%s/%s", database_path, "database.db"));

    g_remove(database_file_path);
    g_remove(database_path);
}

bool db_save_location(Database *db, const char *location_name, const char *save_path)
{
    assert(db != NULL);

    //    gchar database_path[PATH_MAX] = "";
    //    location_build_path (database_path,
    //                         sizeof (database_path),
    //                         save_path);
    //    trace ("%s\n", database_path);

    gchar database_fname[PATH_MAX] = "";
    assert(0 <= snprintf(database_fname, sizeof(database_fname), "%s/database.db", save_path));
    DatabaseLocation *location = db_location_get_for_path(db, location_name);
    if (location) {
        db_location_write_to_file(location, save_path);
    }

    return true;
}

bool db_save_locations(Database *db, const char *save_path)
{
    assert(db != NULL);
    g_return_val_if_fail(db->locations != NULL, false);

    //db_update_sort_index (db);
    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        DatabaseLocation *location = (DatabaseLocation *)l->data;
        BTreeNode *root = btree_node_get_root(location->entries);
        const char *location_path = root->name;
        db_save_location(db, location_path, save_path);
    }
    return true;
}

static gchar *
db_location_get_path(const char *location_name)
{
    //    gchar database_path[PATH_MAX] = "";
    //    location_build_path (database_path,
    //                         sizeof (database_path),
    //                         location_name);
    //    trace ("%s\n", database_path);

    gchar database_fname[PATH_MAX] = "";
    assert(0 <= snprintf(database_fname, sizeof(database_fname), "%s/database.db", location_name));

    return g_strdup(database_fname);
}

bool db_location_load(Database *db, const char *location_name)
{
    db_lock(db);
    gchar *load_path = db_location_get_path(location_name);
    if (!load_path) {
        db_unlock(db);
        return false;
    }
    DatabaseLocation *location = db_location_load_from_file(load_path);
    g_free(load_path);
    load_path = NULL;

    if (location) {
        location->num_items = btree_node_n_nodes(location->entries);
        //        trace ("number of nodes: %d\n", location->num_items);
        db->locations = g_list_append(db->locations, location);
        db->num_entries += location->num_items;
        db_update_timestamp(db);
        db_unlock(db);
        return true;
    }
    db_update_timestamp(db);
    db_unlock(db);
    return false;
}

bool db_location_add(Database *db,
                     const char *location_name,
                     bool *is_stop,
                     void (*callback)(const char *))
{
    assert(db != NULL);
    db_lock(db);
    //    printf ("load location: %s\n", location_name);

    DatabaseLocation *location = db_location_build_tree(location_name, db->db_config, is_stop, callback);

    if (location) {
        //        printf ("location num entries: %d\n", location->num_items);
        db->locations = g_list_append(db->locations, location);
        db->num_entries += location->num_items;
        db_update_timestamp(db);
        db_unlock(db);
        return true;
    }

    db_update_timestamp(db);
    db_unlock(db);
    return false;
}

void db_update_sort_index(Database *db)
{
    assert(db != NULL);
    assert(db->entries != NULL);

    for (uint32_t i = 0; i < db->num_entries; ++i) {
        BTreeNode *node = darray_get_item(db->entries, i);
        if (node) {
            node->pos = i;
        }
    }
}

static uint32_t
db_locations_get_num_entries(Database *db)
{
    assert(db != NULL);
    assert(db->locations != NULL);

    uint32_t num_entries = 0;
    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        DatabaseLocation *location = l->data;
        num_entries += location->num_items;
    }
    return num_entries;
}

void db_build_initial_entries_list(Database *db)
{
    assert(db != NULL);
    assert(db->num_entries >= 0);

    db_lock(db);
    db_entries_clear(db);
    uint32_t num_entries = db_locations_get_num_entries(db);
    db->entries = darray_new(num_entries);

    GList *locations = db->locations;
    temp_index = 0;
    for (GList *l = locations; l != NULL; l = l->next) {
        db_list_add_location(db, l->data);
    }
    db_sort(db);
    db_update_sort_index(db);
    db_unlock(db);
}

void db_update_entries_list(Database *db)
{
    assert(db != NULL);
    assert(db->num_entries >= 0);

    db_lock(db);
    db_entries_clear(db);
    uint32_t num_entries = db_locations_get_num_entries(db);
    //    trace ("update list: %d\n", num_entries);
    db->entries = darray_new(num_entries);

    GList *locations = db->locations;
    for (GList *l = locations; l != NULL; l = l->next) {
        db_list_insert_location(db, l->data);
    }
    db_unlock(db);
}

BTreeNode *
db_location_get_entries(DatabaseLocation *location)
{
    assert(location != NULL);
    return location->entries;
}

Database *
db_new()
{
    Database *db = g_new0(Database, 1);
    db->db_config = g_new0(DatabaseConfig, 1);
    g_mutex_init(&db->mutex);
    return db;
}

static void
db_entries_clear(Database *db)
{
    // free entries
    assert(db != NULL);

    if (db->entries) {
        darray_free(db->entries);
        db->entries = NULL;
    }
    db->num_entries = 0;
}

void db_free(Database *db)
{
    assert(db != NULL);

    db_entries_clear(db);
    g_mutex_clear(&db->mutex);
    g_free(db->db_config);
    g_free(db);
    db = NULL;
    return;
}

time_t
db_get_timestamp(Database *db)
{
    assert(db != NULL);
    return db->timestamp;
}

uint32_t
db_get_num_entries(Database *db)
{
    assert(db != NULL);
    return db->num_entries;
}

void db_unlock(Database *db)
{
    assert(db != NULL);
    g_mutex_unlock(&db->mutex);
}

void db_lock(Database *db)
{
    assert(db != NULL);
    g_mutex_lock(&db->mutex);
}

bool db_try_lock(Database *db)
{
    assert(db != NULL);
    return g_mutex_trylock(&db->mutex);
}

DynamicArray *
db_get_entries(Database *db)
{
    assert(db != NULL);
    return db->entries;
}

static int
sort_by_name(const void *a, const void *b)
{
    BTreeNode *node_a = *(BTreeNode **)a;
    BTreeNode *node_b = *(BTreeNode **)b;

    if (!node_a) {
        return -1;
    }
    if (!node_b) {
        return 1;
    }

    const bool is_dir_a = node_a->is_dir;
    const bool is_dir_b = node_b->is_dir;
    if (is_dir_a != is_dir_b) {
        return is_dir_a ? -1 : 1;
    }

    return strverscmp(node_a->name, node_b->name);
}

void db_sort(Database *db)
{
    assert(db != NULL);
    assert(db->entries != NULL);

    //    trace ("start sorting\n");
    darray_sort(db->entries, sort_by_name);
    //    trace ("finished sorting\n");
}

static void
db_location_free_all(Database *db)
{
    assert(db != NULL);
    g_return_if_fail(db->locations != NULL);

    GList *l = db->locations;
    while (l) {
        //        trace ("free location\n");
        db_location_free(l->data);
        l = l->next;
    }
    g_list_free(db->locations);
    db->locations = NULL;
}

bool db_clear(Database *db)
{
    assert(db != NULL);

    //    trace ("clear locations\n");
    db_entries_clear(db);
    db_location_free_all(db);
    return true;
}

bool db_support(const char *search_path, bool has_data_prefix)
{
    if (has_data_prefix)
        return true;

    GList *info = get_fstable_bindinfo();
    for (; info != NULL; info = g_list_next(info)) {
        char *data = info->data;
        if (strncmp(data, search_path, strlen(data)) == 0)
            return false;
    }

    regex_t reg;
    regmatch_t pmatch[1];

    regcomp(&reg, DATABASE_FILTER_PATH, REG_EXTENDED);
    if (regexec(&reg, search_path, 1, pmatch, 0) == 0) {
        regfree(&reg);
        return false;
    }

    regfree(&reg);
    return true;
}
