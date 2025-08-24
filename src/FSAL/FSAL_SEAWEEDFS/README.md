# FSAL_SEAWEEDFS - SeaweedFS FSAL for NFS-Ganesha

## Overview

FSAL_SEAWEEDFS is a File System Abstraction Layer (FSAL) module for NFS-Ganesha that provides NFS access to SeaweedFS distributed storage. This implementation allows standard NFS clients to access files stored in a SeaweedFS cluster.

## Features (MVP Version)

- **NFSv3/NFSv4 Support**: Basic file and directory operations
- **Direct Filer Integration**: Uses SeaweedFS Filer gRPC API for metadata operations
- **File-level Locking**: Simple file locks using SeaweedFS distributed locking
- **Path-based Handles**: Stable filehandles based on file paths
- **Connection Pooling**: Efficient connection management to SeaweedFS Filers
- **Metadata Caching**: LRU cache for improved performance

## Requirements

### Build Dependencies

- NFS-Ganesha development headers
- OpenSSL development libraries (for MD5 hashing)
- CMake 3.5 or later
- GCC/Clang compiler with C11 support

### Runtime Dependencies  

- SeaweedFS cluster with Filer service
- Network connectivity to SeaweedFS Filers
- Standard NFS client utilities

## Installation

### Building from Source

1. **Enable SEAWEEDFS FSAL** in your NFS-Ganesha build:
   ```bash
   cd /path/to/nfs-ganesha
   mkdir build && cd build
   cmake -DUSE_FSAL_SEAWEEDFS=ON ../src
   make fsalseaweedfs
   sudo make install
   ```

2. **Install the FSAL library**:
   ```bash
   sudo cp src/FSAL/FSAL_SEAWEEDFS/libfsalseaweedfs.so.4 /usr/lib64/ganesha/
   ```

### Configuration

1. **Create ganesha configuration file** (see `seaweedfs.conf` example):
   ```bash
   sudo mkdir -p /etc/ganesha
   sudo cp src/config_samples/seaweedfs.conf /etc/ganesha/ganesha.conf
   ```

2. **Edit the configuration** to match your environment:
   - Update `filer_endpoints` to point to your SeaweedFS Filers
   - Adjust `Path` and `Pseudo` for your export requirements
   - Configure client access permissions

3. **Start NFS-Ganesha**:
   ```bash
   sudo systemctl start nfs-ganesha
   # or
   sudo ganesha.nfsd -f /etc/ganesha/ganesha.conf
   ```

## Configuration Parameters

### FSAL Block Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `filer_endpoints` | string | "localhost:18888" | Comma-separated list of SeaweedFS Filer endpoints |
| `connection_timeout` | int | 30 | Connection timeout in seconds |
| `connection_pool_size` | int | 10 | Size of connection pool |
| `max_concurrent_requests` | int | 100 | Maximum concurrent requests |
| `path_cache_size` | int | 10000 | Size of path mapping cache |
| `path_cache_ttl` | int | 300 | Cache TTL in seconds |
| `enable_file_locks` | bool | true | Enable file locking support |
| `default_lock_timeout` | int | 300 | Default lock timeout in seconds |

### Example Configuration

```conf
FSAL
{
    Name = SEAWEEDFS;
    filer_endpoints = "filer1:18888,filer2:18888,filer3:18888";
    connection_pool_size = 20;
    path_cache_size = 50000;
    path_cache_ttl = 600;
}

EXPORT
{
    Export_Id = 1;
    Path = "/";
    Pseudo = "/seaweedfs";
    Access_Type = RW;
    
    FSAL {
        Name = SEAWEEDFS;
    }
    
    CLIENT {
        Clients = "192.168.1.0/24";
        Access_Type = RW;
    }
}
```

## Client Usage

### Mounting NFS Export

**NFSv3**:
```bash
sudo mount -t nfs -o vers=3 server:/seaweedfs /mnt/seaweedfs
```

**NFSv4**:
```bash
sudo mount -t nfs -o vers=4 server:/seaweedfs /mnt/seaweedfs
```

### Basic Operations

