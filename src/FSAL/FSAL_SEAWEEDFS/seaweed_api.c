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
 * @file seaweed_api.c
 * @brief SeaweedFS Filer API implementation (MVP stub version)
 */

#ifdef SEAWEED_MVP_TEST
/* Test mode includes */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include "seaweed_api.h"
/* Mock gsh_free and gsh_calloc for testing */
#define gsh_free(ptr) free(ptr)
#define gsh_calloc(count, size) calloc(count, size)
/* Mock logging for testing */
#define LogFullDebug(component, format, ...) printf("DEBUG: " format "\n", ##__VA_ARGS__)
#define LogDebug(component, format, ...) printf("DEBUG: " format "\n", ##__VA_ARGS__)
#define COMPONENT_FSAL 0
/* Add unused parameter macro */
#define UNUSED(x) ((void)(x))
/* File type constants for test mode */
#ifndef S_IFDIR
#define S_IFDIR  0040000
#endif
#ifndef S_IFREG
#define S_IFREG  0100000
#endif
#ifndef S_IFLNK
#define S_IFLNK  0120000
#endif
#else
/* NFS-Ganesha mode includes */
#include "config.h"
#include "fsal.h"
#include "fsal_seaweed.h"
#include "seaweed_api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#endif

/* Mock data for MVP implementation */
struct mock_file_entry {
	char path[SEAWEED_MAX_PATH];
	struct seaweed_entry entry;
	struct mock_file_entry *next;
};

static struct mock_file_entry *mock_filesystem = NULL;
static pthread_mutex_t mock_fs_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_inode = 1000;

/* Utility functions */

const char *seaweed_status_to_string(seaweed_status_t status)
{
	switch (status) {
	case SEAWEED_OK:
		return "OK";
	case SEAWEED_ERROR_NOT_FOUND:
		return "Not Found";
	case SEAWEED_ERROR_ALREADY_EXISTS:
		return "Already Exists";
	case SEAWEED_ERROR_ACCESS_DENIED:
		return "Access Denied";
	case SEAWEED_ERROR_IO:
		return "I/O Error";
	case SEAWEED_ERROR_INVALID_ARGUMENT:
		return "Invalid Argument";
	case SEAWEED_ERROR_UNAVAILABLE:
		return "Unavailable";
	case SEAWEED_ERROR_TIMEOUT:
		return "Timeout";
	case SEAWEED_ERROR_INTERNAL:
		return "Internal Error";
	default:
		return "Unknown Error";
	}
}

seaweed_status_t seaweed_parse_file_id(const char *file_id_str,
				       struct seaweed_file_id *fid)
{
	if (!file_id_str || !fid) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}

	/* Parse format: volume_id,needle_id,cookie */
	if (sscanf(file_id_str, "%u,%lu,%u", 
		   &fid->volume_id, &fid->needle_id, &fid->cookie) == 3) {
		return SEAWEED_OK;
	}

	return SEAWEED_ERROR_INVALID_ARGUMENT;
}

seaweed_status_t seaweed_format_file_id(const struct seaweed_file_id *fid,
					char *file_id_str, size_t str_len)
{
	if (!fid || !file_id_str || str_len == 0) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}

	int ret = snprintf(file_id_str, str_len, "%u,%lu,%u",
			   fid->volume_id, fid->needle_id, fid->cookie);
	
	if (ret < 0 || (size_t)ret >= str_len) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}

	return SEAWEED_OK;
}

void seaweed_free_entry(struct seaweed_entry *entry)
{
	struct seaweed_file_chunk *chunk, *next_chunk;

	if (!entry) {
		return;
	}

	/* Free chunks */
	chunk = entry->chunks;
	while (chunk) {
		next_chunk = chunk->next;
		gsh_free(chunk);
		chunk = next_chunk;
	}
	entry->chunks = NULL;
	entry->chunk_count = 0;

	/* Free extended attributes if any */
	if (entry->extended_attributes) {
		gsh_free(entry->extended_attributes);
		entry->extended_attributes = NULL;
	}
}

