/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * SeaweedFS FSAL Real API Test
 * 
 * This program tests the SeaweedFS FSAL Phase 8 implementation with real HTTP API calls
 * against a real SeaweedFS instance (not MVP mode).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>

/* Disable MVP test mode to enable real API calls */
#undef SEAWEED_MVP_TEST

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

/* SeaweedFS endpoint configuration */
#define SEAWEEDFS_FILER_ENDPOINT "http://127.0.0.1:8333"

/* Mock connection structure for testing */
struct test_connection {
    char endpoints[1][256];  /* Single endpoint for testing */
} test_conn = {
    .endpoints = {SEAWEEDFS_FILER_ENDPOINT}
};

/* Test real HTTP API connectivity */
static int test_seaweedfs_real_connectivity(void)
{
    CURL *curl;
    CURLcode res;
    long response_code;
    
    curl = curl_easy_init();
    TEST_ASSERT(curl != NULL, "Failed to initialize cURL");
    
    /* Test the /cluster/status endpoint */
    curl_easy_setopt(curl, CURLOPT_URL, SEAWEEDFS_FILER_ENDPOINT "/cluster/status");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);
    
    printf("SeaweedFS response code: %ld\n", response_code);
    
    TEST_ASSERT(res == CURLE_OK, "Failed to connect to SeaweedFS");
    TEST_ASSERT(response_code == 200, "SeaweedFS returned non-200 status");
    
    TEST_SUCCESS();
}

/* Test real file creation via HTTP API */
static int test_real_file_creation(void)
{
    struct seaweed_create_request create_req;
    struct seaweed_create_response create_resp;
    seaweed_status_t status;

    printf("Testing real file creation via HTTP API...\n");

    /* Test: Create a test file */
    memset(&create_req, 0, sizeof(create_req));
    memset(&create_resp, 0, sizeof(create_resp));
    strcpy(create_req.directory, "/");
    strcpy(create_req.entry.name, "phase8_test_file.txt");
    create_req.entry.type = SEAWEED_FILE_TYPE_REGULAR;
    create_req.entry.attributes.file_mode = 0644;
    create_req.entry.attributes.file_size = 0;
    create_req.entry.attributes.uid = 1000;
    create_req.entry.attributes.gid = 1000;
    create_req.o_excl = true;

    status = seaweed_filer_create_entry(NULL, &create_req, &create_resp);
    TEST_ASSERT(status == SEAWEED_OK, "Real file creation should succeed");
    TEST_ASSERT(strcmp(create_resp.entry.name, "phase8_test_file.txt") == 0, 
                "Created file name should match");

    printf("Successfully created file: %s\n", create_resp.entry.name);

    TEST_SUCCESS();
}

/* Test real directory creation via HTTP API */
static int test_real_directory_creation(void)
{
    struct seaweed_create_request create_req;
    struct seaweed_create_response create_resp;
    seaweed_status_t status;

    printf("Testing real directory creation via HTTP API...\n");

    /* Test: Create a test directory */
    memset(&create_req, 0, sizeof(create_req));
    memset(&create_resp, 0, sizeof(create_resp));
    strcpy(create_req.directory, "/");
    strcpy(create_req.entry.name, "phase8_test_dir");
    create_req.entry.type = SEAWEED_FILE_TYPE_DIRECTORY;
    create_req.entry.attributes.file_mode = 0755;
    create_req.entry.attributes.uid = 1000;
    create_req.entry.attributes.gid = 1000;

    status = seaweed_filer_create_entry(NULL, &create_req, &create_resp);
    TEST_ASSERT(status == SEAWEED_OK, "Real directory creation should succeed");
    TEST_ASSERT(create_resp.entry.type == SEAWEED_FILE_TYPE_DIRECTORY, 
                "Created entry should be directory");

    printf("Successfully created directory: %s\n", create_resp.entry.name);

    TEST_SUCCESS();
}

