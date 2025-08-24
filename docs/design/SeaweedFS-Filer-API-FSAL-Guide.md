# SeaweedFS Filer API 梳理 - Ganesha FSAL 实现指南

## 1. 概述

SeaweedFS Filer 提供完整的分布式文件系统接口，支持 POSIX 语义、分布式锁和高性能 I/O 操作。本文档为实现 NFS-Ganesha FSAL SeaweedFS 层提供完整的 API 参考。

## 2. API 架构

SeaweedFS Filer 提供双重 API 接口：
- **gRPC API**: 高性能二进制协议，推荐用于 FSAL 实现
- **REST API**: HTTP 接口，便于调试和集成测试

## 3. gRPC API 详解

### 3.1 服务定义

```protobuf
service SeaweedFiler {
    // 文件系统基础操作
    rpc LookupDirectoryEntry (LookupDirectoryEntryRequest) returns (LookupDirectoryEntryResponse);
    rpc ListEntries (ListEntriesRequest) returns (stream ListEntriesResponse);
    rpc CreateEntry (CreateEntryRequest) returns (CreateEntryResponse);
    rpc UpdateEntry (UpdateEntryRequest) returns (UpdateEntryResponse);
    rpc AppendToEntry (AppendToEntryRequest) returns (AppendToEntryResponse);
    rpc DeleteEntry (DeleteEntryRequest) returns (DeleteEntryResponse);
    rpc AtomicRenameEntry (AtomicRenameEntryRequest) returns (AtomicRenameEntryResponse);
    
    // 存储管理
    rpc AssignVolume (AssignVolumeRequest) returns (AssignVolumeResponse);
    rpc LookupVolume (LookupVolumeRequest) returns (LookupVolumeResponse);
    
    // 分布式锁
    rpc DistributedLock(LockRequest) returns (LockResponse);
    rpc DistributedUnlock(UnlockRequest) returns (UnlockResponse);
    rpc FindLockOwner(FindLockOwnerRequest) returns (FindLockOwnerResponse);
    
    // 键值存储
    rpc KvGet (KvGetRequest) returns (KvGetResponse);
    rpc KvPut (KvPutRequest) returns (KvPutResponse);
    
    // 元数据订阅
    rpc SubscribeMetadata (SubscribeMetadataRequest) returns (stream SubscribeMetadataResponse);
}
```

### 3.2 核心数据结构

#### Entry 结构
```protobuf
message Entry {
    string name = 1;                    // 文件/目录名
    bool is_directory = 2;              // 是否为目录
    repeated FileChunk chunks = 3;      // 文件数据块列表
    FuseAttributes attributes = 4;      // FUSE 属性
    map<string, bytes> extended = 5;    // 扩展属性
    bytes hard_link_id = 7;            // 硬链接 ID
    int32 hard_link_counter = 8;       // 硬链接计数
    bytes content = 9;                 // 小文件内容（<8KB）
    int64 quota = 11;                  // 配额限制
    int64 worm_enforced_at_ts_ns = 12; // WORM 强制时间
}
```

#### FUSE 属性结构
```protobuf
message FuseAttributes {
    uint64 file_size = 1;        // 文件大小
    int64 mtime = 2;             // 修改时间 (unix 时间戳秒)
    uint32 file_mode = 3;        // 文件权限模式 (类似 st_mode)
    uint32 uid = 4;              // 用户 ID
    uint32 gid = 5;              // 组 ID
    int64 crtime = 6;            // 创建时间
    string mime = 7;             // MIME 类型
    int32 ttl_sec = 10;          // 生存时间
    string user_name = 11;       // 用户名 (for HDFS)
    repeated string group_name = 12; // 组名列表
    string symlink_target = 13;  // 符号链接目标
    bytes md5 = 14;              // MD5 校验和
    uint32 rdev = 16;            // 设备号
    uint64 inode = 17;           // inode 号
}
```

