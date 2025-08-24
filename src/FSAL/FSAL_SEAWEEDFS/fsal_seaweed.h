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
 * @file fsal_seaweed.h
 * @brief SeaweedFS FSAL internal header
 */

#ifndef FSAL_SEAWEED_H
#define FSAL_SEAWEED_H

#include "fsal.h"
#include "fsal_types.h"
#include "fsal_api.h"
#include "FSAL/fsal_commonlib.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

/* Forward declarations */
struct seaweed_fsal_module;
struct seaweed_fsal_export;
struct seaweed_fsal_obj_handle;

/* SeaweedFS FSAL Constants */
#define SEAWEED_MAGIC 0x53454157    /* 'SEAW' */
#define SEAWEED_VERSION 1
#define SEAWEED_MAX_PATH_LEN 4096
#define SEAWEED_HASH_LEN 16         /* MD5 hash length */
#define SEAWEED_DEFAULT_FILER_PORT 18888
#define SEAWEED_DEFAULT_TIMEOUT 30
#define SEAWEED_MAX_ENDPOINTS 8
#define SEAWEED_MAX_ENDPOINT_LEN 256

/* Configuration limits */
#define SEAWEED_MAX_CONNECTIONS 100
#define SEAWEED_DEFAULT_CONNECTIONS 10
#define SEAWEED_MAX_CACHE_SIZE 1000000
#define SEAWEED_DEFAULT_CACHE_SIZE 10000
#define SEAWEED_DEFAULT_CACHE_TTL 300

/**
 * @brief SeaweedFS filehandle structure
 * 
 * This structure represents a filehandle that can be encoded/decoded
 * and persists across server restarts.
 */
struct seaweed_filehandle {
	uint32_t magic;           /* SEAWEED_MAGIC */
	uint16_t version;         /* SEAWEED_VERSION */
	uint16_t flags;           /* Reserved flags */
	char path_hash[SEAWEED_HASH_LEN]; /* MD5 hash of full path */
	uint64_t create_time;     /* File creation timestamp */
	uint32_t path_len;        /* Path length for validation */
} __attribute__((packed));

/**
 * @brief Path mapping entry for hash->path resolution
 */
struct seaweed_path_mapping {
	char path_hash[SEAWEED_HASH_LEN];
	char full_path[SEAWEED_MAX_PATH_LEN];
	time_t cache_time;
	time_t expire_time;
	uint32_t ref_count;
	bool is_valid;
	struct seaweed_path_mapping *next;
};

/**
 * @brief SeaweedFS Filer connection info
 */
struct seaweed_filer_connection {
	char endpoint[SEAWEED_MAX_ENDPOINT_LEN];
	void *grpc_channel;       /* grpc_channel pointer */
	void *filer_stub;         /* seaweed_filer_stub pointer */
	time_t last_used;
	bool is_healthy;
	pthread_mutex_t lock;
	struct seaweed_filer_connection *next;
};

/**
 * @brief Connection pool for Filer connections
 */
struct seaweed_connection_pool {
	struct seaweed_filer_connection *connections;
	int pool_size;
	int active_count;
	pthread_mutex_t pool_lock;
	pthread_cond_t pool_cond;
};

/**
 * @brief SeaweedFS FSAL module structure
 */
struct seaweed_fsal_module {
	struct fsal_module fsal;
	struct seaweed_connection_pool conn_pool;
	
	/* Configuration */
	char filer_endpoints[SEAWEED_MAX_ENDPOINTS][SEAWEED_MAX_ENDPOINT_LEN];
	int num_endpoints;
	uint32_t connection_timeout;
	uint32_t connection_pool_size;
	uint32_t max_concurrent_requests;
	
	/* Path mapping cache */
	struct seaweed_path_mapping **path_cache;
	uint32_t path_cache_size;
	uint32_t path_cache_ttl;
	pthread_rwlock_t path_cache_lock;
	
	/* Simple file locking */
	bool enable_file_locks;
	uint32_t default_lock_timeout;
	
	/* Statistics */
	struct {
		uint64_t operations_total;
		uint64_t operations_failed;
		uint64_t cache_hits;
		uint64_t cache_misses;
		uint64_t connections_active;
		uint64_t connections_failed;
	} stats;
	pthread_mutex_t stats_lock;
};

/**
 * @brief SeaweedFS FSAL export structure
 */
struct seaweed_fsal_export {
	struct fsal_export export;
	struct seaweed_fsal_module *seaweed_module;
	
	/* Export-specific configuration */
	char export_path[SEAWEED_MAX_PATH_LEN];
	bool read_only;
};

/**
 * @brief SeaweedFS FSAL object handle structure
 */
struct seaweed_fsal_obj_handle {
	struct fsal_obj_handle obj_handle;
	struct seaweed_filehandle seaweed_handle;
	struct seaweed_fsal_export *export;
	
	/* Cached attributes */
	struct fsal_attrlist cached_attrs;
	time_t attrs_cache_time;
	bool attrs_cached;
	
	/* File-specific data */
	bool is_directory;
	char *full_path;  /* Cached full path */
};

/**
 * @brief Simple file lock structure (MVP implementation)
 */
struct seaweed_file_lock {
	char file_path[SEAWEED_MAX_PATH_LEN];
	char lock_token[256];     /* Filer lock token */
	char owner_id[256];       /* Lock owner identifier */
	time_t lock_time;
	time_t expire_time;
	bool is_write_lock;
	struct seaweed_file_lock *next;
};

/**
 * @brief Lock manager for simple file locks
 */
