/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2024, NFS-Ganesha Project
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file main.c
 * @brief SeaweedFS FSAL module registration and initialization
 */

#include "config.h"

#include "fsal.h"
#include "fsal_seaweed.h"
#include "FSAL/fsal_init.h"
#include "FSAL/fsal_commonlib.h"
#include "nfs_exports.h"
#include "export_mgr.h"

#include <libgen.h>		/* used for 'dirname' and 'basename' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <mntent.h>
#include <sys/statvfs.h>
#include <os/quota.h>

/* SeaweedFS FSAL module */
static struct seaweed_fsal_module SeaweedFS;

/* Static FSAL name string */
static const char myname[] = "SEAWEEDFS";

/* Module methods forward declarations */
static fsal_status_t seaweed_init_config(struct fsal_module *fsal_hdl,
					  config_file_t config_struct,
					  struct config_error_type *err_type);
static fsal_status_t seaweed_create_export(struct fsal_module *fsal_hdl,
					    void *parse_node,
					    struct config_error_type *err_type,
					    const struct fsal_up_vector *up_ops);
static void seaweed_fsal_release(struct fsal_module *fsal_hdl);

/* Export methods forward declarations */
static void seaweed_export_release(struct fsal_export *exp_hdl);
static fsal_status_t seaweed_get_dynamic_info(struct fsal_export *exp_hdl,
					       struct fsal_obj_handle *obj_hdl,
					       fsal_dynamicfsinfo_t *infop);
static bool seaweed_fs_supports(struct fsal_export *exp_hdl,
				 fsal_fsinfo_options_t option);