#### 文件块结构
```protobuf
message FileChunk {
    string file_id = 1;          // 文件 ID (已废弃)
    int64 offset = 2;            // 在文件中的偏移量
    uint64 size = 3;             // 块大小
    int64 modified_ts_ns = 4;    // 修改时间戳 (纳秒)
    string e_tag = 5;            // ETag
    FileId fid = 7;              // 新的文件 ID 结构
    bytes cipher_key = 9;        // 加密密钥
    bool is_compressed = 10;     // 是否压缩
    bool is_chunk_manifest = 11; // 是否为块清单
}
```

### 3.3 文件系统操作 API

#### 查找目录项
```protobuf
message LookupDirectoryEntryRequest {
    string directory = 1;  // 父目录路径
    string name = 2;       // 文件/目录名
}

message LookupDirectoryEntryResponse {
    Entry entry = 1;       // 返回的目录项
}
```

**FSAL 映射**: `fsal_lookup()`

#### 列出目录内容
```protobuf
message ListEntriesRequest {
    string directory = 1;           // 目录路径
    string prefix = 2;              // 名称前缀过滤
    string startFromFileName = 3;   // 分页起始文件名
    bool inclusiveStartFrom = 4;    // 是否包含起始文件
    uint32 limit = 5;               // 每页限制数量
}

message ListEntriesResponse {
    Entry entry = 1;        // 目录项
}
```

**FSAL 映射**: `fsal_readdir()`

#### 创建目录项
```protobuf
message CreateEntryRequest {
    string directory = 1;                 // 父目录路径
    Entry entry = 2;                      // 要创建的目录项
    bool o_excl = 3;                      // O_EXCL 语义
    bool is_from_other_cluster = 4;       // 来自其他集群
    repeated int32 signatures = 5;        // 签名
    bool skip_check_parent_directory = 6; // 跳过父目录检查
}
```

**FSAL 映射**: `fsal_create()`, `fsal_mkdir()`

#### 更新目录项
```protobuf
message UpdateEntryRequest {
    string directory = 1;           // 父目录路径
    Entry entry = 2;                // 更新的目录项
    bool is_from_other_cluster = 3; // 来自其他集群
    repeated int32 signatures = 4;  // 签名
}
```

**FSAL 映射**: `fsal_setattrs()`, `fsal_close()` (更新文件大小)

#### 删除目录项
```protobuf
message DeleteEntryRequest {
    string directory = 1;             // 父目录路径
    string name = 2;                  // 文件/目录名
    bool is_delete_data = 4;          // 是否删除实际数据
    bool is_recursive = 5;            // 递归删除
    bool ignore_recursive_error = 6;  // 忽略递归错误
    bool is_from_other_cluster = 7;   // 来自其他集群
    repeated int32 signatures = 8;    // 签名
    int64 if_not_modified_after = 9;  // 条件删除时间戳
}
```

**FSAL 映射**: `fsal_remove()`, `fsal_rmdir()`

#### 原子重命名
```protobuf
message AtomicRenameEntryRequest {
    string old_directory = 1;  // 原父目录
    string old_name = 2;       // 原文件名
    string new_directory = 3;  // 新父目录
    string new_name = 4;       // 新文件名
    repeated int32 signatures = 5; // 签名
}
```

**FSAL 映射**: `fsal_rename()`

#### 追加文件内容
```protobuf
message AppendToEntryRequest {
    string directory = 1;              // 目录路径
    string entry_name = 2;             // 文件名
    repeated FileChunk chunks = 3;     // 要追加的数据块
}
```

**FSAL 映射**: `fsal_write()` (追加写入模式)

### 3.4 存储管理 API

#### 分配存储卷
```protobuf
message AssignVolumeRequest {
    int32 count = 1;        // 请求的卷数量
    string collection = 2;  // 集合名
    string replication = 3; // 复制策略 (如 "001", "010")
    int32 ttl_sec = 4;      // 生存时间 (秒)
    string data_center = 5; // 数据中心
    string path = 6;        // 文件路径 (用于路径相关配置)
    string rack = 7;        // 机架
    string data_node = 9;   // 数据节点
    string disk_type = 8;   // 磁盘类型
}

message AssignVolumeResponse {
    string file_id = 1;     // 分配的文件 ID
    int32 count = 4;        // 实际分配数量
    string auth = 5;        // 认证令牌
    string collection = 6;  // 集合名
    string replication = 7; // 复制策略
    string error = 8;       // 错误信息
    Location location = 9;  // 存储位置
}
```

