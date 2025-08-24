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
 * @file seaweed_api.h
 * @brief SeaweedFS Filer API definitions and structures
 */

#ifndef SEAWEED_API_H
#define SEAWEED_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

/* Maximum limits */
#define SEAWEED_MAX_FILE_NAME 255
#define SEAWEED_MAX_PATH 4096
#define SEAWEED_MAX_ETAG 32
#define SEAWEED_MAX_FILE_ID 256
#define SEAWEED_MAX_URL 512

/* File types */
typedef enum {
	SEAWEED_FILE_TYPE_UNKNOWN = 0,
	SEAWEED_FILE_TYPE_REGULAR = 1,
	SEAWEED_FILE_TYPE_DIRECTORY = 2,
	SEAWEED_FILE_TYPE_SYMLINK = 3
} seaweed_file_type_t;

/* API status codes */
typedef enum {
	SEAWEED_OK = 0,
	SEAWEED_ERROR_NOT_FOUND = 1,
	SEAWEED_ERROR_ALREADY_EXISTS = 2,
	SEAWEED_ERROR_ACCESS_DENIED = 3,
	SEAWEED_ERROR_IO = 4,
	SEAWEED_ERROR_INVALID_ARGUMENT = 5,
	SEAWEED_ERROR_UNAVAILABLE = 6,
	SEAWEED_ERROR_TIMEOUT = 7,
	SEAWEED_ERROR_INTERNAL = 8
} seaweed_status_t;

/**
 * @brief File ID structure
 */
struct seaweed_file_id {
	uint32_t volume_id;
	uint64_t needle_id;
	uint32_t cookie;
};

/**
 * @brief File chunk information
 */
struct seaweed_file_chunk {
	struct seaweed_file_id fid;
	uint64_t offset;        /* Offset in file */
	uint64_t size;          /* Chunk size */
	uint64_t modified_ts;   /* Modified timestamp (nanoseconds) */
	char etag[SEAWEED_MAX_ETAG];
	char url[SEAWEED_MAX_URL];  /* Volume server URL */
	bool is_compressed;
	bool is_encrypted;
	struct seaweed_file_chunk *next;
};

/**
 * @brief FUSE-like attributes
 */
struct seaweed_attributes {
	uint64_t file_size;
	int64_t mtime;          /* Modified time (seconds since epoch) */
	int64_t ctime;          /* Created time (seconds since epoch) */
	uint32_t file_mode;     /* File mode (like st_mode) */
	uint32_t uid;           /* User ID */
	uint32_t gid;           /* Group ID */
	uint64_t inode;         /* Inode number */
	uint32_t nlinks;        /* Number of hard links */
	char mime[64];          /* MIME type */
	char etag[SEAWEED_MAX_ETAG];
	char symlink_target[SEAWEED_MAX_PATH];  /* For symbolic links */
};

/**
 * @brief Directory entry
 */
struct seaweed_entry {
	char name[SEAWEED_MAX_FILE_NAME];
	seaweed_file_type_t type;
	struct seaweed_attributes attributes;
	struct seaweed_file_chunk *chunks;     /* For regular files */
	uint32_t chunk_count;
	void *extended_attributes;             /* Extended attributes (optional) */
	struct seaweed_entry *next;
};

/**
 * @brief Lookup request
 */
struct seaweed_lookup_request {
	char directory[SEAWEED_MAX_PATH];
	char name[SEAWEED_MAX_FILE_NAME];
};

/**
 * @brief Lookup response
 */
struct seaweed_lookup_response {
	seaweed_status_t status;
	struct seaweed_entry entry;
};

/**
 * @brief Create entry request
 */
struct seaweed_create_request {
	char directory[SEAWEED_MAX_PATH];
	struct seaweed_entry entry;
	bool o_excl;            /* O_EXCL semantics */
	bool skip_parent_check; /* Skip parent directory check */
};

/**
 * @brief Create entry response
 */
struct seaweed_create_response {
	seaweed_status_t status;
	struct seaweed_entry entry;
};