struct seaweed_lock_manager {
	struct seaweed_file_lock *locks;
	pthread_rwlock_t lock_list_lock;
	uint32_t lock_count;
	uint32_t max_locks;
};

/* Function prototypes */

/* Module initialization */
fsal_status_t seaweed_init_fsal_module(struct fsal_module *fsal_hdl,
				       config_file_t config_struct,
				       struct config_error_type *err_type);

/* Connection management */
fsal_status_t seaweed_init_connection_pool(struct seaweed_fsal_module *module);
void seaweed_destroy_connection_pool(struct seaweed_fsal_module *module);
struct seaweed_filer_connection *seaweed_acquire_connection(
	struct seaweed_fsal_module *module);
void seaweed_release_connection(struct seaweed_filer_connection *conn);

/* Path mapping and filehandle management */
fsal_status_t seaweed_create_handle_from_path(const char *path,
					      struct seaweed_filehandle *handle);
fsal_status_t seaweed_resolve_path_from_handle(
	const struct seaweed_filehandle *handle,
	char *path_out, size_t path_size);
void seaweed_add_to_path_cache(struct seaweed_fsal_module *module,
			       const char *path_hash, const char *path);
fsal_status_t seaweed_lookup_path_cache(struct seaweed_fsal_module *module,
					const char *path_hash,
					char *path_out, size_t path_size);

/* SeaweedFS Filer integration */
fsal_status_t seaweed_filer_lookup(struct seaweed_filer_connection *conn,
				   const char *parent_path, const char *name,
				   void *entry_out);
fsal_status_t seaweed_filer_create_file(struct seaweed_filer_connection *conn,
					const char *parent_path, const char *name,
					uint32_t mode, void *entry_out);
fsal_status_t seaweed_filer_create_directory(struct seaweed_filer_connection *conn,
					     const char *parent_path, const char *name,
					     uint32_t mode, void *entry_out);
fsal_status_t seaweed_filer_read_file(struct seaweed_filer_connection *conn,
				      const char *file_path, uint64_t offset,
				      size_t count, void *buffer, size_t *bytes_read);
fsal_status_t seaweed_filer_write_file(struct seaweed_filer_connection *conn,
				       const char *file_path, uint64_t offset,
				       size_t count, const void *buffer,
				       size_t *bytes_written);
fsal_status_t seaweed_filer_delete_entry(struct seaweed_filer_connection *conn,
					 const char *parent_path, const char *name,
					 bool is_directory);
fsal_status_t seaweed_filer_list_directory(struct seaweed_filer_connection *conn,
					   const char *dir_path, uint32_t limit,
					   const char *start_from,
					   void *entries_out, uint32_t *count_out);
fsal_status_t seaweed_filer_rename_entry(struct seaweed_filer_connection *conn,
					 const char *old_parent, const char *old_name,
					 const char *new_parent, const char *new_name);

/* Simple file locking (MVP) */
fsal_status_t seaweed_init_lock_manager(struct seaweed_lock_manager *lock_mgr);
void seaweed_destroy_lock_manager(struct seaweed_lock_manager *lock_mgr);
fsal_status_t seaweed_acquire_file_lock(struct seaweed_filer_connection *conn,
					const char *file_path, const char *owner_id,
					bool is_write_lock, char *lock_token_out,
					size_t token_size);
fsal_status_t seaweed_release_file_lock(struct seaweed_filer_connection *conn,
				        const char *file_path, const char *lock_token);

/* FSAL method implementations */
fsal_status_t seaweed_fsal_lookup(struct fsal_obj_handle *parent,
				  const char *path,
				  struct fsal_obj_handle **handle,
				  struct fsal_attrlist *attrs_out);
fsal_status_t seaweed_fsal_create(struct fsal_obj_handle *dir_hdl,
				  const char *name,
				  struct fsal_attrlist *attrib,
				  struct fsal_obj_handle **handle,
				  struct fsal_attrlist *attrs_out);
fsal_status_t seaweed_fsal_read(struct fsal_obj_handle *obj_hdl,
				uint64_t offset,
				size_t buffer_size,
				void *buffer,
				size_t *read_amount,
				bool *end_of_file);
fsal_status_t seaweed_fsal_write(struct fsal_obj_handle *obj_hdl,
				 uint64_t offset,
				 size_t buffer_size,
				 void *buffer,
				 size_t *write_amount,
				 bool *fsal_stable);

/* Attribute conversion utilities */
void seaweed_convert_entry_to_attrs(const void *seaweed_entry,
				    struct fsal_attrlist *attrs);
void seaweed_attrs_to_entry_attrs(const struct fsal_attrlist *attrs,
				  void *entry_attrs);

/* Error mapping utilities */
fsal_status_t seaweed_grpc_status_to_fsal(int grpc_status);

/* Utility functions */
void seaweed_compute_path_hash(const char *path, char *hash_out);
const char *seaweed_get_client_identifier(void);
bool seaweed_is_valid_name(const char *name);
time_t seaweed_current_time(void);

/* Configuration parsing */
fsal_status_t seaweed_load_config(struct seaweed_fsal_module *module,
				  config_file_t config_struct,
				  struct config_error_type *err_type);

/* Statistics */
void seaweed_update_stats(struct seaweed_fsal_module *module,
			  const char *operation, bool success);
void seaweed_get_stats(struct seaweed_fsal_module *module,
		       struct fsal_statistics *stats);

/* Export these for FSAL registration */
extern struct fsal_ops seaweed_fsal_ops;

#endif /* FSAL_SEAWEED_H */