#### 查找卷位置
```protobuf
message LookupVolumeRequest {
    repeated string volume_ids = 1; // 卷 ID 列表
}

message LookupVolumeResponse {
    map<string, Locations> locations_map = 1; // 卷位置映射
}

message Location {
    string url = 1;           // HTTP URL
    string public_url = 2;    // 公共 URL
    uint32 grpc_port = 3;     // gRPC 端口
    string data_center = 4;   // 数据中心
    bool data_in_remote = 5;  // 数据是否在远程
}
```

### 3.5 分布式锁 API

#### 获取分布式锁
```protobuf
message LockRequest {
    string name = 1;              // 锁名称 (建议使用文件路径)
    int64 seconds_to_lock = 2;    // 锁定时长 (秒)
    string renew_token = 3;       // 续期令牌
    bool is_moved = 4;            // 锁是否已迁移
    string owner = 5;             // 锁拥有者标识
}

message LockResponse {
    string renew_token = 1;       // 续期令牌
    string lock_owner = 2;        // 锁拥有者
    string lock_host_moved_to = 3; // 锁迁移到的主机
    string error = 4;             // 错误信息
}
```

**FSAL 映射**: `fsal_lock_op()` (F_SETLK, F_SETLKW)

#### 释放分布式锁
```protobuf
message UnlockRequest {
    string name = 1;        // 锁名称
    string renew_token = 2; // 续期令牌
    bool is_moved = 3;      // 锁是否已迁移
}

message UnlockResponse {
    string error = 1;    // 错误信息
    string moved_to = 2; // 迁移目标
}
```

**FSAL 映射**: `fsal_lock_op()` (F_UNLCK)

#### 查找锁拥有者
```protobuf
message FindLockOwnerRequest {
    string name = 1;     // 锁名称
    bool is_moved = 2;   // 锁是否已迁移
}

message FindLockOwnerResponse {
    string owner = 1;    // 锁拥有者
}
```

### 3.6 键值存储 API

```protobuf
message KvGetRequest {
    bytes key = 1;       // 键
}

message KvGetResponse {
    bytes value = 1;     // 值
    string error = 2;    // 错误信息
}

message KvPutRequest {
    bytes key = 1;       // 键
    bytes value = 2;     // 值 (空值表示删除)
}

message KvPutResponse {
    string error = 1;    // 错误信息
}
```

**用途**: 存储元数据、扩展属性等

### 3.7 元数据订阅 API

```protobuf
message SubscribeMetadataRequest {
    string client_name = 1;          // 客户端名称
    string path_prefix = 2;          // 路径前缀
    int64 since_ns = 3;              // 起始时间戳 (纳秒)
    int32 signature = 4;             // 签名
    repeated string path_prefixes = 6; // 多个路径前缀
    int32 client_id = 7;             // 客户端 ID
    int64 until_ns = 8;              // 结束时间戳
    int32 client_epoch = 9;          // 客户端周期
    repeated string directories = 10; // 确切的目录列表
}

message SubscribeMetadataResponse {
    string directory = 1;             // 目录路径
    EventNotification event_notification = 2; // 事件通知
    int64 ts_ns = 3;                 // 时间戳 (纳秒)
}
```

**用途**: 实现文件系统变更通知

## 4. REST API 详解

### 4.1 HTTP 方法映射

| HTTP 方法 | 功能 | 端点示例 | 对应 gRPC |
|-----------|------|----------|-----------|
| GET | 读取文件内容 | `GET /path/to/file` | 通过 chunks 读取 |
| GET | 列出目录 | `GET /path/to/dir/` | ListEntries |
| HEAD | 获取文件元数据 | `HEAD /path/to/file` | LookupDirectoryEntry |
| POST/PUT | 上传文件 | `POST /path/to/file` | AssignVolume + CreateEntry |
| DELETE | 删除文件/目录 | `DELETE /path/to/file` | DeleteEntry |