/**
 * @brief List entries request
 */
struct seaweed_list_request {
	char directory[SEAWEED_MAX_PATH];
	char prefix[SEAWEED_MAX_FILE_NAME];         /* Name prefix filter */
	char start_from[SEAWEED_MAX_FILE_NAME];     /* Pagination start */
	bool inclusive_start;                       /* Include start_from file */
	uint32_t limit;                             /* Max entries to return */
};

/**
 * @brief List entries response
 */
struct seaweed_list_response {
	seaweed_status_t status;
	struct seaweed_entry *entries;
	uint32_t count;
	char last_name[SEAWEED_MAX_FILE_NAME];      /* Last entry name */
	bool has_more;                              /* More entries available */
};

/**
 * @brief Delete entry request
 */
struct seaweed_delete_request {
	char directory[SEAWEED_MAX_PATH];
	char name[SEAWEED_MAX_FILE_NAME];
	bool is_recursive;      /* Recursive delete for directories */
	bool ignore_errors;     /* Ignore recursive errors */
	bool delete_chunks;     /* Delete actual data chunks */
};

/**
 * @brief Delete entry response
 */
struct seaweed_delete_response {
	seaweed_status_t status;
};

/**
 * @brief Rename entry request
 */
struct seaweed_rename_request {
	char old_directory[SEAWEED_MAX_PATH];
	char old_name[SEAWEED_MAX_FILE_NAME];
	char new_directory[SEAWEED_MAX_PATH];
	char new_name[SEAWEED_MAX_FILE_NAME];
};

/**
 * @brief Rename entry response
 */
struct seaweed_rename_response {
	seaweed_status_t status;
};

/**
 * @brief Update entry request
 */
struct seaweed_update_request {
	char directory[SEAWEED_MAX_PATH];
	struct seaweed_entry entry;
};

/**
 * @brief Update entry response
 */
struct seaweed_update_response {
	seaweed_status_t status;
};

/**
 * @brief Volume assignment request
 */
struct seaweed_assign_request {
	uint32_t count;         /* Number of file IDs to assign */
	char collection[64];    /* Collection name */
	char replication[8];    /* Replication strategy (e.g., "001") */
	uint32_t ttl_sec;       /* Time to live in seconds */
	char data_center[64];   /* Data center preference */
	char rack[64];          /* Rack preference */
	char data_node[128];    /* Data node preference */
	char disk_type[32];     /* Disk type preference */
};

/**
 * @brief Volume location info
 */
struct seaweed_location {
	char url[SEAWEED_MAX_URL];          /* HTTP URL */
	char public_url[SEAWEED_MAX_URL];   /* Public URL */
	uint32_t grpc_port;                 /* gRPC port */
	char data_center[64];               /* Data center */
	bool data_in_remote;                /* Data is remote */
};

/**
 * @brief Volume assignment response
 */
struct seaweed_assign_response {
	seaweed_status_t status;
	char file_id[SEAWEED_MAX_FILE_ID];  /* Assigned file ID */
	uint32_t count;                     /* Number of IDs assigned */
	char auth_token[256];               /* Authorization token */
	char collection[64];                /* Collection name */
	char replication[8];                /* Replication strategy */
	struct seaweed_location location;   /* Storage location */
};

/**
 * @brief File lock request
 */
struct seaweed_lock_request {
	char name[SEAWEED_MAX_PATH];        /* Lock name (usually file path) */
	uint32_t seconds_to_lock;           /* Lock duration in seconds */
	char renew_token[256];              /* Token for renewal */
	char owner[128];                    /* Lock owner identifier */
	bool is_moved;                      /* Lock has been moved */
};

/**
 * @brief File lock response
 */
struct seaweed_lock_response {
	seaweed_status_t status;
	char renew_token[256];              /* Token for renewal/unlock */
	char lock_owner[128];               /* Current lock owner */
	char moved_to[SEAWEED_MAX_URL];     /* New location if moved */
};

