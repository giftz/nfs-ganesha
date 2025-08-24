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
 * @file handle.c
 * @brief SeaweedFS FSAL filehandle management
 */

#include "config.h"
#include "fsal.h"
#include "fsal_seaweed.h"
#include <openssl/md5.h>
#include <string.h>
#include <time.h>

/* Path mapping hash table operations */

/**
 * @brief Compute MD5 hash of a path
 */
void seaweed_compute_path_hash(const char *path, char *hash_out)
{
	MD5_CTX md5_ctx;
	
	if (!path || !hash_out) {
		return;
	}
	
	MD5_Init(&md5_ctx);
	MD5_Update(&md5_ctx, path, strlen(path));
	MD5_Final((unsigned char *)hash_out, &md5_ctx);
}

/**
 * @brief Hash function for path cache
 */
static uint32_t path_cache_hash(const char *hash)
{
	uint32_t result = 0;
	int i;
	
	for (i = 0; i < SEAWEED_HASH_LEN; i++) {
		result = result * 31 + (unsigned char)hash[i];
	}
	
	return result;
}

/**
 * @brief Create filehandle from path
 */
fsal_status_t seaweed_create_handle_from_path(const char *path,
					      struct seaweed_filehandle *handle)
{
	if (!path || !handle) {
		LogMajor(COMPONENT_FSAL, "Invalid parameters to create_handle_from_path");
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	if (strlen(path) >= SEAWEED_MAX_PATH_LEN) {
		LogMajor(COMPONENT_FSAL, "Path too long: %zu >= %d", 
			 strlen(path), SEAWEED_MAX_PATH_LEN);
		return fsalstat(ERR_FSAL_NAMETOOLONG, ENAMETOOLONG);
	}

	memset(handle, 0, sizeof(struct seaweed_filehandle));
	
	/* Set magic and version */
	handle->magic = SEAWEED_MAGIC;
	handle->version = SEAWEED_VERSION;
	handle->flags = 0;
	
	/* Compute path hash */
	seaweed_compute_path_hash(path, handle->path_hash);
	
	/* Set timestamps */
	handle->create_time = (uint64_t)time(NULL);
	handle->path_len = (uint32_t)strlen(path);

	LogFullDebug(COMPONENT_FSAL, 
		     "Created filehandle for path: %s (len=%d)", 
		     path, handle->path_len);
	
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Add path to cache
 */
void seaweed_add_to_path_cache(struct seaweed_fsal_module *module,
			       const char *path_hash, const char *path)
{
	struct seaweed_path_mapping *mapping;
	uint32_t cache_index;
	time_t now;

	if (!module || !path_hash || !path) {
		return;
	}

	if (strlen(path) >= SEAWEED_MAX_PATH_LEN) {
		LogDebug(COMPONENT_FSAL, "Path too long for cache: %s", path);
		return;
	}

	now = time(NULL);
	cache_index = path_cache_hash(path_hash) % module->path_cache_size;

	pthread_rwlock_wrlock(&module->path_cache_lock);

	/* Check if already exists */
	mapping = module->path_cache[cache_index];
	while (mapping) {
		if (memcmp(mapping->path_hash, path_hash, SEAWEED_HASH_LEN) == 0) {
			/* Update existing entry */
			strncpy(mapping->full_path, path, sizeof(mapping->full_path) - 1);
			mapping->full_path[sizeof(mapping->full_path) - 1] = '\0';
			mapping->cache_time = now;
			mapping->expire_time = now + module->path_cache_ttl;
			mapping->is_valid = true;
			mapping->ref_count++;
			
			LogFullDebug(COMPONENT_FSAL, 
				     "Updated path cache entry: %s", path);
			goto unlock;
		}
		mapping = mapping->next;
	}

	/* Create new entry */
	mapping = gsh_calloc(1, sizeof(struct seaweed_path_mapping));
	if (!mapping) {
		LogMajor(COMPONENT_FSAL, "Failed to allocate path mapping");
		goto unlock;
	}

	memcpy(mapping->path_hash, path_hash, SEAWEED_HASH_LEN);
	strncpy(mapping->full_path, path, sizeof(mapping->full_path) - 1);
	mapping->full_path[sizeof(mapping->full_path) - 1] = '\0';
	mapping->cache_time = now;
	mapping->expire_time = now + module->path_cache_ttl;
	mapping->ref_count = 1;
	mapping->is_valid = true;

	/* Add to hash chain */
	mapping->next = module->path_cache[cache_index];
	module->path_cache[cache_index] = mapping;

	LogFullDebug(COMPONENT_FSAL, "Added path to cache: %s", path);

	/* Update cache statistics */
	pthread_mutex_lock(&module->stats_lock);
	/* Note: cache_hits and cache_misses are updated in lookup operations */
	pthread_mutex_unlock(&module->stats_lock);

unlock:
	pthread_rwlock_unlock(&module->path_cache_lock);
}

/**
 * @brief Lookup path in cache
 */
fsal_status_t seaweed_lookup_path_cache(struct seaweed_fsal_module *module,
					const char *path_hash,
					char *path_out, size_t path_size)
{
	struct seaweed_path_mapping *mapping;
	uint32_t cache_index;
	time_t now;
	fsal_status_t status = fsalstat(ERR_FSAL_NOENT, ENOENT);

	if (!module || !path_hash || !path_out || path_size == 0) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	now = time(NULL);
	cache_index = path_cache_hash(path_hash) % module->path_cache_size;

	pthread_rwlock_rdlock(&module->path_cache_lock);

	mapping = module->path_cache[cache_index];
	while (mapping) {
		if (memcmp(mapping->path_hash, path_hash, SEAWEED_HASH_LEN) == 0) {
			if (mapping->is_valid && mapping->expire_time > now) {
				/* Cache hit */
				strncpy(path_out, mapping->full_path, path_size - 1);
				path_out[path_size - 1] = '\0';
				mapping->ref_count++;
				status = fsalstat(ERR_FSAL_NO_ERROR, 0);
				
				LogFullDebug(COMPONENT_FSAL, 
					     "Cache hit: %s", mapping->full_path);
				
				/* Update cache hit statistics */
				pthread_mutex_lock(&module->stats_lock);
				module->stats.cache_hits++;
				pthread_mutex_unlock(&module->stats_lock);
			} else {
				/* Cache entry expired */
				mapping->is_valid = false;
				LogFullDebug(COMPONENT_FSAL, 
					     "Cache entry expired: %s", mapping->full_path);
			}
			break;
		}
		mapping = mapping->next;
	}

	pthread_rwlock_unlock(&module->path_cache_lock);

	if (FSAL_IS_ERROR(status)) {
		/* Update cache miss statistics */
		pthread_mutex_lock(&module->stats_lock);
		module->stats.cache_misses++;
		pthread_mutex_unlock(&module->stats_lock);
	}

	return status;
}

/**
 * @brief Resolve path from filehandle
 */
fsal_status_t seaweed_resolve_path_from_handle(
	const struct seaweed_filehandle *handle,
	char *path_out, size_t path_size)
{
	/* For MVP, we need access to the module to lookup in cache */
	/* This function should be called with module context */
	
	if (!handle || !path_out || path_size == 0) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	/* Validate handle */
	if (handle->magic != SEAWEED_MAGIC) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle magic: 0x%x", handle->magic);
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	if (handle->version != SEAWEED_VERSION) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle version: %d", handle->version);
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	/* For MVP, we return an error here since we need module context */
	/* In practice, this function would be called through the module interface */
	LogDebug(COMPONENT_FSAL, "Path resolution requires module context");
	return fsalstat(ERR_FSAL_STALE, ESTALE);
}

/**
 * @brief Resolve path from filehandle with module context
 */
fsal_status_t seaweed_resolve_path_from_handle_with_module(
	struct seaweed_fsal_module *module,
	const struct seaweed_filehandle *handle,
	char *path_out, size_t path_size)
{
	fsal_status_t status;

	if (!module || !handle || !path_out || path_size == 0) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	/* Validate handle */
	if (handle->magic != SEAWEED_MAGIC) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle magic: 0x%x", handle->magic);
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	if (handle->version != SEAWEED_VERSION) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle version: %d", handle->version);
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	/* Look up path in cache */
	status = seaweed_lookup_path_cache(module, handle->path_hash, 
					   path_out, path_size);
	if (FSAL_IS_ERROR(status)) {
		LogDebug(COMPONENT_FSAL, "Failed to resolve path from cache");
		return status;
	}

	/* Verify path length matches handle */
	if (strlen(path_out) != handle->path_len) {
		LogDebug(COMPONENT_FSAL, 
			 "Path length mismatch: actual=%zu, expected=%u",
			 strlen(path_out), handle->path_len);
		/* This could happen if path was renamed, but for MVP we treat as stale */
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	LogFullDebug(COMPONENT_FSAL, "Resolved path: %s", path_out);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Clean expired entries from path cache
 * 
 * This function should be called periodically to clean up expired cache entries.
 */
void seaweed_cleanup_path_cache(struct seaweed_fsal_module *module)
{
	struct seaweed_path_mapping *mapping, *next_mapping, *prev_mapping;
	time_t now;
	uint32_t i;
	uint32_t cleaned_count = 0;

	if (!module || !module->path_cache) {
		return;
	}

	now = time(NULL);

	pthread_rwlock_wrlock(&module->path_cache_lock);

	for (i = 0; i < module->path_cache_size; i++) {
		prev_mapping = NULL;
		mapping = module->path_cache[i];

		while (mapping) {
			next_mapping = mapping->next;

			/* Remove expired entries with zero references */
			if (!mapping->is_valid || 
			    (mapping->expire_time < now && mapping->ref_count == 0)) {
				
				if (prev_mapping) {
					prev_mapping->next = next_mapping;
				} else {
					module->path_cache[i] = next_mapping;
				}

				LogFullDebug(COMPONENT_FSAL,
					     "Cleaning expired cache entry: %s",
					     mapping->full_path);
				
				gsh_free(mapping);
				cleaned_count++;
			} else {
				prev_mapping = mapping;
			}

			mapping = next_mapping;
		}
	}

	pthread_rwlock_unlock(&module->path_cache_lock);

	if (cleaned_count > 0) {
		LogDebug(COMPONENT_FSAL, "Cleaned %u expired cache entries", cleaned_count);
	}
}

/**
 * @brief Convert wire format handle to internal format
 * 
 * This function converts a filehandle from NFS wire format to our internal format.
 */
fsal_status_t seaweed_wire_to_host_handle(struct fsal_obj_handle *obj_hdl,
					  fsal_digesttype_t in_type,
					  struct gsh_buffdesc *fh_desc)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;
	
	if (!obj_hdl || !fh_desc) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	if (in_type != FSAL_DIGEST_NFSV3 && in_type != FSAL_DIGEST_NFSV4) {
		LogMajor(COMPONENT_FSAL, "Unsupported digest type: %d", in_type);
		return fsalstat(ERR_FSAL_SERVERFAULT, EINVAL);
	}

	if (fh_desc->len != sizeof(struct seaweed_filehandle)) {
		LogMajor(COMPONENT_FSAL, 
			 "Invalid filehandle size: %zu, expected: %zu",
			 fh_desc->len, sizeof(struct seaweed_filehandle));
		return fsalstat(ERR_FSAL_SERVERFAULT, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);
	
	/* Copy the handle data */
	memcpy(&seaweed_handle->seaweed_handle, fh_desc->addr, fh_desc->len);

	/* Validate the handle */
	if (seaweed_handle->seaweed_handle.magic != SEAWEED_MAGIC) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle magic");
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	if (seaweed_handle->seaweed_handle.version != SEAWEED_VERSION) {
		LogMajor(COMPONENT_FSAL, "Invalid filehandle version");
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	LogFullDebug(COMPONENT_FSAL, "Converted wire handle to host format");
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Convert internal format handle to wire format
 * 
 * This function converts our internal filehandle to NFS wire format.
 */
fsal_status_t seaweed_host_to_wire_handle(struct fsal_obj_handle *obj_hdl,
					  fsal_digesttype_t out_type,
					  struct gsh_buffdesc *fh_desc)
{
	struct seaweed_fsal_obj_handle *seaweed_handle;

	if (!obj_hdl || !fh_desc) {
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	seaweed_handle = container_of(obj_hdl, struct seaweed_fsal_obj_handle, obj_handle);

	if (out_type != FSAL_DIGEST_NFSV3 && out_type != FSAL_DIGEST_NFSV4) {
		LogMajor(COMPONENT_FSAL, "Unsupported digest type: %d", out_type);
		return fsalstat(ERR_FSAL_SERVERFAULT, EINVAL);
	}

	if (fh_desc->len < sizeof(struct seaweed_filehandle)) {
		LogMajor(COMPONENT_FSAL,
			 "Insufficient buffer size: %zu, required: %zu",
			 fh_desc->len, sizeof(struct seaweed_filehandle));
		return fsalstat(ERR_FSAL_TOOSMALL, ENOBUFS);
	}

	/* Copy the handle data */
	memcpy(fh_desc->addr, &seaweed_handle->seaweed_handle, 
	       sizeof(struct seaweed_filehandle));
	fh_desc->len = sizeof(struct seaweed_filehandle);

	LogFullDebug(COMPONENT_FSAL, "Converted host handle to wire format");
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}