### 4.2 请求参数

#### 上传参数
- `collection`: 集合名
- `replication`: 复制策略 (如 "001")
- `ttl`: 生存时间
- `disk`: 磁盘类型
- `fsync`: 强制同步 (true/false)
- `dataCenter`: 数据中心
- `rack`: 机架
- `dataNode`: 数据节点

#### 查询参数
- `limit`: 分页限制
- `lastFileName`: 分页起始文件名
- `recursive`: 递归删除 (DELETE 时)

### 4.3 响应格式

#### 目录列表 JSON 响应
```json
{
    "Path": "/path/to/directory",
    "Entries": [
        {
            "FullPath": "/path/to/directory/file1.txt",
            "Mtime": "2024-01-01T10:00:00Z",
            "Crtime": "2024-01-01T09:00:00Z",
            "Mode": 33188,
            "Uid": 1000,
            "Gid": 1000,
            "Mime": "text/plain",
            "Size": 1024,
            "Chunks": [...]
        }
    ],
    "Limit": 100,
    "LastFileName": "file1.txt",
    "ShouldDisplayLoadMore": true
}
```

#### 错误响应
```json
{
    "error": "file not found"
}
```

### 4.4 HTTP 头部

#### 请求头
- `Range`: 支持范围请求
- `If-Modified-Since`: 条件请求
- `If-None-Match`: ETag 条件请求
- `Content-Type`: multipart/form-data (上传时)

#### 响应头
- `ETag`: 文件 ETag
- `Last-Modified`: 最后修改时间
- `Content-Type`: MIME 类型
- `Content-Length`: 内容长度
- `Accept-Ranges`: bytes (支持范围请求)

## 5. FSAL 实现映射

### 5.1 核心接口映射表

| FSAL 操作 | gRPC API | 说明 |
|-----------|----------|------|
| `fsal_lookup()` | `LookupDirectoryEntry` | 查找文件/目录 |
| `fsal_readdir()` | `ListEntries` | 读取目录内容 |
| `fsal_create()` | `CreateEntry` | 创建文件 |
| `fsal_mkdir()` | `CreateEntry` (is_directory=true) | 创建目录 |
| `fsal_open()` | `LookupDirectoryEntry` | 打开文件 |
| `fsal_read()` | Volume Server HTTP GET | 读取文件数据 |
| `fsal_write()` | `AssignVolume` + Volume Server HTTP POST | 写入文件数据 |
| `fsal_close()` | `UpdateEntry` | 关闭文件，更新元数据 |
| `fsal_remove()` | `DeleteEntry` | 删除文件 |
| `fsal_rmdir()` | `DeleteEntry` (is_recursive=true) | 删除目录 |
| `fsal_rename()` | `AtomicRenameEntry` | 重命名 |
| `fsal_getattrs()` | Entry.attributes | 获取文件属性 |
| `fsal_setattrs()` | `UpdateEntry` | 设置文件属性 |
| `fsal_symlink()` | `CreateEntry` + symlink_target | 创建符号链接 |
| `fsal_readlink()` | Entry.attributes.symlink_target | 读取符号链接 |
| `fsal_lock_op()` | `DistributedLock`/`DistributedUnlock` | 文件锁定 |

### 5.2 数据结构映射

#### stat 结构映射
```c
struct stat {
    .st_ino = entry.attributes.inode,
    .st_mode = entry.attributes.file_mode,
    .st_uid = entry.attributes.uid,
    .st_gid = entry.attributes.gid,
    .st_size = entry.attributes.file_size,
    .st_mtime = entry.attributes.mtime,
    .st_ctime = entry.attributes.crtime,
    .st_rdev = entry.attributes.rdev
};
```

#### 文件锁映射
```c
struct flock {
    .l_type = F_RDLCK/F_WRLCK/F_UNLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,  // SeaweedFS 支持文件级锁
    .l_len = 0     // 整个文件
};
```