/**
 * @brief File unlock request
 */
struct seaweed_unlock_request {
	char name[SEAWEED_MAX_PATH];        /* Lock name */
	char renew_token[256];              /* Renewal token */
	bool is_moved;                      /* Lock has been moved */
};

/**
 * @brief File unlock response
 */
struct seaweed_unlock_response {
	seaweed_status_t status;
	char moved_to[SEAWEED_MAX_URL];     /* New location if moved */
};

/* Forward declaration */
struct seaweed_filer_connection;

/* API function prototypes */

/**
 * @brief Lookup a directory entry
 */
seaweed_status_t seaweed_filer_lookup_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_lookup_request *req,
					     struct seaweed_lookup_response *resp);

/**
 * @brief Create a new entry (file or directory)
 */
seaweed_status_t seaweed_filer_create_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_create_request *req,
					     struct seaweed_create_response *resp);

/**
 * @brief List directory entries
 */
seaweed_status_t seaweed_filer_list_entries(struct seaweed_filer_connection *conn,
					     const struct seaweed_list_request *req,
					     struct seaweed_list_response *resp);

/**
 * @brief Delete an entry
 */
seaweed_status_t seaweed_filer_delete_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_delete_request *req,
					     struct seaweed_delete_response *resp);

/**
 * @brief Rename an entry
 */
seaweed_status_t seaweed_filer_rename_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_rename_request *req,
					     struct seaweed_rename_response *resp);

/**
 * @brief Update an entry
 */
seaweed_status_t seaweed_filer_update_entry(struct seaweed_filer_connection *conn,
					     const struct seaweed_update_request *req,
					     struct seaweed_update_response *resp);

/**
 * @brief Assign volume for writing
 */
seaweed_status_t seaweed_filer_assign_volume(struct seaweed_filer_connection *conn,
					      const struct seaweed_assign_request *req,
					      struct seaweed_assign_response *resp);

/**
 * @brief Acquire file lock
 */
seaweed_status_t seaweed_filer_lock(struct seaweed_filer_connection *conn,
				    const struct seaweed_lock_request *req,
				    struct seaweed_lock_response *resp);

/**
 * @brief Release file lock
 */
seaweed_status_t seaweed_filer_unlock(struct seaweed_filer_connection *conn,
				      const struct seaweed_unlock_request *req,
				      struct seaweed_unlock_response *resp);

/* Volume server operations (HTTP-based) */

/**
 * @brief Write data to volume server
 */
seaweed_status_t seaweed_volume_write(const struct seaweed_location *location,
				      const char *file_id,
				      const void *data, size_t size,
				      const char *auth_token);

/**
 * @brief Read data from volume server
 */
seaweed_status_t seaweed_volume_read(const struct seaweed_location *location,
				     const char *file_id,
				     uint64_t offset, size_t size,
				     void *buffer, size_t *bytes_read);

/**
 * @brief Delete file from volume server
 */
seaweed_status_t seaweed_volume_delete(const struct seaweed_location *location,
				       const char *file_id,
				       const char *auth_token);

/* Utility functions */

/**
 * @brief Convert SeaweedFS status to string
 */
const char *seaweed_status_to_string(seaweed_status_t status);

/**
 * @brief Parse file ID string
 */
seaweed_status_t seaweed_parse_file_id(const char *file_id_str,
				       struct seaweed_file_id *fid);

/**
 * @brief Format file ID to string
 */
seaweed_status_t seaweed_format_file_id(const struct seaweed_file_id *fid,
					char *file_id_str, size_t str_len);

/**
 * @brief Free entry structure and associated memory
 */
void seaweed_free_entry(struct seaweed_entry *entry);

/**
 * @brief Free list response and associated memory
 */
void seaweed_free_list_response(struct seaweed_list_response *resp);

/**
 * @brief Clone an entry structure
 */
seaweed_status_t seaweed_clone_entry(const struct seaweed_entry *src,
				     struct seaweed_entry *dst);

#endif /* SEAWEED_API_H */