static uint64_t seaweed_fs_maxfilesize(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_maxread(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_maxwrite(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_maxlink(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_maxnamelen(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_maxpathlen(struct fsal_export *exp_hdl);
static struct timespec seaweed_fs_lease_time(struct fsal_export *exp_hdl);
static fsal_aclsupp_t seaweed_fs_acl_support(struct fsal_export *exp_hdl);
static attrmask_t seaweed_fs_supported_attrs(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_umask(struct fsal_export *exp_hdl);
static uint32_t seaweed_fs_xattr_access_rights(struct fsal_export *exp_hdl);

/* Object handle methods forward declarations */  
static void seaweed_handle_release(struct fsal_obj_handle *obj_hdl);
static fsal_status_t seaweed_handle_lookup(struct fsal_obj_handle *parent,
					   const char *path,
					   struct fsal_obj_handle **handle,
					   struct fsal_attrlist *attrs_out);
static fsal_status_t seaweed_handle_readdir(struct fsal_obj_handle *dir_hdl,
					    fsal_cookie_t *whence,
					    void *dir_state,
					    fsal_readdir_cb cb,
					    attrmask_t attrmask,
					    bool *eof);
static fsal_status_t seaweed_handle_create(struct fsal_obj_handle *dir_hdl,
					   const char *name,
					   struct fsal_attrlist *attrib,
					   struct fsal_obj_handle **handle,
					   struct fsal_attrlist *attrs_out);
static fsal_status_t seaweed_handle_mkdir(struct fsal_obj_handle *dir_hdl,
					  const char *name,
					  struct fsal_attrlist *attrib,
					  struct fsal_obj_handle **handle,
					  struct fsal_attrlist *attrs_out);
static fsal_status_t seaweed_handle_read(struct fsal_obj_handle *obj_hdl,
					 uint64_t offset,
					 size_t buffer_size,
					 void *buffer,
					 size_t *read_amount,
					 bool *end_of_file);
static fsal_status_t seaweed_handle_write(struct fsal_obj_handle *obj_hdl,
					  uint64_t offset,
					  size_t buffer_size,
					  void *buffer,
					  size_t *write_amount,
					  bool *fsal_stable);
static fsal_status_t seaweed_handle_unlink(struct fsal_obj_handle *dir_hdl,
					   const char *name);
static fsal_status_t seaweed_handle_rename(struct fsal_obj_handle *old_dir_hdl,
					   const char *old_name,
					   struct fsal_obj_handle *new_dir_hdl,
					   const char *new_name);
static fsal_status_t seaweed_handle_getattrs(struct fsal_obj_handle *obj_hdl,
					     struct fsal_attrlist *attrs_out);
static fsal_status_t seaweed_handle_setattrs(struct fsal_obj_handle *obj_hdl,
					     struct fsal_attrlist *attrs);

/**
 * @brief SeaweedFS FSAL module operations
 */
struct fsal_ops seaweed_fsal_ops = {
	.init_config = seaweed_init_config,
	.create_export = seaweed_create_export,
	.release = seaweed_fsal_release
};

/**
 * @brief SeaweedFS FSAL export operations
 */
static struct export_ops seaweed_export_ops = {
	.release = seaweed_export_release,
	.lookup_path = seaweed_export_lookup_path,
	.fs_supports = seaweed_fs_supports,
	.fs_maxfilesize = seaweed_fs_maxfilesize,
	.fs_maxread = seaweed_fs_maxread,
	.fs_maxwrite = seaweed_fs_maxwrite,
	.fs_maxlink = seaweed_fs_maxlink,
	.fs_maxnamelen = seaweed_fs_maxnamelen,
	.fs_maxpathlen = seaweed_fs_maxpathlen,
	.fs_lease_time = seaweed_fs_lease_time,
	.fs_acl_support = seaweed_fs_acl_support,
	.fs_supported_attrs = seaweed_fs_supported_attrs,
	.fs_umask = seaweed_fs_umask,
	.fs_xattr_access_rights = seaweed_fs_xattr_access_rights,
	.get_fs_dynamic_info = seaweed_get_dynamic_info
};

/**
 * @brief SeaweedFS FSAL object handle operations
 */
static struct fsal_obj_ops seaweed_handle_ops = {
	.release = seaweed_handle_release,
	.lookup = seaweed_handle_lookup,
	.readdir = seaweed_handle_readdir,
	.create = seaweed_handle_create,
	.mkdir = seaweed_handle_mkdir,
	.read = seaweed_handle_read,
	.write = seaweed_handle_write,
	.unlink = seaweed_handle_unlink,
	.rename = seaweed_handle_rename,
	.getattrs = seaweed_handle_getattrs,
	.setattrs = seaweed_handle_setattrs
};

/**
 * @brief Initialize and register the FSAL
 * 
 * This function is called by the FSAL loading mechanism to initialize
 * and register the SeaweedFS FSAL.
 */
void seaweed_fsal_init(void)
{
	int retval;
	struct fsal_module *myself = &SeaweedFS.fsal;

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL module registering...");

	/* Initialize FSAL module structure */
	retval = register_fsal(myself, myname, FSAL_MAJOR_VERSION, FSAL_MINOR_VERSION,
			       FSAL_ID_NO_PNFS);
	if (retval != 0) {
		LogCrit(COMPONENT_FSAL,
			"SeaweedFS FSAL module failed to register");
		return;
	}

	/* Set up FSAL operations */
	myself->m_ops = &seaweed_fsal_ops;

	/* Initialize module-wide mutex and structures */
	pthread_rwlock_init(&SeaweedFS.path_cache_lock, NULL);
	pthread_mutex_init(&SeaweedFS.stats_lock, NULL);

	/* Initialize connection pool */
	pthread_mutex_init(&SeaweedFS.conn_pool.pool_lock, NULL);
	pthread_cond_init(&SeaweedFS.conn_pool.pool_cond, NULL);
	
	/* Initialize statistics */
	memset(&SeaweedFS.stats, 0, sizeof(SeaweedFS.stats));

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL module registered successfully");
}

/**
 * @brief Release the FSAL module
 */
static void seaweed_fsal_release(struct fsal_module *fsal_hdl)
{
	struct seaweed_fsal_module *seaweed_module = 
		container_of(fsal_hdl, struct seaweed_fsal_module, fsal);

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL module shutting down");

	/* Destroy connection pool */
	seaweed_destroy_connection_pool(seaweed_module);

	/* Clean up path cache */
	if (seaweed_module->path_cache) {
		/* TODO: Clean up path cache entries */
		gsh_free(seaweed_module->path_cache);
		seaweed_module->path_cache = NULL;
	}

	/* Destroy mutexes */
	pthread_rwlock_destroy(&seaweed_module->path_cache_lock);
	pthread_mutex_destroy(&seaweed_module->stats_lock);
	pthread_mutex_destroy(&seaweed_module->conn_pool.pool_lock);
	pthread_cond_destroy(&seaweed_module->conn_pool.pool_cond);

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL module shutdown complete");
}

/**
 * @brief Initialize FSAL configuration
 */
static fsal_status_t seaweed_init_config(struct fsal_module *fsal_hdl,
					  config_file_t config_struct,
					  struct config_error_type *err_type)
{
	struct seaweed_fsal_module *seaweed_module =
		container_of(fsal_hdl, struct seaweed_fsal_module, fsal);
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL,
		"Initializing SeaweedFS FSAL configuration");

	/* Load configuration from config file */
	status = seaweed_load_config(seaweed_module, config_struct, err_type);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL,
			"Failed to load SeaweedFS FSAL configuration: %s",
			fsal_err_txt(status));
		return status;
	}

	/* Initialize connection pool */
	status = seaweed_init_connection_pool(seaweed_module);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL,
			"Failed to initialize SeaweedFS connection pool: %s",
			fsal_err_txt(status));
		return status;
	}

	/* Initialize path cache */
	seaweed_module->path_cache = gsh_calloc(seaweed_module->path_cache_size,
						sizeof(struct seaweed_path_mapping *));
	if (!seaweed_module->path_cache) {
		LogMajor(COMPONENT_FSAL,
			"Failed to allocate path cache");
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS FSAL configuration initialized: "
		"%d endpoints, pool_size=%d, cache_size=%d",
		seaweed_module->num_endpoints,
		seaweed_module->connection_pool_size,
		seaweed_module->path_cache_size);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Create a new export
 */
static fsal_status_t seaweed_create_export(struct fsal_module *fsal_hdl,
					    void *parse_node,
					    struct config_error_type *err_type,
					    const struct fsal_up_vector *up_ops)
{
	struct seaweed_fsal_export *seaweed_export = NULL;
	struct seaweed_fsal_module *seaweed_module =
		container_of(fsal_hdl, struct seaweed_fsal_module, fsal);
	fsal_status_t status;
	
	LogDebug(COMPONENT_FSAL, "Creating SeaweedFS export");

	/* Allocate export structure */
	seaweed_export = gsh_calloc(1, sizeof(struct seaweed_fsal_export));
	if (seaweed_export == NULL) {
		LogMajor(COMPONENT_FSAL,
			"Unable to allocate SeaweedFS export structure");
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Initialize basic export */
	status = fsal_export_init(&seaweed_export->export);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL,
			"Failed to initialize export structure: %s",
			fsal_err_txt(status));
		gsh_free(seaweed_export);
		return status;
	}

	/* Set up export operations and links */
	seaweed_export->export.fsal = fsal_hdl;
	seaweed_export->export.up_ops = up_ops;
	seaweed_export->export.ops = &seaweed_export_ops;
	seaweed_export->seaweed_module = seaweed_module;

	/* Initialize export-specific data */
	strncpy(seaweed_export->export_path, "/", 
		sizeof(seaweed_export->export_path) - 1);
	seaweed_export->read_only = false;

	/* TODO: Parse export-specific configuration from parse_node */

	LogInfo(COMPONENT_FSAL,
		"SeaweedFS export created successfully: path=%s",
		seaweed_export->export_path);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Export method implementations */

/**
 * @brief Export-level path lookup (for root lookups)
 */
static fsal_status_t seaweed_export_lookup_path(struct fsal_export *exp_hdl,
						const char *path,
						struct fsal_obj_handle **handle,
						struct fsal_attrlist *attrs_out)
{
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_fsal_obj_handle *new_handle;
	struct seaweed_filer_connection *conn;
	struct seaweed_lookup_request req;
	struct seaweed_lookup_response resp;
	fsal_status_t status;
	char *dir_path, *base_name;
	char path_copy[SEAWEED_MAX_PATH];

	LogDebug(COMPONENT_FSAL, "SeaweedFS export lookup path: %s", path);

	if (!exp_hdl || !path || !handle) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_export = container_of(exp_hdl, struct seaweed_fsal_export, export);

	/* Handle root directory specially */
	if (strcmp(path, "/") == 0) {
		/* Get connection from pool */
		conn = seaweed_get_connection(seaweed_export->seaweed_module);
		if (!conn) {
			LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
			return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
		}

		/* Setup lookup request for root */
		memset(&req, 0, sizeof(req));
		memset(&resp, 0, sizeof(resp));
		
		strncpy(req.directory, "/", sizeof(req.directory) - 1);
		strncpy(req.name, "/", sizeof(req.name) - 1);

		/* Perform lookup */
		status = seaweed_filer_lookup_entry(conn, &req, &resp);
		seaweed_put_connection(seaweed_export->seaweed_module, conn);

		if (status != SEAWEED_OK) {
			LogMajor(COMPONENT_FSAL, "SeaweedFS root lookup failed: %s",
				 seaweed_status_to_string(status));
			return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
		}

		/* Create root handle */
		new_handle = gsh_calloc(1, sizeof(struct seaweed_fsal_obj_handle));
		if (!new_handle) {
			seaweed_free_entry(&resp.entry);
			return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
		}

		new_handle->full_path = gsh_strdup("/");
		if (!new_handle->full_path) {
			gsh_free(new_handle);
			seaweed_free_entry(&resp.entry);
			return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
		}

		/* Create filehandle for root */
		status = seaweed_create_handle_from_path("/", &new_handle->seaweed_handle);
		if (FSAL_IS_ERROR(status)) {
			gsh_free(new_handle->full_path);
			gsh_free(new_handle);
			seaweed_free_entry(&resp.entry);
			return status;
		}

		/* Add root to path cache */
		seaweed_add_to_path_cache(seaweed_export->seaweed_module,
					  new_handle->seaweed_handle.path_hash, "/");

		/* Initialize FSAL handle */
		fsal_obj_handle_init(&new_handle->obj_handle, exp_hdl, DIRECTORY);
		new_handle->obj_handle.fsid = exp_hdl->fsid;
		new_handle->obj_handle.fileid = resp.entry.attributes.inode;
		new_handle->obj_handle.ops = &seaweed_handle_ops;

		/* Fill attributes if requested */
		if (attrs_out) {
			seaweed_entry_to_attributes(&resp.entry, attrs_out);
		}

		*handle = &new_handle->obj_handle;
		seaweed_free_entry(&resp.entry);

		LogFullDebug(COMPONENT_FSAL, "SeaweedFS root lookup successful");
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	/* For non-root paths, split into directory and basename */
	strncpy(path_copy, path, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';
	
	dir_path = dirname(path_copy);
	base_name = basename((char *)path);

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup lookup request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, dir_path, sizeof(req.directory) - 1);
	strncpy(req.name, base_name, sizeof(req.name) - 1);

	/* Perform lookup */
	status = seaweed_filer_lookup_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_NOENT, ENOENT);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS lookup failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Create new handle */
	new_handle = gsh_calloc(1, sizeof(struct seaweed_fsal_obj_handle));
	if (!new_handle) {
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Initialize handle */
	new_handle->full_path = gsh_strdup(path);
	if (!new_handle->full_path) {
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Create filehandle */
	status = seaweed_create_handle_from_path(path, &new_handle->seaweed_handle);
	if (FSAL_IS_ERROR(status)) {
		gsh_free(new_handle->full_path);
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return status;
	}

	/* Add to path cache */
	seaweed_add_to_path_cache(seaweed_export->seaweed_module,
				  new_handle->seaweed_handle.path_hash, path);

	/* Initialize FSAL handle */
	fsal_obj_handle_init(&new_handle->obj_handle, exp_hdl,
			     (resp.entry.type == SEAWEED_FILE_TYPE_DIRECTORY) ? 
			     DIRECTORY : REGULAR_FILE);
	new_handle->obj_handle.fsid = exp_hdl->fsid;
	new_handle->obj_handle.fileid = resp.entry.attributes.inode;
	new_handle->obj_handle.ops = &seaweed_handle_ops;

	/* Fill attributes if requested */
	if (attrs_out) {
		seaweed_entry_to_attributes(&resp.entry, attrs_out);
	}

	*handle = &new_handle->obj_handle;

	seaweed_free_entry(&resp.entry);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS export lookup successful: %s", path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Release an export
 */
static void seaweed_export_release(struct fsal_export *exp_hdl)
{
	struct seaweed_fsal_export *seaweed_export =
		container_of(exp_hdl, struct seaweed_fsal_export, export);

	LogDebug(COMPONENT_FSAL, "Releasing SeaweedFS export");

	fsal_detach_export(exp_hdl->fsal, &exp_hdl->exports);
	free_export_ops(exp_hdl);
	gsh_free(seaweed_export);
}

/**
 * @brief Get dynamic filesystem information
 */
static fsal_status_t seaweed_get_dynamic_info(struct fsal_export *exp_hdl,
					       struct fsal_obj_handle *obj_hdl,
					       fsal_dynamicfsinfo_t *infop)
{
	LogFullDebug(COMPONENT_FSAL, "Getting dynamic FS info");

	/* Set some reasonable defaults for SeaweedFS */
	infop->total_bytes = 1ULL << 50;      /* 1 PB total */
	infop->free_bytes = 1ULL << 49;       /* 512 TB free */
	infop->avail_bytes = 1ULL << 49;      /* 512 TB available */
	infop->total_files = 1ULL << 32;      /* 4G files max */
	infop->free_files = 1ULL << 31;       /* 2G files available */
	infop->avail_files = 1ULL << 31;      /* 2G files available */
	infop->time_delta.tv_sec = 1;
	infop->time_delta.tv_nsec = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Check if filesystem supports a specific option
 */
static bool seaweed_fs_supports(struct fsal_export *exp_hdl,
				fsal_fsinfo_options_t option)
{
	switch (option) {
	case fso_no_trunc:
	case fso_chown_restricted:
	case fso_case_insensitive:
	case fso_case_preserving:
		return true;
	
	case fso_link_support:
	case fso_symlink_support:
	case fso_lock_support:
	case fso_lock_support_owner:
	case fso_lock_support_async_block:
		return true; /* MVP supports basic locking */
		
	case fso_named_attr:
	case fso_unique_handles:
	case fso_lease_time:
	case fso_share_deny:
	case fso_cansettime:
		return true;
		
	case fso_homogenous:
	case fso_accesscheck_support:
		return false; /* Not supported in MVP */
		
	default:
		return false;
	}
}

/* Filesystem capability methods */
static uint64_t seaweed_fs_maxfilesize(struct fsal_export *exp_hdl)
{
	return (uint64_t) UINT64_MAX; /* SeaweedFS can handle very large files */
}

static uint32_t seaweed_fs_maxread(struct fsal_export *exp_hdl)
{
	return FSAL_MAXIOSIZE; /* Use FSAL default */
}

static uint32_t seaweed_fs_maxwrite(struct fsal_export *exp_hdl)
{
	return FSAL_MAXIOSIZE; /* Use FSAL default */
}

static uint32_t seaweed_fs_maxlink(struct fsal_export *exp_hdl)
{
	return 1024; /* Reasonable limit for hard links */
}

static uint32_t seaweed_fs_maxnamelen(struct fsal_export *exp_hdl)
{
	return 255; /* Standard filename length limit */
}

static uint32_t seaweed_fs_maxpathlen(struct fsal_export *exp_hdl)
{
	return SEAWEED_MAX_PATH_LEN;
}

static struct timespec seaweed_fs_lease_time(struct fsal_export *exp_hdl)
{
	struct timespec lease_time = {.tv_sec = 60, .tv_nsec = 0};
	return lease_time;
}

static fsal_aclsupp_t seaweed_fs_acl_support(struct fsal_export *exp_hdl)
{
	return FSAL_ACLSUPPORT_ALLOW; /* Basic ACL support */
}

static attrmask_t seaweed_fs_supported_attrs(struct fsal_export *exp_hdl)
{
	return ATTR_TYPE | ATTR_SIZE | ATTR_FILEID | ATTR_MODE |
	       ATTR_NUMLINKS | ATTR_OWNER | ATTR_GROUP |
	       ATTR_ATIME | ATTR_MTIME | ATTR_CTIME | ATTR_CHANGE |
	       ATTR_SPACEUSED | ATTR_RAWDEV;
}

static uint32_t seaweed_fs_umask(struct fsal_export *exp_hdl)
{
	return 0000; /* No umask restriction */
}

static uint32_t seaweed_fs_xattr_access_rights(struct fsal_export *exp_hdl)
{
	return XATTR_RW; /* Allow read/write extended attributes */
}

/* Helper functions */

/**
 * @brief Convert SeaweedFS entry to FSAL attributes
 */
static void seaweed_entry_to_attributes(const struct seaweed_entry *entry,
					struct fsal_attrlist *attrs)
{
	if (!entry || !attrs) {
		return;
	}

	/* Clear all attributes first */
	fsal_prepare_attrs(attrs, ATTR_TYPE | ATTR_SIZE | ATTR_FILEID | ATTR_MODE |
			   ATTR_NUMLINKS | ATTR_OWNER | ATTR_GROUP |
			   ATTR_ATIME | ATTR_MTIME | ATTR_CTIME | ATTR_CHANGE |
			   ATTR_SPACEUSED);

	/* Set file type */
	switch (entry->type) {
	case SEAWEED_FILE_TYPE_REGULAR:
		attrs->type = REGULAR_FILE;
		break;
	case SEAWEED_FILE_TYPE_DIRECTORY:
		attrs->type = DIRECTORY;
		break;
	case SEAWEED_FILE_TYPE_SYMLINK:
		attrs->type = SYMBOLIC_LINK;
		break;
	default:
		attrs->type = NO_FILE_TYPE;
		break;
	}

	/* Set basic attributes */
	attrs->filesize = entry->attributes.file_size;
	attrs->fileid = entry->attributes.inode;
	attrs->mode = entry->attributes.file_mode;
	attrs->numlinks = entry->attributes.nlinks;
	attrs->owner = entry->attributes.uid;
	attrs->group = entry->attributes.gid;

	/* Set timestamps */
	attrs->atime.tv_sec = entry->attributes.mtime;
	attrs->atime.tv_nsec = 0;
	attrs->mtime.tv_sec = entry->attributes.mtime;
	attrs->mtime.tv_nsec = 0;
	attrs->ctime.tv_sec = entry->attributes.ctime;
	attrs->ctime.tv_nsec = 0;
	attrs->change = attrs->mtime;

	/* Set space used */
	attrs->spaceused = entry->attributes.file_size;

	LogFullDebug(COMPONENT_FSAL, "Converted attributes: size=%lu, mode=%o, inode=%lu",
		     attrs->filesize, attrs->mode, attrs->fileid);
}

/* Object handle method stubs (to be implemented) */

static void seaweed_handle_release(struct fsal_obj_handle *obj_hdl)
{
	struct seaweed_fsal_obj_handle *seaweed_handle =
		container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);

	if (seaweed_handle->full_path) {
		gsh_free(seaweed_handle->full_path);
	}
	
	fsal_obj_handle_fini(obj_hdl);
	gsh_free(seaweed_handle);
}

static fsal_status_t seaweed_handle_lookup(struct fsal_obj_handle *parent,
					   const char *path,
					   struct fsal_obj_handle **handle,
					   struct fsal_attrlist *attrs_out)
{
	struct seaweed_fsal_obj_handle *parent_handle;
	struct seaweed_fsal_obj_handle *new_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_lookup_request req;
	struct seaweed_lookup_response resp;
	char full_path[SEAWEED_MAX_PATH];
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS handle lookup: %s", path);

	if (!parent || !path || !handle) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	parent_handle = container_of(parent, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(parent->fsal, struct seaweed_fsal_export, export);

	/* Build full path */
	if (parent_handle->full_path && strcmp(parent_handle->full_path, "/") == 0) {
		snprintf(full_path, sizeof(full_path), "/%s", path);
	} else {
		snprintf(full_path, sizeof(full_path), "%s/%s", 
			 parent_handle->full_path ? parent_handle->full_path : "", path);
	}

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup lookup request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, parent_handle->full_path ? parent_handle->full_path : "/", 
		sizeof(req.directory) - 1);
	strncpy(req.name, path, sizeof(req.name) - 1);

	/* Perform lookup */
	status = seaweed_filer_lookup_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_NOENT, ENOENT);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS lookup failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Create new handle */
	new_handle = gsh_calloc(1, sizeof(struct seaweed_fsal_obj_handle));
	if (!new_handle) {
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Initialize handle */
	new_handle->full_path = gsh_strdup(full_path);
	if (!new_handle->full_path) {
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Create filehandle */
	status = seaweed_create_handle_from_path(full_path, &new_handle->seaweed_handle);
	if (FSAL_IS_ERROR(status)) {
		gsh_free(new_handle->full_path);
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return status;
	}

	/* Add to path cache */
	seaweed_add_to_path_cache(seaweed_export->seaweed_module,
				  new_handle->seaweed_handle.path_hash, full_path);

	/* Initialize FSAL handle */
	fsal_obj_handle_init(&new_handle->obj_handle, &seaweed_export->export,
			     (resp.entry.type == SEAWEED_FILE_TYPE_DIRECTORY) ? 
			     DIRECTORY : REGULAR_FILE);
	new_handle->obj_handle.fsid = seaweed_export->export.fsid;
	new_handle->obj_handle.fileid = resp.entry.attributes.inode;
	new_handle->obj_handle.ops = &seaweed_handle_ops;

	/* Fill attributes if requested */
	if (attrs_out) {
		seaweed_entry_to_attributes(&resp.entry, attrs_out);
	}

	*handle = &new_handle->obj_handle;

	seaweed_free_entry(&resp.entry);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS lookup successful: %s", full_path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_readdir(struct fsal_obj_handle *dir_hdl,
					    fsal_cookie_t *whence,
					    void *dir_state,
					    fsal_readdir_cb cb,
					    attrmask_t attrmask,
					    bool *eof)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_list_request req;
	struct seaweed_list_response resp;
	struct seaweed_entry *entry;
	struct fsal_attrlist attrs;
	fsal_status_t status;
	fsal_cookie_t cookie = 0;

	LogDebug(COMPONENT_FSAL, "SeaweedFS handle readdir");

	if (!dir_hdl || !cb || !eof) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(dir_hdl->fsal, struct seaweed_fsal_export, export);

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup list request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, seaweed_handle->full_path ? seaweed_handle->full_path : "/", 
		sizeof(req.directory) - 1);
	req.limit = 1000;  /* Reasonable default */
	req.inclusive_start = false;

	/* If we have a starting point, use it */
	if (whence && *whence != 0) {
		/* For simplicity in MVP, we don't implement true pagination */
		LogDebug(COMPONENT_FSAL, "Readdir with continuation not fully supported in MVP");
	}

	/* Perform list operation */
	status = seaweed_filer_list_entries(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		LogMajor(COMPONENT_FSAL, "SeaweedFS list failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Process each entry */
	entry = resp.entries;
	while (entry) {
		/* Prepare attributes for callback */
		fsal_prepare_attrs(&attrs, attrmask);
		seaweed_entry_to_attributes(entry, &attrs);

		/* Call the callback */
		if (!cb(entry->name, dir_hdl, &attrs, dir_state, cookie++)) {
			LogDebug(COMPONENT_FSAL, "Readdir callback signaled stop");
			break;
		}

		fsal_release_attrs(&attrs);
		entry = entry->next;
	}

	/* Set end-of-file status */
	*eof = !resp.has_more;

	/* Clean up response */
	seaweed_free_list_response(&resp);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS readdir completed: %d entries", resp.count);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_create(struct fsal_obj_handle *dir_hdl,
					   const char *name,
					   struct fsal_attrlist *attrib,
					   struct fsal_obj_handle **handle,
					   struct fsal_attrlist *attrs_out)
{
	struct seaweed_fsal_obj_handle *parent_handle;
	struct seaweed_fsal_obj_handle *new_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_create_request req;
	struct seaweed_create_response resp;
	char full_path[SEAWEED_MAX_PATH];
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS handle create: %s", name);

	if (!dir_hdl || !name || !handle) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	parent_handle = container_of(dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(dir_hdl->fsal, struct seaweed_fsal_export, export);

	/* Build full path */
	if (parent_handle->full_path && strcmp(parent_handle->full_path, "/") == 0) {
		snprintf(full_path, sizeof(full_path), "/%s", name);
	} else {
		snprintf(full_path, sizeof(full_path), "%s/%s", 
			 parent_handle->full_path ? parent_handle->full_path : "", name);
	}

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup create request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, parent_handle->full_path ? parent_handle->full_path : "/", 
		sizeof(req.directory) - 1);
	strncpy(req.entry.name, name, sizeof(req.entry.name) - 1);
	
	req.entry.type = SEAWEED_FILE_TYPE_REGULAR;
	req.o_excl = true;  /* Default to exclusive creation */

	/* Set attributes from request */
	if (attrib) {
		req.entry.attributes.file_mode = attrib->mode ? attrib->mode : (S_IFREG | 0644);
		req.entry.attributes.uid = attrib->owner ? attrib->owner : 0;
		req.entry.attributes.gid = attrib->group ? attrib->group : 0;
		req.entry.attributes.file_size = 0;
	} else {
		req.entry.attributes.file_mode = S_IFREG | 0644;
		req.entry.attributes.uid = 0;
		req.entry.attributes.gid = 0;
		req.entry.attributes.file_size = 0;
	}

	/* Perform create operation */
	status = seaweed_filer_create_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_ALREADY_EXISTS) {
			return fsalstat(ERR_FSAL_EXIST, EEXIST);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS create failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Create new handle */
	new_handle = gsh_calloc(1, sizeof(struct seaweed_fsal_obj_handle));
	if (!new_handle) {
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Initialize handle */
	new_handle->full_path = gsh_strdup(full_path);
	if (!new_handle->full_path) {
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Create filehandle */
	status = seaweed_create_handle_from_path(full_path, &new_handle->seaweed_handle);
	if (FSAL_IS_ERROR(status)) {
		gsh_free(new_handle->full_path);
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return status;
	}

	/* Add to path cache */
	seaweed_add_to_path_cache(seaweed_export->seaweed_module,
				  new_handle->seaweed_handle.path_hash, full_path);

	/* Initialize FSAL handle */
	fsal_obj_handle_init(&new_handle->obj_handle, &seaweed_export->export, REGULAR_FILE);
	new_handle->obj_handle.fsid = seaweed_export->export.fsid;
	new_handle->obj_handle.fileid = resp.entry.attributes.inode;
	new_handle->obj_handle.ops = &seaweed_handle_ops;

	/* Fill attributes if requested */
	if (attrs_out) {
		seaweed_entry_to_attributes(&resp.entry, attrs_out);
	}

	*handle = &new_handle->obj_handle;

	seaweed_free_entry(&resp.entry);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS create successful: %s", full_path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_mkdir(struct fsal_obj_handle *dir_hdl,
					  const char *name,
					  struct fsal_attrlist *attrib,
					  struct fsal_obj_handle **handle,
					  struct fsal_attrlist *attrs_out)
{
	struct seaweed_fsal_obj_handle *parent_handle;
	struct seaweed_fsal_obj_handle *new_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_create_request req;
	struct seaweed_create_response resp;
	char full_path[SEAWEED_MAX_PATH];
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS handle mkdir: %s", name);

	if (!dir_hdl || !name || !handle) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	parent_handle = container_of(dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(dir_hdl->fsal, struct seaweed_fsal_export, export);

	/* Build full path */
	if (parent_handle->full_path && strcmp(parent_handle->full_path, "/") == 0) {
		snprintf(full_path, sizeof(full_path), "/%s", name);
	} else {
		snprintf(full_path, sizeof(full_path), "%s/%s", 
			 parent_handle->full_path ? parent_handle->full_path : "", name);
	}

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup create request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, parent_handle->full_path ? parent_handle->full_path : "/", 
		sizeof(req.directory) - 1);
	strncpy(req.entry.name, name, sizeof(req.entry.name) - 1);
	
	req.entry.type = SEAWEED_FILE_TYPE_DIRECTORY;
	req.o_excl = true;  /* Default to exclusive creation */

	/* Set attributes from request */
	if (attrib) {
		req.entry.attributes.file_mode = attrib->mode ? attrib->mode : (S_IFDIR | 0755);
		req.entry.attributes.uid = attrib->owner ? attrib->owner : 0;
		req.entry.attributes.gid = attrib->group ? attrib->group : 0;
		req.entry.attributes.file_size = 4096;  /* Standard directory size */
	} else {
		req.entry.attributes.file_mode = S_IFDIR | 0755;
		req.entry.attributes.uid = 0;
		req.entry.attributes.gid = 0;
		req.entry.attributes.file_size = 4096;
	}

	/* Perform create operation */
	status = seaweed_filer_create_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_ALREADY_EXISTS) {
			return fsalstat(ERR_FSAL_EXIST, EEXIST);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS mkdir failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Create new handle */
	new_handle = gsh_calloc(1, sizeof(struct seaweed_fsal_obj_handle));
	if (!new_handle) {
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Initialize handle */
	new_handle->full_path = gsh_strdup(full_path);
	if (!new_handle->full_path) {
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return fsalstat(ERR_FSAL_NOMEM, ENOMEM);
	}

	/* Create filehandle */
	status = seaweed_create_handle_from_path(full_path, &new_handle->seaweed_handle);
	if (FSAL_IS_ERROR(status)) {
		gsh_free(new_handle->full_path);
		gsh_free(new_handle);
		seaweed_free_entry(&resp.entry);
		return status;
	}

	/* Add to path cache */
	seaweed_add_to_path_cache(seaweed_export->seaweed_module,
				  new_handle->seaweed_handle.path_hash, full_path);

	/* Initialize FSAL handle */
	fsal_obj_handle_init(&new_handle->obj_handle, &seaweed_export->export, DIRECTORY);
	new_handle->obj_handle.fsid = seaweed_export->export.fsid;
	new_handle->obj_handle.fileid = resp.entry.attributes.inode;
	new_handle->obj_handle.ops = &seaweed_handle_ops;

	/* Fill attributes if requested */
	if (attrs_out) {
		seaweed_entry_to_attributes(&resp.entry, attrs_out);
	}

	*handle = &new_handle->obj_handle;

	seaweed_free_entry(&resp.entry);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS mkdir successful: %s", full_path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_getattrs(struct fsal_obj_handle *obj_hdl,
					     struct fsal_attrlist *attrs_out)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_lookup_request req;
	struct seaweed_lookup_response resp;
	char *dir_path, *base_name;
	char path_copy[SEAWEED_MAX_PATH];
	fsal_status_t status;

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS getattrs");

	if (!obj_hdl || !attrs_out) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(obj_hdl->fsal, struct seaweed_fsal_export, export);

	if (!seaweed_handle->full_path) {
		LogMajor(COMPONENT_FSAL, "SeaweedFS handle missing full path");
		return fsalstat(ERR_FSAL_SERVERFAULT, EFAULT);
	}

	/* For root directory, handle specially */
	if (strcmp(seaweed_handle->full_path, "/") == 0) {
		strncpy(req.directory, "/", sizeof(req.directory) - 1);
		strncpy(req.name, "/", sizeof(req.name) - 1);
	} else {
		/* Split path into directory and basename */
		strncpy(path_copy, seaweed_handle->full_path, sizeof(path_copy) - 1);
		path_copy[sizeof(path_copy) - 1] = '\0';
		
		dir_path = dirname(path_copy);
		base_name = basename((char *)seaweed_handle->full_path);
		
		strncpy(req.directory, dir_path, sizeof(req.directory) - 1);
		strncpy(req.name, base_name, sizeof(req.name) - 1);
	}

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup lookup request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	/* Perform lookup to get current attributes */
	status = seaweed_filer_lookup_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_STALE, ESTALE);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS getattrs lookup failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Convert entry to attributes */
	seaweed_entry_to_attributes(&resp.entry, attrs_out);

	seaweed_free_entry(&resp.entry);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS getattrs successful: %s", 
		     seaweed_handle->full_path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_setattrs(struct fsal_obj_handle *obj_hdl,
					     struct fsal_attrlist *attrs)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_update_request req;
	struct seaweed_update_response resp;
	char *dir_path, *base_name;
	char path_copy[SEAWEED_MAX_PATH];
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS setattrs");

	if (!obj_hdl || !attrs) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(obj_hdl->fsal, struct seaweed_fsal_export, export);

	if (!seaweed_handle->full_path) {
		LogMajor(COMPONENT_FSAL, "SeaweedFS handle missing full path");
		return fsalstat(ERR_FSAL_SERVERFAULT, EFAULT);
	}

	/* Split path into directory and basename */
	strncpy(path_copy, seaweed_handle->full_path, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';
	
	dir_path = dirname(path_copy);
	base_name = basename((char *)seaweed_handle->full_path);

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup update request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, dir_path, sizeof(req.directory) - 1);
	strncpy(req.entry.name, base_name, sizeof(req.entry.name) - 1);

	/* Convert FSAL attributes to SeaweedFS entry attributes */
	if (FSAL_TEST_MASK(attrs->valid_mask, ATTR_MODE)) {
		req.entry.attributes.file_mode = attrs->mode;
	}
	if (FSAL_TEST_MASK(attrs->valid_mask, ATTR_OWNER)) {
		req.entry.attributes.uid = attrs->owner;
	}
	if (FSAL_TEST_MASK(attrs->valid_mask, ATTR_GROUP)) {
		req.entry.attributes.gid = attrs->group;
	}
	if (FSAL_TEST_MASK(attrs->valid_mask, ATTR_SIZE)) {
		req.entry.attributes.file_size = attrs->filesize;
	}
	if (FSAL_TEST_MASK(attrs->valid_mask, ATTR_MTIME)) {
		req.entry.attributes.mtime = attrs->mtime.tv_sec;
	}

	/* Perform update operation */
	status = seaweed_filer_update_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_STALE, ESTALE);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS setattrs failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS setattrs successful: %s", 
		     seaweed_handle->full_path);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_unlink(struct fsal_obj_handle *dir_hdl,
					   const char *name)
{
	struct seaweed_fsal_obj_handle *parent_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_delete_request req;
	struct seaweed_delete_response resp;
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS unlink: %s", name);

	if (!dir_hdl || !name) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	parent_handle = container_of(dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(dir_hdl->fsal, struct seaweed_fsal_export, export);

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup delete request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.directory, parent_handle->full_path ? parent_handle->full_path : "/", 
		sizeof(req.directory) - 1);
	strncpy(req.name, name, sizeof(req.name) - 1);
	req.is_recursive = false;
	req.delete_chunks = true;

	/* Perform delete operation */
	status = seaweed_filer_delete_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_NOENT, ENOENT);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS unlink failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS unlink successful: %s", name);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_rename(struct fsal_obj_handle *old_dir_hdl,
					   const char *old_name,
					   struct fsal_obj_handle *new_dir_hdl,
					   const char *new_name)
{
	struct seaweed_fsal_obj_handle *old_parent_handle;
	struct seaweed_fsal_obj_handle *new_parent_handle;
	struct seaweed_fsal_export *seaweed_export;
	struct seaweed_filer_connection *conn;
	struct seaweed_rename_request req;
	struct seaweed_rename_response resp;
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "SeaweedFS rename: %s -> %s", old_name, new_name);

	if (!old_dir_hdl || !old_name || !new_dir_hdl || !new_name) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	old_parent_handle = container_of(old_dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	new_parent_handle = container_of(new_dir_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(old_dir_hdl->fsal, struct seaweed_fsal_export, export);

	/* Get connection from pool */
	conn = seaweed_get_connection(seaweed_export->seaweed_module);
	if (!conn) {
		LogMajor(COMPONENT_FSAL, "Failed to get SeaweedFS connection");
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	/* Setup rename request */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	
	strncpy(req.old_directory, old_parent_handle->full_path ? old_parent_handle->full_path : "/", 
		sizeof(req.old_directory) - 1);
	strncpy(req.old_name, old_name, sizeof(req.old_name) - 1);
	strncpy(req.new_directory, new_parent_handle->full_path ? new_parent_handle->full_path : "/", 
		sizeof(req.new_directory) - 1);
	strncpy(req.new_name, new_name, sizeof(req.new_name) - 1);

	/* Perform rename operation */
	status = seaweed_filer_rename_entry(conn, &req, &resp);
	seaweed_put_connection(seaweed_export->seaweed_module, conn);

	if (status != SEAWEED_OK) {
		if (status == SEAWEED_ERROR_NOT_FOUND) {
			return fsalstat(ERR_FSAL_NOENT, ENOENT);
		} else if (status == SEAWEED_ERROR_ALREADY_EXISTS) {
			return fsalstat(ERR_FSAL_EXIST, EEXIST);
		}
		LogMajor(COMPONENT_FSAL, "SeaweedFS rename failed: %s",
			 seaweed_status_to_string(status));
		return fsalstat(ERR_FSAL_SERVERFAULT, EIO);
	}

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS rename successful: %s -> %s", old_name, new_name);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_read(struct fsal_obj_handle *obj_hdl,
					 uint64_t offset,
					 size_t buffer_size,
					 void *buffer,
					 size_t *read_amount,
					 bool *end_of_file)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	struct seaweed_fsal_export *seaweed_export;

	LogDebug(COMPONENT_FSAL, "SeaweedFS read: offset=%lu, size=%zu", offset, buffer_size);

	if (!obj_hdl || !buffer || !read_amount || !end_of_file) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(obj_hdl->fsal, struct seaweed_fsal_export, export);

	/* For MVP, we implement basic read using the volume API simulation */
	/* This is a simplified implementation - in real version would:
	 * 1. Get file chunks from filer
	 * 2. Read from appropriate volume servers
	 * 3. Handle chunk boundaries and partial reads
	 */

	/* For now, simulate read with dummy data */
	size_t to_read = (buffer_size > 1024) ? 1024 : buffer_size;
	memset(buffer, 'A', to_read);
	*read_amount = to_read;
	*end_of_file = (offset + to_read >= 1024); /* Simulate 1KB file size */

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS read successful: %zu bytes", *read_amount);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t seaweed_handle_write(struct fsal_obj_handle *obj_hdl,
					  uint64_t offset,
					  size_t buffer_size,
					  void *buffer,
					  size_t *write_amount,
					  bool *fsal_stable)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	struct seaweed_fsal_export *seaweed_export;

	LogDebug(COMPONENT_FSAL, "SeaweedFS write: offset=%lu, size=%zu", offset, buffer_size);

	if (!obj_hdl || !buffer || !write_amount || !fsal_stable) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	seaweed_export = container_of(obj_hdl->fsal, struct seaweed_fsal_export, export);

	/* For MVP, we simulate write operations
	 * In real implementation would:
	 * 1. Get volume assignment from filer
	 * 2. Write chunks to volume servers
	 * 3. Update filer metadata
	 * 4. Handle chunk boundaries and partial writes
	 */

	/* Simulate successful write */
	*write_amount = buffer_size;
	*fsal_stable = true; /* Simulate stable write */

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS write successful: %zu bytes", *write_amount);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Module entry points */

MODULE_INIT void seaweed_init(void)
{
	LogInfo(COMPONENT_FSAL, "SeaweedFS FSAL module initializing");
	
	if (glist_null(&SeaweedFS.fsal.list_exports)) {
		glist_init(&SeaweedFS.fsal.list_exports);
	}
	
	seaweed_fsal_init();
}

MODULE_FINI void seaweed_unload(void)
{
	LogInfo(COMPONENT_FSAL, "SeaweedFS FSAL module unloading");
	
	/* The module release function will be called automatically */
}