### 5.3 错误码映射

| gRPC 状态 | POSIX 错误码 | 说明 |
|-----------|--------------|------|
| `GRPC_STATUS_OK` | 0 | 成功 |
| `GRPC_STATUS_NOT_FOUND` | `ENOENT` | 文件不存在 |
| `GRPC_STATUS_ALREADY_EXISTS` | `EEXIST` | 文件已存在 |
| `GRPC_STATUS_PERMISSION_DENIED` | `EACCES` | 权限拒绝 |
| `GRPC_STATUS_RESOURCE_EXHAUSTED` | `ENOSPC` | 磁盘空间不足 |
| `GRPC_STATUS_INVALID_ARGUMENT` | `EINVAL` | 参数无效 |
| `GRPC_STATUS_UNIMPLEMENTED` | `ENOSYS` | 功能未实现 |
| `GRPC_STATUS_INTERNAL` | `EIO` | I/O 错误 |
| `GRPC_STATUS_UNAVAILABLE` | `EAGAIN` | 服务不可用 |

## 6. 实现要点和最佳实践

### 6.1 连接管理
```c
typedef struct {
    grpc_channel *channel;
    seaweed_filer_stub *stub;
    char *filer_address;
    grpc_completion_queue *cq;
    pthread_mutex_t lock;
} seaweed_filer_client_t;

// 连接池管理
seaweed_filer_client_t *client_pool[MAX_CLIENTS];
```

### 6.2 文件读写流程

#### 读取流程
```c
int seaweed_read(struct fsal_obj_handle *obj_hdl, uint64_t offset, 
                 size_t buffer_size, void *buffer, size_t *read_amount) {
    // 1. 获取文件的 FileChunk 列表
    LookupDirectoryEntryRequest req = {.directory = parent, .name = filename};
    LookupDirectoryEntryResponse resp;
    
    // 2. 根据 offset 找到对应的 chunks
    for (FileChunk *chunk : resp.entry.chunks) {
        if (chunk->offset <= offset && offset < chunk->offset + chunk->size) {
            // 3. 从对应的 Volume Server 读取数据
            volume_url = lookup_volume(chunk->fid.volume_id);
            http_get(volume_url + "/" + chunk->file_id, buffer, size);
        }
    }
}
```

#### 写入流程
```c
int seaweed_write(struct fsal_obj_handle *obj_hdl, uint64_t offset,
                  size_t buffer_size, void *buffer, size_t *write_amount) {
    // 1. 分配存储卷
    AssignVolumeRequest req = {.count = 1, .collection = collection};
    AssignVolumeResponse resp;
    assign_volume(&req, &resp);
    
    // 2. 写入数据到 Volume Server
    http_post(resp.location.url + "/" + resp.file_id, buffer, buffer_size);
    
    // 3. 更新文件的 chunks 信息
    FileChunk new_chunk = {
        .fid = parse_file_id(resp.file_id),
        .offset = offset,
        .size = buffer_size,
        .modified_ts_ns = current_time_ns()
    };
    
    // 4. 更新 Entry
    UpdateEntryRequest update_req;
    update_req.entry.chunks.append(new_chunk);
    update_entry(&update_req);
}
```

### 6.3 缓存策略
```c
typedef struct {
    char path[PATH_MAX];
    Entry entry;
    time_t cache_time;
    time_t expire_time;
} entry_cache_t;

// 元数据缓存
static entry_cache_t *metadata_cache[CACHE_SIZE];

// 目录列表缓存  
typedef struct {
    char directory[PATH_MAX];
    Entry *entries;
    int count;
    time_t cache_time;
} dir_cache_t;
```

### 6.4 分布式锁实现
```c
int seaweed_lock(struct fsal_obj_handle *obj_hdl, struct flock *lock_desc) {
    char lock_name[PATH_MAX];
    get_full_path(obj_hdl, lock_name);
    
    LockRequest req = {
        .name = lock_name,
        .seconds_to_lock = DEFAULT_LOCK_TIMEOUT,
        .owner = get_client_id()
    };
    
    LockResponse resp;
    if (distributed_lock(&req, &resp) == GRPC_STATUS_OK) {
        // 保存 renew_token 用于解锁
        save_lock_token(lock_name, resp.renew_token);
        return 0;
    }
    return -1;
}
```