void seaweed_free_list_response(struct seaweed_list_response *resp)
{
	struct seaweed_entry *entry, *next_entry;

	if (!resp) {
		return;
	}

	entry = resp->entries;
	while (entry) {
		next_entry = entry->next;
		seaweed_free_entry(entry);
		gsh_free(entry);
		entry = next_entry;
	}
	resp->entries = NULL;
	resp->count = 0;
}

seaweed_status_t seaweed_clone_entry(const struct seaweed_entry *src,
				     struct seaweed_entry *dst)
{
	struct seaweed_file_chunk *src_chunk, *dst_chunk, *last_chunk = NULL;

	if (!src || !dst) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}

	/* Copy basic entry data */
	memcpy(dst, src, sizeof(struct seaweed_entry));
	dst->chunks = NULL;
	dst->next = NULL;
	dst->extended_attributes = NULL;

	/* Clone chunks */
	src_chunk = src->chunks;
	while (src_chunk) {
		dst_chunk = gsh_calloc(1, sizeof(struct seaweed_file_chunk));
		if (!dst_chunk) {
			seaweed_free_entry(dst);
			return SEAWEED_ERROR_INTERNAL;
		}

		memcpy(dst_chunk, src_chunk, sizeof(struct seaweed_file_chunk));
		dst_chunk->next = NULL;

		if (last_chunk) {
			last_chunk->next = dst_chunk;
		} else {
			dst->chunks = dst_chunk;
		}
		last_chunk = dst_chunk;

		src_chunk = src_chunk->next;
	}

	return SEAWEED_OK;
}

/* Mock filesystem functions */

static void init_mock_root(void)
{
	struct mock_file_entry *root;
	time_t now = time(NULL);

	if (mock_filesystem != NULL) {
		return;
	}

	root = gsh_calloc(1, sizeof(struct mock_file_entry));
	if (!root) {
		return;
	}

	strcpy(root->path, "/");
	strcpy(root->entry.name, "/");
	root->entry.type = SEAWEED_FILE_TYPE_DIRECTORY;
	root->entry.attributes.file_size = 4096;
	root->entry.attributes.file_mode = S_IFDIR | 0755;
	root->entry.attributes.uid = 0;
	root->entry.attributes.gid = 0;
	root->entry.attributes.mtime = now;
	root->entry.attributes.ctime = now;
	root->entry.attributes.inode = next_inode++;
	root->entry.attributes.nlinks = 2;
	strcpy(root->entry.attributes.mime, "inode/directory");
	root->entry.chunks = NULL;
	root->entry.chunk_count = 0;

	mock_filesystem = root;
}

static struct mock_file_entry *find_mock_entry(const char *path)
{
	struct mock_file_entry *entry = mock_filesystem;

	while (entry) {
		if (strcmp(entry->path, path) == 0) {
			return entry;
		}
		entry = entry->next;
	}

	return NULL;
}

static seaweed_status_t add_mock_entry(const char *path, const struct seaweed_entry *entry)
{
	struct mock_file_entry *mock_entry;
	
	if (find_mock_entry(path)) {
		return SEAWEED_ERROR_ALREADY_EXISTS;
	}

	mock_entry = gsh_calloc(1, sizeof(struct mock_file_entry));
	if (!mock_entry) {
		return SEAWEED_ERROR_INTERNAL;
	}

	strncpy(mock_entry->path, path, sizeof(mock_entry->path) - 1);
	if (seaweed_clone_entry(entry, &mock_entry->entry) != SEAWEED_OK) {
		gsh_free(mock_entry);
		return SEAWEED_ERROR_INTERNAL;
	}

	mock_entry->next = mock_filesystem;
	mock_filesystem = mock_entry;

	return SEAWEED_OK;
}

static seaweed_status_t remove_mock_entry(const char *path)
{
	struct mock_file_entry *entry = mock_filesystem;
	struct mock_file_entry *prev = NULL;

	while (entry) {
		if (strcmp(entry->path, path) == 0) {
			if (prev) {
				prev->next = entry->next;
			} else {
				mock_filesystem = entry->next;
			}
			
			seaweed_free_entry(&entry->entry);
			gsh_free(entry);
			return SEAWEED_OK;
		}
		prev = entry;
		entry = entry->next;
	}

	return SEAWEED_ERROR_NOT_FOUND;
}