```bash
# Create files and directories
echo "Hello SeaweedFS" > /mnt/seaweedfs/test.txt
mkdir /mnt/seaweedfs/testdir

# List contents
ls -la /mnt/seaweedfs/

# File operations
cp /mnt/seaweedfs/test.txt /mnt/seaweedfs/test-copy.txt
mv /mnt/seaweedfs/test-copy.txt /mnt/seaweedfs/testdir/

# Remove files
rm /mnt/seaweedfs/testdir/test-copy.txt
rmdir /mnt/seaweedfs/testdir
```

### File Locking

```bash
# Exclusive lock example
flock /mnt/seaweedfs/locktest.txt -c "sleep 10; echo 'locked operation'"

# Shared lock for reading
flock -s /mnt/seaweedfs/data.txt cat /mnt/seaweedfs/data.txt
```

## Architecture

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   NFS Clients   │    │  NFS-Ganesha    │    │  SeaweedFS      │
│                 │    │                 │    │                 │
│ ┌─────────────┐ │    │ ┌─────────────┐ │    │ ┌─────────────┐ │
│ │ Linux/VMware│ │◄──►│ │FSAL_SeaweedFS│ │◄──►│ │Filer Service│ │
│ │    etc.     │ │    │ │             │ │    │ │             │ │
│ └─────────────┘ │    │ └─────────────┘ │    │ └─────────────┘ │
└─────────────────┘    └─────────────────┘    │ ┌─────────────┐ │
                                              │ │Volume Servers│ │
                                              │ └─────────────┘ │
                                              └─────────────────┘
```

### Key Components

- **Path Mapping**: MD5-based filehandle generation from file paths
- **Connection Pool**: Round-robin load balancing across Filer endpoints  
- **Metadata Cache**: LRU cache for frequently accessed path mappings
- **Lock Manager**: Simple file-level locking via SeaweedFS distributed locks

## Limitations (MVP Version)

- **File-level locks only**: No byte-range locking support
- **Basic error handling**: Limited retry and recovery mechanisms
- **No advanced caching**: Simple metadata-only caching
- **Single-threaded operations**: No parallel I/O optimization
- **Limited security**: Basic authentication only

## Troubleshooting

### Common Issues

1. **"FSAL SEAWEEDFS not found"**:
   - Verify FSAL library is installed in correct location
   - Check NFS-Ganesha was built with `USE_FSAL_SEAWEEDFS=ON`

2. **Connection failures**:
   - Verify SeaweedFS Filer is running and accessible
   - Check network connectivity and firewall settings
   - Validate `filer_endpoints` configuration

3. **Permission denied**:
   - Check NFS export CLIENT configuration
   - Verify SeaweedFS file permissions
   - Ensure proper UID/GID mapping

4. **Stale file handles**:
   - Restart NFS-Ganesha to refresh handle cache
   - Check SeaweedFS cluster health
   - Verify path mappings are consistent

### Debug Logging

Enable debug logging in `/etc/ganesha/ganesha.conf`:

```conf
LOG
{
    Default_Log_Level = DEBUG;
    
    Components
    {
        FSAL = FULL_DEBUG;
        NFS_V4 = DEBUG;
        CACHE = DEBUG;
    }
    
    Facility
    {
        name = FILE;
        destination = "/var/log/ganesha/ganesha.log";
        enable = active;
    }
}
```

### Performance Monitoring

Monitor key metrics:
- Connection pool utilization
- Cache hit/miss ratios  
- Lock contention
- I/O latency

Log files contain performance statistics updated periodically.

## Development

### Testing

Run the test suite:

```bash
cd build
make test_seaweedfs
./src/FSAL/FSAL_SEAWEEDFS/test_seaweedfs
```

### Contributing

1. Follow NFS-Ganesha coding standards
2. Add appropriate logging statements
3. Update configuration examples
4. Add test cases for new functionality
5. Update documentation

## Support

- **Issues**: Report bugs on the project issue tracker
- **Documentation**: See full design documentation in `docs/design/`
- **Community**: Join NFS-Ganesha and SeaweedFS communities

## License

This software is licensed under LGPL-3.0-or-later, consistent with NFS-Ganesha licensing.