### 6.5 性能优化

#### 批量操作
```c
// 批量查找多个文件
int batch_lookup(char **paths, int count, Entry **entries) {
    // 使用多个并发的 gRPC 请求
    grpc_call *calls[count];
    for (int i = 0; i < count; i++) {
        calls[i] = start_lookup_call(paths[i]);
    }
    
    // 等待所有请求完成
    for (int i = 0; i < count; i++) {
        wait_for_completion(calls[i], &entries[i]);
    }
}
```

#### 预取策略
```c
// 目录预取
void prefetch_directory(const char *directory) {
    ListEntriesRequest req = {
        .directory = directory,
        .limit = PREFETCH_LIMIT
    };
    
    // 异步预取目录内容
    async_list_entries(&req, prefetch_callback);
}
```

### 6.6 错误处理和重试
```c
int call_with_retry(grpc_call_func func, void *req, void *resp, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        grpc_status status = func(req, resp);
        
        if (status == GRPC_STATUS_OK) {
            return 0;
        }
        
        if (status == GRPC_STATUS_UNAVAILABLE || 
            status == GRPC_STATUS_DEADLINE_EXCEEDED) {
            // 指数退避重试
            usleep(1000 * (1 << i));
            continue;
        }
        
        // 其他错误不重试
        return map_grpc_error(status);
    }
    return -EAGAIN;
}
```

## 7. 配置和部署

### 7.1 Filer 配置示例
```toml
[filer.options]
recursive_delete = true
max_file_name_length = 255

[leveldb2]
enabled = true
dir = "./filerldb2"
```

### 7.2 FSAL 配置示例
```conf
FSAL {
    Name = SEAWEEDFS;
    filer_endpoints = "localhost:18888";
    connection_pool_size = 10;
    metadata_cache_size = 10000;
    metadata_cache_timeout = 60;
    enable_distributed_lock = true;
    lock_timeout = 300;
}
```

## 8. 测试和调试

### 8.1 单元测试
```c
void test_lookup() {
    // 测试文件查找
    Entry entry;
    int ret = seaweed_lookup("/test/file.txt", &entry);
    assert(ret == 0);
    assert(strcmp(entry.name, "file.txt") == 0);
}

void test_lock() {
    // 测试分布式锁
    struct flock lock = {.l_type = F_WRLCK};
    int ret = seaweed_lock(obj_hdl, &lock);
    assert(ret == 0);
    
    // 解锁
    lock.l_type = F_UNLCK;
    ret = seaweed_lock(obj_hdl, &lock);
    assert(ret == 0);
}
```

### 8.2 性能测试
```bash
# 使用 iozone 测试
iozone -a -g 4G /nfs/mount/point

# 使用 dbench 测试
dbench -D /nfs/mount/point 10
```

## 9. 故障排查

### 9.1 常见问题
1. **连接超时**: 检查 Filer 服务状态和网络连接
2. **权限错误**: 确认 UID/GID 映射正确
3. **锁冲突**: 检查分布式锁状态和超时设置
4. **缓存一致性**: 清除元数据缓存或调整缓存时间

### 9.2 调试工具
```bash
# 查看 Filer 状态
curl http://localhost:8888/stats/counter

# 检查分布式锁
grpcurl -plaintext localhost:18888 filer_pb.SeaweedFiler/FindLockOwner

# 监听元数据变化
grpcurl -plaintext -d '{"path_prefix":"/test"}' localhost:18888 filer_pb.SeaweedFiler/SubscribeMetadata
```

这个完整的 API 梳理和实现指南为基于 SeaweedFS Filer 实现 NFS-Ganesha FSAL 提供了详细的技术参考。SeaweedFS 的设计已经很好地支持了分布式文件系统的需求，包括完整的 POSIX 语义、高性能 I/O、分布式锁等核心功能。