/* Test real volume operations */
static int test_real_volume_operations(void)
{
    struct seaweed_assign_request assign_req;
    struct seaweed_assign_response assign_resp;
    seaweed_status_t status;
    char test_data[] = "Hello Phase 8!";
    char read_buffer[256];
    size_t bytes_read;

    printf("Testing real volume read/write operations...\n");

    /* Test volume assignment */
    memset(&assign_req, 0, sizeof(assign_req));
    memset(&assign_resp, 0, sizeof(assign_resp));
    assign_req.count = 1;
    strcpy(assign_req.collection, "phase8_test");
    strcpy(assign_req.replication, "001");

    status = seaweed_filer_assign_volume(NULL, &assign_req, &assign_resp);
    TEST_ASSERT(status == SEAWEED_OK, "Volume assignment should succeed");
    TEST_ASSERT(strlen(assign_resp.file_id) > 0, "Should receive a file ID");

    printf("Assigned file ID: %s\n", assign_resp.file_id);
    printf("Volume location: %s\n", assign_resp.location.url);

    /* Test real volume write */
    status = seaweed_volume_write(&assign_resp.location, assign_resp.file_id, 
                                  test_data, strlen(test_data), NULL);
    TEST_ASSERT(status == SEAWEED_OK, "Real volume write should succeed");

    printf("Successfully wrote %zu bytes to volume\n", strlen(test_data));

    /* Test real volume read */
    memset(read_buffer, 0, sizeof(read_buffer));
    status = seaweed_volume_read(&assign_resp.location, assign_resp.file_id, 
                                 0, sizeof(read_buffer), read_buffer, &bytes_read);
    TEST_ASSERT(status == SEAWEED_OK, "Real volume read should succeed");
    TEST_ASSERT(bytes_read == strlen(test_data), "Should read back same number of bytes");
    TEST_ASSERT(strcmp(read_buffer, test_data) == 0, "Read data should match written data");

    printf("Successfully read back %zu bytes: '%s'\n", bytes_read, read_buffer);

    TEST_SUCCESS();
}

/* Test error handling with real API */
static int test_real_error_handling(void)
{
    struct seaweed_create_request create_req;
    struct seaweed_create_response create_resp;
    seaweed_status_t status;

    printf("Testing real API error handling...\n");

    /* Test duplicate file creation (should fail) */
    memset(&create_req, 0, sizeof(create_req));
    memset(&create_resp, 0, sizeof(create_resp));
    strcpy(create_req.directory, "/");
    strcpy(create_req.entry.name, "phase8_test_file.txt");  /* Same as before */
    create_req.entry.type = SEAWEED_FILE_TYPE_REGULAR;
    create_req.entry.attributes.file_mode = 0644;
    create_req.o_excl = true;

    status = seaweed_filer_create_entry(NULL, &create_req, &create_resp);
    TEST_ASSERT(status == SEAWEED_ERROR_ALREADY_EXISTS || status == SEAWEED_ERROR_IO, 
                "Duplicate file creation should fail appropriately");

    printf("Duplicate creation properly rejected with status: %s\n", 
           seaweed_status_to_string(status));

    TEST_SUCCESS();
}

/* Check if SeaweedFS is accessible */
static int check_seaweedfs_accessibility(void)
{
    CURL *curl;
    CURLcode res;
    
    curl = curl_easy_init();
    if (!curl) {
        return 0;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, SEAWEEDFS_FILER_ENDPOINT "/cluster/status");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK) ? 1 : 0;
}

/* Main test runner */
int main(void)
{
    printf("========================================\n");
    printf("SeaweedFS FSAL Phase 8 Real API Test\n");
    printf("========================================\n\n");

    /* Initialize cURL */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Check if SeaweedFS is accessible */
    printf("Checking SeaweedFS accessibility at %s...\n", SEAWEEDFS_FILER_ENDPOINT);
    if (check_seaweedfs_accessibility()) {
        printf("✓ SeaweedFS is accessible\n\n");
    } else {
        printf("❌ SeaweedFS is not accessible at %s\n", SEAWEEDFS_FILER_ENDPOINT);
        printf("Please ensure your SeaweedFS docker compose is running\n");
        return 1;
    }

    /* Run real API tests */
    RUN_TEST(test_seaweedfs_real_connectivity);
    RUN_TEST(test_real_file_creation);
    RUN_TEST(test_real_directory_creation);
    RUN_TEST(test_real_volume_operations);
    RUN_TEST(test_real_error_handling);

    /* Cleanup */
    curl_global_cleanup();

    /* Print results */
    printf("\n========================================\n");
    printf("Phase 8 Real API Test Results:\n");
    printf("Total tests: %d\n", total_tests);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Success rate: %.1f%%\n", 
           total_tests > 0 ? (100.0 * tests_passed / total_tests) : 0.0);
    printf("========================================\n");

    if (tests_failed == 0) {
        printf("🎉 All Phase 8 real API tests passed!\n");
        printf("\nPhase 8 Implementation Status:\n");
        printf("✓ Real HTTP API calls for volume read/write\n");
        printf("✓ Real HTTP API calls for file/directory creation\n");
        printf("✓ Proper error handling with real SeaweedFS responses\n");
        printf("✓ Ready for full NFS-Ganesha integration testing\n");
        return 0;
    } else {
        printf("❌ Some Phase 8 tests failed\n");
        return 1;
    }
}