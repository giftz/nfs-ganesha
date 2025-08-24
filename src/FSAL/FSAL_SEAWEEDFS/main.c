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

#include <libgen.h>		/* used for 'dirname' */
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
	.lookup_path = seaweed_handle_lookup,
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
	.mkdir = seaweed_handle_mkdir
	/* Additional methods will be added as implementation progresses */
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
	/* TODO: Implement lookup functionality */
	LogDebug(COMPONENT_FSAL, "SeaweedFS handle lookup: %s", path);
	return fsalstat(ERR_FSAL_NOTSUPP, ENOSYS);
}

static fsal_status_t seaweed_handle_readdir(struct fsal_obj_handle *dir_hdl,
					    fsal_cookie_t *whence,
					    void *dir_state,
					    fsal_readdir_cb cb,
					    attrmask_t attrmask,
					    bool *eof)
{
	/* TODO: Implement readdir functionality */
	LogDebug(COMPONENT_FSAL, "SeaweedFS handle readdir");
	return fsalstat(ERR_FSAL_NOTSUPP, ENOSYS);
}

static fsal_status_t seaweed_handle_create(struct fsal_obj_handle *dir_hdl,
					   const char *name,
					   struct fsal_attrlist *attrib,
					   struct fsal_obj_handle **handle,
					   struct fsal_attrlist *attrs_out)
{
	/* TODO: Implement create functionality */
	LogDebug(COMPONENT_FSAL, "SeaweedFS handle create: %s", name);
	return fsalstat(ERR_FSAL_NOTSUPP, ENOSYS);
}

static fsal_status_t seaweed_handle_mkdir(struct fsal_obj_handle *dir_hdl,
					  const char *name,
					  struct fsal_attrlist *attrib,
					  struct fsal_obj_handle **handle,
					  struct fsal_attrlist *attrs_out)
{
	/* TODO: Implement mkdir functionality */
	LogDebug(COMPONENT_FSAL, "SeaweedFS handle mkdir: %s", name);
	return fsalstat(ERR_FSAL_NOTSUPP, ENOSYS);
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