/* API Implementation */

seaweed_status_t seaweed_filer_lookup_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_lookup_request *req,
					     struct seaweed_lookup_response *resp)
{
	char full_path[SEAWEED_MAX_PATH];
	struct mock_file_entry *entry;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS lookup: %s/%s", req->directory, req->name);

	pthread_mutex_lock(&mock_fs_lock);

	init_mock_root();

	/* Build full path */
	if (strcmp(req->directory, "/") == 0) {
		if (strcmp(req->name, "/") == 0) {
			strcpy(full_path, "/");  /* Special case: root lookup */
		} else {
			int ret = snprintf(full_path, sizeof(full_path), "/%s", req->name);
			if (ret >= (int)sizeof(full_path)) {
				resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
				pthread_mutex_unlock(&mock_fs_lock);
				return resp->status;
			}
		}
	} else {
		int ret = snprintf(full_path, sizeof(full_path), "%s/%s", req->directory, req->name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	entry = find_mock_entry(full_path);
	if (entry) {
		resp->status = SEAWEED_OK;
		if (seaweed_clone_entry(&entry->entry, &resp->entry) != SEAWEED_OK) {
			resp->status = SEAWEED_ERROR_INTERNAL;
		}
	} else {
		resp->status = SEAWEED_ERROR_NOT_FOUND;
		memset(&resp->entry, 0, sizeof(resp->entry));
	}

	pthread_mutex_unlock(&mock_fs_lock);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS lookup result: %s", 
		     seaweed_status_to_string(resp->status));

	return resp->status;
}

seaweed_status_t seaweed_filer_create_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_create_request *req,
					     struct seaweed_create_response *resp)
{
	char full_path[SEAWEED_MAX_PATH];
	struct seaweed_entry new_entry;
	time_t now = time(NULL);

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogDebug(COMPONENT_FSAL, "SeaweedFS create: %s/%s", req->directory, req->entry.name);

	pthread_mutex_lock(&mock_fs_lock);

	init_mock_root();

