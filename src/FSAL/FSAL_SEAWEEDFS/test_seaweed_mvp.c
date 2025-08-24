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
 * @file test_seaweed_mvp.c
 * @brief Unit tests for SeaweedFS FSAL MVP implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "seaweed_api.h"

/* Test framework macros */
#define TEST_ASSERT(condition, message) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL: %s - %s\n", __func__, message); \
			return -1; \
		} \
	} while (0)

#define TEST_SUCCESS() \
	do { \
		printf("PASS: %s\n", __func__); \
		return 0; \
	} while (0)

#define RUN_TEST(test_func) \
	do { \
		printf("Running %s...\n", #test_func); \
		if (test_func() != 0) { \
			tests_failed++; \
		} else { \
			tests_passed++; \
		} \
		total_tests++; \
	} while (0)

/* Global test counters */
static int total_tests = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/**
 * @brief Test SeaweedFS status string conversion
 */
static int test_status_to_string(void)
{
	const char *str;

	str = seaweed_status_to_string(SEAWEED_OK);
	TEST_ASSERT(strcmp(str, "OK") == 0, "SEAWEED_OK should return 'OK'");

	str = seaweed_status_to_string(SEAWEED_ERROR_NOT_FOUND);
	TEST_ASSERT(strcmp(str, "Not Found") == 0, "NOT_FOUND should return 'Not Found'");

	str = seaweed_status_to_string(999); /* Invalid status */
	TEST_ASSERT(strcmp(str, "Unknown Error") == 0, "Invalid status should return 'Unknown Error'");

	TEST_SUCCESS();
}

/**
 * @brief Test file ID parsing and formatting
 */
static int test_file_id_operations(void)
{
	struct seaweed_file_id fid;
	char file_id_str[64];
	seaweed_status_t status;

	/* Test parsing */
	status = seaweed_parse_file_id("123,456789,999", &fid);
	TEST_ASSERT(status == SEAWEED_OK, "Valid file ID should parse successfully");
	TEST_ASSERT(fid.volume_id == 123, "Volume ID should be 123");
	TEST_ASSERT(fid.needle_id == 456789, "Needle ID should be 456789");
	TEST_ASSERT(fid.cookie == 999, "Cookie should be 999");

	/* Test invalid parsing */
	status = seaweed_parse_file_id("invalid", &fid);
	TEST_ASSERT(status == SEAWEED_ERROR_INVALID_ARGUMENT, "Invalid format should fail");

	status = seaweed_parse_file_id(NULL, &fid);
	TEST_ASSERT(status == SEAWEED_ERROR_INVALID_ARGUMENT, "NULL input should fail");

	/* Test formatting */
	fid.volume_id = 111;
	fid.needle_id = 222333;
	fid.cookie = 444;

	status = seaweed_format_file_id(&fid, file_id_str, sizeof(file_id_str));
	TEST_ASSERT(status == SEAWEED_OK, "Formatting should succeed");
	TEST_ASSERT(strcmp(file_id_str, "111,222333,444") == 0, "Formatted string should match expected");

	/* Test buffer too small */
	status = seaweed_format_file_id(&fid, file_id_str, 5);
	TEST_ASSERT(status == SEAWEED_ERROR_INVALID_ARGUMENT, "Small buffer should fail");

	TEST_SUCCESS();
}

/**
 * @brief Test entry memory management
 */
static int test_entry_memory_management(void)
{
	struct seaweed_entry src_entry, dst_entry;
	struct seaweed_file_chunk *chunk1, *chunk2;
	seaweed_status_t status;

	/* Initialize source entry */
	memset(&src_entry, 0, sizeof(src_entry));
	strcpy(src_entry.name, "test_file.txt");
	src_entry.type = SEAWEED_FILE_TYPE_REGULAR;
	src_entry.attributes.file_size = 1024;
	src_entry.attributes.inode = 12345;
	src_entry.chunk_count = 2;

	/* Create some chunks */
	chunk1 = malloc(sizeof(struct seaweed_file_chunk));
	TEST_ASSERT(chunk1 != NULL, "Chunk allocation should succeed");
	memset(chunk1, 0, sizeof(struct seaweed_file_chunk));
	chunk1->fid.volume_id = 1;
	chunk1->fid.needle_id = 1001;
	chunk1->offset = 0;
	chunk1->size = 512;

	chunk2 = malloc(sizeof(struct seaweed_file_chunk));
	TEST_ASSERT(chunk2 != NULL, "Chunk allocation should succeed");
	memset(chunk2, 0, sizeof(struct seaweed_file_chunk));
	chunk2->fid.volume_id = 1;
	chunk2->fid.needle_id = 1002;
	chunk2->offset = 512;
	chunk2->size = 512;

	/* Link chunks */
	src_entry.chunks = chunk1;
	chunk1->next = chunk2;
	chunk2->next = NULL;

	/* Test cloning */
	memset(&dst_entry, 0, sizeof(dst_entry));
	status = seaweed_clone_entry(&src_entry, &dst_entry);
	TEST_ASSERT(status == SEAWEED_OK, "Entry cloning should succeed");
	TEST_ASSERT(strcmp(dst_entry.name, "test_file.txt") == 0, "Name should be cloned");
	TEST_ASSERT(dst_entry.type == SEAWEED_FILE_TYPE_REGULAR, "Type should be cloned");
	TEST_ASSERT(dst_entry.attributes.file_size == 1024, "File size should be cloned");
	TEST_ASSERT(dst_entry.chunk_count == 2, "Chunk count should be cloned");
	TEST_ASSERT(dst_entry.chunks != NULL, "Chunks should be cloned");
	TEST_ASSERT(dst_entry.chunks != src_entry.chunks, "Chunks should be deep copied");

	/* Test freeing */
	seaweed_free_entry(&dst_entry);
	TEST_ASSERT(dst_entry.chunks == NULL, "Chunks should be freed");
	TEST_ASSERT(dst_entry.chunk_count == 0, "Chunk count should be reset");

	/* Clean up source (manually since we allocated chunks) */
	free(chunk1);
	free(chunk2);

	TEST_SUCCESS();
}

/**
 * @brief Test basic Filer API operations
 */
static int test_filer_operations(void)
{
	struct seaweed_lookup_request lookup_req;
	struct seaweed_lookup_response lookup_resp;
	struct seaweed_create_request create_req;
	struct seaweed_create_response create_resp;
	struct seaweed_list_request list_req;
	struct seaweed_list_response list_resp;
	seaweed_status_t status;

	/* Test root lookup (should succeed due to mock root) */
	memset(&lookup_req, 0, sizeof(lookup_req));
	memset(&lookup_resp, 0, sizeof(lookup_resp));
	strcpy(lookup_req.directory, "/");
	strcpy(lookup_req.name, "/");

	status = seaweed_filer_lookup_entry(NULL, &lookup_req, &lookup_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Root lookup should succeed");
	TEST_ASSERT(lookup_resp.entry.type == SEAWEED_FILE_TYPE_DIRECTORY, "Root should be directory");

	/* Test creating a file */
	memset(&create_req, 0, sizeof(create_req));
	memset(&create_resp, 0, sizeof(create_resp));
	strcpy(create_req.directory, "/");
	strcpy(create_req.entry.name, "test_file.txt");
	create_req.entry.type = SEAWEED_FILE_TYPE_REGULAR;
	create_req.entry.attributes.file_mode = 0644;
	create_req.entry.attributes.file_size = 0;
	create_req.o_excl = true;

	status = seaweed_filer_create_entry(NULL, &create_req, &create_resp);
	TEST_ASSERT(status == SEAWEED_OK, "File creation should succeed");
	TEST_ASSERT(create_resp.entry.type == SEAWEED_FILE_TYPE_REGULAR, "Created file should be regular");

	/* Test creating the same file again (should fail with O_EXCL) */
	status = seaweed_filer_create_entry(NULL, &create_req, &create_resp);
	TEST_ASSERT(status == SEAWEED_ERROR_ALREADY_EXISTS, "Duplicate creation should fail");

	/* Test looking up the created file */
	memset(&lookup_req, 0, sizeof(lookup_req));
	memset(&lookup_resp, 0, sizeof(lookup_resp));
	strcpy(lookup_req.directory, "/");
	strcpy(lookup_req.name, "test_file.txt");

	status = seaweed_filer_lookup_entry(NULL, &lookup_req, &lookup_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Created file lookup should succeed");
	TEST_ASSERT(strcmp(lookup_resp.entry.name, "test_file.txt") == 0, "File name should match");

	/* Test listing directory */
	memset(&list_req, 0, sizeof(list_req));
	memset(&list_resp, 0, sizeof(list_resp));
	strcpy(list_req.directory, "/");
	list_req.limit = 10;

	status = seaweed_filer_list_entries(NULL, &list_req, &list_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Directory listing should succeed");
	TEST_ASSERT(list_resp.count >= 1, "Should find at least the created file");

	/* Clean up */
	seaweed_free_entry(&lookup_resp.entry);
	seaweed_free_entry(&create_resp.entry);
	seaweed_free_list_response(&list_resp);

	TEST_SUCCESS();
}

/**
 * @brief Test file locking functionality
 */
static int test_file_locking(void)
{
	struct seaweed_lock_request lock_req;
	struct seaweed_lock_response lock_resp;
	struct seaweed_unlock_request unlock_req;
	struct seaweed_unlock_response unlock_resp;
	seaweed_status_t status;

	/* Test acquiring a lock */
	memset(&lock_req, 0, sizeof(lock_req));
	memset(&lock_resp, 0, sizeof(lock_resp));
	strcpy(lock_req.name, "/test_lock_file");
	lock_req.seconds_to_lock = 60;
	strcpy(lock_req.owner, "test_owner_1");

	status = seaweed_filer_lock(NULL, &lock_req, &lock_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Lock acquisition should succeed");
	TEST_ASSERT(strlen(lock_resp.renew_token) > 0, "Should receive a renew token");

	/* Test acquiring the same lock again (should fail) */
	struct seaweed_lock_request lock_req2;
	struct seaweed_lock_response lock_resp2;
	memset(&lock_req2, 0, sizeof(lock_req2));
	memset(&lock_resp2, 0, sizeof(lock_resp2));
	strcpy(lock_req2.name, "/test_lock_file");
	lock_req2.seconds_to_lock = 60;
	strcpy(lock_req2.owner, "test_owner_2");

	status = seaweed_filer_lock(NULL, &lock_req2, &lock_resp2);
	TEST_ASSERT(status == SEAWEED_ERROR_ALREADY_EXISTS, "Duplicate lock should fail");

	/* Test unlocking */
	memset(&unlock_req, 0, sizeof(unlock_req));
	memset(&unlock_resp, 0, sizeof(unlock_resp));
	strcpy(unlock_req.name, "/test_lock_file");
	strcpy(unlock_req.renew_token, lock_resp.renew_token);

	status = seaweed_filer_unlock(NULL, &unlock_req, &unlock_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Lock release should succeed");

	/* Test acquiring the lock again after release (should succeed now) */
	status = seaweed_filer_lock(NULL, &lock_req2, &lock_resp2);
	TEST_ASSERT(status == SEAWEED_OK, "Lock after release should succeed");

	/* Clean up */
	memset(&unlock_req, 0, sizeof(unlock_req));
	strcpy(unlock_req.name, "/test_lock_file");
	strcpy(unlock_req.renew_token, lock_resp2.renew_token);
	seaweed_filer_unlock(NULL, &unlock_req, &unlock_resp);

	TEST_SUCCESS();
}

/**
 * @brief Test volume operations
 */
static int test_volume_operations(void)
{
	struct seaweed_assign_request assign_req;
	struct seaweed_assign_response assign_resp;
	struct seaweed_location location;
	char buffer[1024];
	size_t bytes_read;
	seaweed_status_t status;

	/* Test volume assignment */
	memset(&assign_req, 0, sizeof(assign_req));
	memset(&assign_resp, 0, sizeof(assign_resp));
	assign_req.count = 1;
	strcpy(assign_req.collection, "test_collection");
	strcpy(assign_req.replication, "001");

	status = seaweed_filer_assign_volume(NULL, &assign_req, &assign_resp);
	TEST_ASSERT(status == SEAWEED_OK, "Volume assignment should succeed");
	TEST_ASSERT(strlen(assign_resp.file_id) > 0, "Should receive a file ID");
	TEST_ASSERT(assign_resp.count == 1, "Should assign requested count");

	/* Test volume operations (mock implementation) */
	location = assign_resp.location;
	
	/* Test write */
	status = seaweed_volume_write(&location, assign_resp.file_id, 
				      "test data", 9, "test_token");
	TEST_ASSERT(status == SEAWEED_OK, "Volume write should succeed");

	/* Test read */
	status = seaweed_volume_read(&location, assign_resp.file_id, 
				     0, sizeof(buffer), buffer, &bytes_read);
	TEST_ASSERT(status == SEAWEED_OK, "Volume read should succeed");
	TEST_ASSERT(bytes_read > 0, "Should read some data");

	/* Test delete */
	status = seaweed_volume_delete(&location, assign_resp.file_id, "test_token");
	TEST_ASSERT(status == SEAWEED_OK, "Volume delete should succeed");

	TEST_SUCCESS();
}

/**
 * @brief Main test runner
 */
int main(void)
{
	printf("========================================\n");
	printf("SeaweedFS FSAL MVP Test Suite\n");
	printf("========================================\n\n");

	/* Run all tests */
	RUN_TEST(test_status_to_string);
	RUN_TEST(test_file_id_operations);
	RUN_TEST(test_entry_memory_management);
	RUN_TEST(test_filer_operations);
	RUN_TEST(test_file_locking);
	RUN_TEST(test_volume_operations);

	/* Print results */
	printf("\n========================================\n");
	printf("Test Results:\n");
	printf("Total tests: %d\n", total_tests);
	printf("Passed: %d\n", tests_passed);
	printf("Failed: %d\n", tests_failed);
	printf("Success rate: %.1f%%\n", 
	       total_tests > 0 ? (100.0 * tests_passed / total_tests) : 0.0);
	printf("========================================\n");

	return tests_failed > 0 ? 1 : 0;
}