	/* Build full path */
	if (strcmp(req->directory, "/") == 0) {
		int ret = snprintf(full_path, sizeof(full_path), "/%s", req->entry.name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	} else {
		int ret = snprintf(full_path, sizeof(full_path), "%s/%s", req->directory, req->entry.name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	/* Check if already exists (for O_EXCL) */
	if (req->o_excl && find_mock_entry(full_path)) {
		resp->status = SEAWEED_ERROR_ALREADY_EXISTS;
		pthread_mutex_unlock(&mock_fs_lock);
		return resp->status;
	}

	/* Create new entry */
	memcpy(&new_entry, &req->entry, sizeof(new_entry));
	new_entry.attributes.inode = next_inode++;
	new_entry.attributes.mtime = now;
	new_entry.attributes.ctime = now;
	new_entry.attributes.nlinks = (new_entry.type == SEAWEED_FILE_TYPE_DIRECTORY) ? 2 : 1;
	
	/* Set default attributes if not specified */
	if (new_entry.attributes.file_mode == 0) {
		if (new_entry.type == SEAWEED_FILE_TYPE_DIRECTORY) {
			new_entry.attributes.file_mode = S_IFDIR | 0755;
			new_entry.attributes.file_size = 4096;
			strcpy(new_entry.attributes.mime, "inode/directory");
		} else {
			new_entry.attributes.file_mode = S_IFREG | 0644;
			new_entry.attributes.file_size = 0;
			strcpy(new_entry.attributes.mime, "application/octet-stream");
		}
	}

	resp->status = add_mock_entry(full_path, &new_entry);
	if (resp->status == SEAWEED_OK) {
		if (seaweed_clone_entry(&new_entry, &resp->entry) != SEAWEED_OK) {
			resp->status = SEAWEED_ERROR_INTERNAL;
		}
	}

	pthread_mutex_unlock(&mock_fs_lock);

	LogDebug(COMPONENT_FSAL, "SeaweedFS create result: %s", 
		 seaweed_status_to_string(resp->status));

	return resp->status;
}

seaweed_status_t seaweed_filer_list_entries(struct seaweed_filer_connection *conn,
					     const struct seaweed_list_request *req,
					     struct seaweed_list_response *resp)
{
	struct mock_file_entry *entry;
	struct seaweed_entry *list_entry, *last_entry = NULL;
	uint32_t count = 0;
	char dir_with_slash[SEAWEED_MAX_PATH];
	size_t dir_len;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS list: %s", req->directory);

	pthread_mutex_lock(&mock_fs_lock);

	init_mock_root();

	/* Prepare directory path with trailing slash */
	strncpy(dir_with_slash, req->directory, sizeof(dir_with_slash) - 1);
	dir_len = strlen(dir_with_slash);
	if (dir_len > 0 && dir_with_slash[dir_len - 1] != '/') {
		strncat(dir_with_slash, "/", sizeof(dir_with_slash) - dir_len - 1);
		dir_len++;
	}

	resp->status = SEAWEED_OK;
	resp->entries = NULL;
	resp->count = 0;
	resp->has_more = false;
	memset(resp->last_name, 0, sizeof(resp->last_name));

	/* Find entries in the directory */
	entry = mock_filesystem;
	while (entry && count < req->limit) {
		/* Check if entry is in the requested directory */
		if (strncmp(entry->path, dir_with_slash, dir_len) == 0) {
			const char *relative_path = entry->path + dir_len;
			
			/* Skip the directory itself */
			if (strlen(relative_path) == 0) {
				entry = entry->next;
				continue;
			}
			
			/* Skip subdirectory entries (only immediate children) */
			if (strchr(relative_path, '/') != NULL) {
				entry = entry->next;
				continue;
			}

			/* Apply prefix filter if specified */
			if (req->prefix[0] != '\0' && 
			    strncmp(relative_path, req->prefix, strlen(req->prefix)) != 0) {
				entry = entry->next;
				continue;
			}

			/* Apply start_from filter if specified */
			if (req->start_from[0] != '\0') {
				int cmp = strcmp(relative_path, req->start_from);
				if (cmp < 0 || (cmp == 0 && !req->inclusive_start)) {
					entry = entry->next;
					continue;
				}
			}

			/* Add entry to result list */
			list_entry = gsh_calloc(1, sizeof(struct seaweed_entry));
			if (!list_entry) {
				seaweed_free_list_response(resp);
				resp->status = SEAWEED_ERROR_INTERNAL;
				pthread_mutex_unlock(&mock_fs_lock);
				return resp->status;
			}

			if (seaweed_clone_entry(&entry->entry, list_entry) != SEAWEED_OK) {
				gsh_free(list_entry);
				seaweed_free_list_response(resp);
				resp->status = SEAWEED_ERROR_INTERNAL;
				pthread_mutex_unlock(&mock_fs_lock);
				return resp->status;
			}

			/* Fix the name to be relative to the directory */
			strncpy(list_entry->name, relative_path, sizeof(list_entry->name) - 1);

			/* Add to linked list */
			if (last_entry) {
				last_entry->next = list_entry;
			} else {
				resp->entries = list_entry;
			}
			last_entry = list_entry;

			count++;
			strncpy(resp->last_name, relative_path, sizeof(resp->last_name) - 1);
		}

		entry = entry->next;
	}

	resp->count = count;
	resp->has_more = (entry != NULL);  /* Simple heuristic */

	pthread_mutex_unlock(&mock_fs_lock);

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS list result: %d entries", count);

	return resp->status;
}

seaweed_status_t seaweed_filer_delete_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_delete_request *req,
					     struct seaweed_delete_response *resp)
{
	char full_path[SEAWEED_MAX_PATH];

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogDebug(COMPONENT_FSAL, "SeaweedFS delete: %s/%s", req->directory, req->name);

	pthread_mutex_lock(&mock_fs_lock);

	/* Build full path */
	if (strcmp(req->directory, "/") == 0) {
		int ret = snprintf(full_path, sizeof(full_path), "/%s", req->name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	} else {
		int ret = snprintf(full_path, sizeof(full_path), "%s/%s", req->directory, req->name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	resp->status = remove_mock_entry(full_path);

	pthread_mutex_unlock(&mock_fs_lock);

	LogDebug(COMPONENT_FSAL, "SeaweedFS delete result: %s", 
		 seaweed_status_to_string(resp->status));

	return resp->status;
}

seaweed_status_t seaweed_filer_rename_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_rename_request *req,
					     struct seaweed_rename_response *resp)
{
	char old_path[SEAWEED_MAX_PATH];
	char new_path[SEAWEED_MAX_PATH];
	struct mock_file_entry *entry;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogDebug(COMPONENT_FSAL, "SeaweedFS rename: %s/%s -> %s/%s", 
		 req->old_directory, req->old_name, req->new_directory, req->new_name);

	pthread_mutex_lock(&mock_fs_lock);

	/* Build paths */
	if (strcmp(req->old_directory, "/") == 0) {
		int ret = snprintf(old_path, sizeof(old_path), "/%s", req->old_name);
		if (ret >= (int)sizeof(old_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	} else {
		int ret = snprintf(old_path, sizeof(old_path), "%s/%s", req->old_directory, req->old_name);
		if (ret >= (int)sizeof(old_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	if (strcmp(req->new_directory, "/") == 0) {
		int ret = snprintf(new_path, sizeof(new_path), "/%s", req->new_name);
		if (ret >= (int)sizeof(new_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	} else {
		int ret = snprintf(new_path, sizeof(new_path), "%s/%s", req->new_directory, req->new_name);
		if (ret >= (int)sizeof(new_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	/* Find old entry */
	entry = find_mock_entry(old_path);
	if (!entry) {
		resp->status = SEAWEED_ERROR_NOT_FOUND;
		pthread_mutex_unlock(&mock_fs_lock);
		return resp->status;
	}

	/* Check if new path already exists */
	if (find_mock_entry(new_path)) {
		resp->status = SEAWEED_ERROR_ALREADY_EXISTS;
		pthread_mutex_unlock(&mock_fs_lock);
		return resp->status;
	}

	/* Update path and name */
	strncpy(entry->path, new_path, sizeof(entry->path) - 1);
	strncpy(entry->entry.name, req->new_name, sizeof(entry->entry.name) - 1);

	resp->status = SEAWEED_OK;

	pthread_mutex_unlock(&mock_fs_lock);

	LogDebug(COMPONENT_FSAL, "SeaweedFS rename result: %s", 
		 seaweed_status_to_string(resp->status));

	return resp->status;
}

seaweed_status_t seaweed_filer_update_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_update_request *req,
					     struct seaweed_update_response *resp)
{
	char full_path[SEAWEED_MAX_PATH];
	struct mock_file_entry *entry;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	LogFullDebug(COMPONENT_FSAL, "SeaweedFS update: %s/%s", req->directory, req->entry.name);

	pthread_mutex_lock(&mock_fs_lock);

	/* Build full path */
	if (strcmp(req->directory, "/") == 0) {
		int ret = snprintf(full_path, sizeof(full_path), "/%s", req->entry.name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	} else {
		int ret = snprintf(full_path, sizeof(full_path), "%s/%s", req->directory, req->entry.name);
		if (ret >= (int)sizeof(full_path)) {
			resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
			pthread_mutex_unlock(&mock_fs_lock);
			return resp->status;
		}
	}

	entry = find_mock_entry(full_path);
	if (entry) {
		/* Update entry (preserve inode and creation time) */
		uint64_t old_inode = entry->entry.attributes.inode;
		int64_t old_ctime = entry->entry.attributes.ctime;
		
		seaweed_free_entry(&entry->entry);
		if (seaweed_clone_entry(&req->entry, &entry->entry) == SEAWEED_OK) {
			entry->entry.attributes.inode = old_inode;
			entry->entry.attributes.ctime = old_ctime;
			entry->entry.attributes.mtime = time(NULL);
			resp->status = SEAWEED_OK;
		} else {
			resp->status = SEAWEED_ERROR_INTERNAL;
		}
	} else {
		resp->status = SEAWEED_ERROR_NOT_FOUND;
	}

	pthread_mutex_unlock(&mock_fs_lock);

	return resp->status;
}

/* Simplified volume operations for MVP */

seaweed_status_t seaweed_filer_assign_volume(struct seaweed_filer_connection *conn,
					      const struct seaweed_assign_request *req,
					      struct seaweed_assign_response *resp)
{
	static uint32_t next_volume_id = 1;
	static uint64_t next_needle_id = 1000;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	/* Mock volume assignment */
	snprintf(resp->file_id, sizeof(resp->file_id), "%u,%lu,%u",
		 next_volume_id, next_needle_id++, (unsigned int)time(NULL) % 10000);
	
	resp->status = SEAWEED_OK;
	resp->count = 1;
	strncpy(resp->collection, req->collection, sizeof(resp->collection) - 1);
	strncpy(resp->replication, req->replication, sizeof(resp->replication) - 1);
	
	/* Mock location */
	snprintf(resp->location.url, sizeof(resp->location.url), 
		 "http://localhost:8080");
	snprintf(resp->location.public_url, sizeof(resp->location.public_url),
		 "http://localhost:8080");
	resp->location.grpc_port = 18080;
	strncpy(resp->location.data_center, "default", sizeof(resp->location.data_center) - 1);

	next_volume_id = (next_volume_id % 255) + 1;  /* Cycle volume IDs */

	return SEAWEED_OK;
}

/* Simple file locking (MVP implementation) */

static struct {
	char locks[100][SEAWEED_MAX_PATH];  /* Simple array of locked paths */
	char owners[100][128];              /* Lock owners */
	char tokens[100][256];              /* Renew tokens */
	time_t expires[100];                /* Expiration times */
	int count;
	pthread_mutex_t lock;
} simple_lock_table = { .count = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

seaweed_status_t seaweed_filer_lock(struct seaweed_filer_connection *conn,
				    const struct seaweed_lock_request *req,
				    struct seaweed_lock_response *resp)
{
	time_t now = time(NULL);
	time_t expire_time = now + req->seconds_to_lock;
	int i, free_slot = -1;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	pthread_mutex_lock(&simple_lock_table.lock);

	/* Clean up expired locks */
	for (i = 0; i < simple_lock_table.count; i++) {
		if (simple_lock_table.expires[i] < now) {
			/* Mark as available */
			simple_lock_table.locks[i][0] = '\0';
		}
	}

	/* Check if already locked */
	for (i = 0; i < simple_lock_table.count; i++) {
		if (simple_lock_table.locks[i][0] != '\0' &&
		    strcmp(simple_lock_table.locks[i], req->name) == 0) {
			/* Already locked */
			resp->status = SEAWEED_ERROR_ALREADY_EXISTS;
			strncpy(resp->lock_owner, simple_lock_table.owners[i], 
				sizeof(resp->lock_owner) - 1);
			pthread_mutex_unlock(&simple_lock_table.lock);
			return resp->status;
		}
		if (simple_lock_table.locks[i][0] == '\0' && free_slot == -1) {
			free_slot = i;
		}
	}

	/* Find or create a slot */
	if (free_slot == -1) {
		if (simple_lock_table.count < 100) {
			free_slot = simple_lock_table.count++;
		} else {
			resp->status = SEAWEED_ERROR_UNAVAILABLE;
			pthread_mutex_unlock(&simple_lock_table.lock);
			return resp->status;
		}
	}

	/* Create the lock */
	strncpy(simple_lock_table.locks[free_slot], req->name, SEAWEED_MAX_PATH - 1);
	strncpy(simple_lock_table.owners[free_slot], req->owner, 127);
	int ret = snprintf(simple_lock_table.tokens[free_slot], 255, "token_%s_%s", req->owner, req->name);
	if (ret >= 255) {
		resp->status = SEAWEED_ERROR_INVALID_ARGUMENT;
		pthread_mutex_unlock(&simple_lock_table.lock);
		return resp->status;
	}
	simple_lock_table.expires[free_slot] = expire_time;

	resp->status = SEAWEED_OK;
	strncpy(resp->renew_token, simple_lock_table.tokens[free_slot], 
		sizeof(resp->renew_token) - 1);
	strncpy(resp->lock_owner, req->owner, sizeof(resp->lock_owner) - 1);

	pthread_mutex_unlock(&simple_lock_table.lock);

	LogDebug(COMPONENT_FSAL, "SeaweedFS lock acquired: %s", req->name);
	return resp->status;
}

seaweed_status_t seaweed_filer_unlock(struct seaweed_filer_connection *conn,
				      const struct seaweed_unlock_request *req,
				      struct seaweed_unlock_response *resp)
{
	int i;
	bool found = false;

	if (!req || !resp) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
	
#ifndef SEAWEED_MVP_TEST
	if (!conn) {
		return SEAWEED_ERROR_INVALID_ARGUMENT;
	}
#else
	UNUSED(conn);
#endif

	pthread_mutex_lock(&simple_lock_table.lock);

	/* Find and remove the lock by token or by name+owner for compatibility */
	for (i = 0; i < simple_lock_table.count; i++) {
		if (strcmp(simple_lock_table.tokens[i], req->renew_token) == 0 ||
		    (strcmp(simple_lock_table.locks[i], req->name) == 0 && 
		     strstr(req->renew_token, simple_lock_table.owners[i]) != NULL)) {
			simple_lock_table.locks[i][0] = '\0';  /* Mark as free */
			found = true;
			break;
		}
	}

	resp->status = found ? SEAWEED_OK : SEAWEED_ERROR_NOT_FOUND;

	pthread_mutex_unlock(&simple_lock_table.lock);

	LogDebug(COMPONENT_FSAL, "SeaweedFS unlock: %s (%s)", req->name,
		 found ? "success" : "not found");
	return resp->status;
}

/* Volume server operations (simplified for MVP) */

seaweed_status_t seaweed_volume_write(const struct seaweed_location *location,
				      const char *file_id,
				      const void *data, size_t size,
				      const char *auth_token)
{
#ifdef SEAWEED_MVP_TEST
	UNUSED(location);
	UNUSED(data);
	UNUSED(auth_token);
#endif
	/* For MVP, we simulate successful write operations */
	LogFullDebug(COMPONENT_FSAL, "SeaweedFS volume write: %s (%zu bytes)", 
		     file_id, size);
	return SEAWEED_OK;
}

seaweed_status_t seaweed_volume_read(const struct seaweed_location *location,
				     const char *file_id,
				     uint64_t offset, size_t size,
				     void *buffer, size_t *bytes_read)
{
#ifdef SEAWEED_MVP_TEST
	UNUSED(location);
#endif
	/* For MVP, we simulate read operations with dummy data */
	if (buffer && bytes_read) {
		*bytes_read = (size > 1024) ? 1024 : size;  /* Simulate partial read */
		memset(buffer, 'A', *bytes_read);  /* Fill with dummy data */
	}
	
	LogFullDebug(COMPONENT_FSAL, "SeaweedFS volume read: %s (offset=%lu, size=%zu)", 
		     file_id, offset, size);
	return SEAWEED_OK;
}

seaweed_status_t seaweed_volume_delete(const struct seaweed_location *location,
				       const char *file_id,
				       const char *auth_token)
{
#ifdef SEAWEED_MVP_TEST
	UNUSED(location);
	UNUSED(auth_token);
#endif
	/* For MVP, we simulate successful delete operations */
	LogFullDebug(COMPONENT_FSAL, "SeaweedFS volume delete: %s", file_id);
	return SEAWEED_OK;
}