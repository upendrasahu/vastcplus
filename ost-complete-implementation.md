# Vast OST Driver - Complete Implementation Guide

## 1. API Abstraction Layer

### 1.1 Storage Server Abstraction

The OST API abstracts physical storage into three hierarchical levels:

```c
// Storage Server - Represents the Vast cluster
typedef struct {
    char name[STS_MAX_STSNAME];           // "VastData:cluster-01"
    char interfaces[STS_MAX_IF];          // IP addresses/hostnames
    sts_cred_t credentials;               // Authentication info
    sts_srv_cap_t capabilities;           // Supported features
    void* vendor_private;                 // Vast-specific data
} storage_server_t;
```
**Key Concepts:**
- **Name Format**: Always "VastData:" prefix + cluster identifier
- **Interfaces**: Multiple IPs for redundancy (e.g., "10.0.1.1,10.0.1.2")
- **Capabilities**: Bit flags indicating what operations are supported
- **Vendor Private**: Store Vast-specific data like cluster UUID, version
```c
// Implementation mapping
OST Concept          Vast Reality
───────────────────────────────────
Storage Server   →   Vast Cluster
Server Handle    →   Connection Pool to Vast
Server Props     →   Cluster capabilities & status
```

#### Key Storage Server APIs:
```c
// Server lifecycle functions that NetBackup calls
int stsp_open_server(server_name, credentials, &handle);
// Get server properties - NetBackup uses this to understand capabilities
int stsp_get_server_prop(handle, &server_info);
// Close server - cleanup all resources
int stsp_close_server(handle);

// Internal implementation
typedef struct vast_server_handle {
    // Connection management
    connection_pool_t* mgmt_connections;   // REST API connections
    connection_pool_t* data_connections;   // S3/NFS connections
    
    // State tracking
    auth_token_t* auth_token;
    time_t token_expiry;
    
    // Performance counters
    atomic_uint64_t bytes_written;
    atomic_uint64_t bytes_read;
    
    // Vast-specific
    char cluster_id[64];
    char s3_endpoint[256];
    char nfs_mount[PATH_MAX];
} vast_server_handle_t;
```

### 1.2 LSU (Logical Storage Unit) Abstraction
LSUs are NetBackup's view of storage containers. In Vast, these map to either S3 buckets or NFS directories, depending on the data path.
```c
// LSU - Maps to Vast bucket or directory
typedef struct {
    char name[STS_MAX_LSUNAME];           // "backup-pool-01"
    sts_uint64_t capacity;                // Total space
    sts_uint64_t used;                    // Used space
    sts_uint32_t flags;                   // Capabilities
    void* vendor_private;                 // Vast-specific
} lsu_t;

// Implementation mapping
OST Concept          Vast Reality (S3)         Vast Reality (NFS)
─────────────────────────────────────────────────────────────────
LSU              →   S3 Bucket            →   Directory
LSU Name         →   Bucket name          →   Directory name
LSU Properties   →   Bucket metadata      →   Directory stats
LSU List         →   List buckets         →   List directories
```

#### Key LSU APIs:
NetBackup discovers available LSUs by calling these functions in sequence:
```c
// LSU discovery and management
// Open LSU list - NetBackup calls this to start LSU enumeration
int stsp_open_lsu_list(server_handle, filter, &list_handle);
// List LSUs - NetBackup calls this repeatedly to get each LSU
int stsp_list_lsu(list_handle, &lsu_name);
// Get LSU properties - NetBackup needs to know capacity, features, etc.
int stsp_get_lsu_prop_byname(lsu_name, &lsu_info);

// Internal state tracking
typedef struct vast_lsu_tracker {
    char lsu_name[256];
    enum { LSU_S3_BUCKET, LSU_NFS_DIR } type;
    union {
        struct {
            char bucket_name[256];
            char region[64];
        } s3;
        struct {
            char path[PATH_MAX];
            int dir_fd;
        } nfs;
    } location;
    
    // Cached properties
    time_t last_stat_time;
    sts_lsu_info_t cached_info;
} vast_lsu_tracker_t;
```

### 1.3 Image Abstraction
Images represent individual backups. They're the actual data that gets written to and read from storage.
```c
// Image - Individual backup
typedef struct {
    char basename[STS_MAX_IMAGENAME];     // "client1-daily-20240115"
    sts_date_t creation_date;             // When created
    sts_uint64_t size;                    // Total size
    sts_uint32_t flags;                   // Full/Incremental
    void* vendor_private;                 // Vast-specific
} image_t;

// Implementation mapping
OST Concept          Vast Reality (S3)              Vast Reality (NFS)
───────────────────────────────────────────────────────────────────────
Image            →   Object collection        →   File(s)
Image Name       →   Object prefix           →   Filename
Image Write      →   Multipart upload        →   File write
Image Read       →   Object GET              →   File read
Image Copy       →   Server-side copy        →   cp/reflink
```

#### Key Image APIs:
```c
// Image operations
// Create new image - NetBackup calls this to start a backup
int stsp_create_image(lsu, image_def, &image_handle);
// Write data to image - Called repeatedly during backup
int stsp_write_image(image_handle, buffer, offset, length);
// Read image to buffer
int stsp_read_image(image_handle, buffer, offset, length);
// Close image - Finalize the backup
int stsp_close_image(image_handle, flags);

// Internal image tracking
typedef struct vast_image_handle {
    char image_id[256];
    enum { IMG_S3_OBJECT, IMG_NFS_FILE } type;
    
    // S3 specific
    struct {
        char bucket[256];
        char key_prefix[512];
        char upload_id[128];        // Multipart upload ID
        part_tracker_t* parts;      // Part ETags
    } s3;
    
    // NFS specific
    struct {
        int fd;                     // File descriptor
        char path[PATH_MAX];
        off_t current_offset;
    } nfs;
    
    // Common
    sts_uint64_t bytes_written;
    sts_uint32_t state;            // Creating/Complete/etc
} vast_image_handle_t;
```

## 2. Storage Server Discovery

### 2.1 Discovery Mechanisms

Since OST doesn't support automatic discovery, administrators must explicitly configure storage servers. The plugin then validates and connects to them.

```c
// Storage server naming convention:
// Format: "VastData:identifier"
// Examples:
//   "VastData:prod-cluster-01"     - Named cluster
//   "VastData:10.0.1.100"          - IP-based
//   "VastData:vast.company.com"     - DNS name

// This is the FIRST function NetBackup calls when it sees a storage server
int stsp_claim(const char* server_name) {
    if (strncmp(server_name, "VastData:", 9) == 0) {
        return STS_EOK;  // We handle this
    }
    return STS_ECLAIM;   // Not ours
}
```

### 2.2 Connection Discovery
After claiming a server, we need to figure out how to connect to it:
```c
// Discover available interfaces/endpoints
typedef struct {
    char mgmt_endpoint[256];     // REST API endpoint
    char data_endpoint[256];     // S3/NFS endpoint
    int mgmt_port;              // Usually 443
    int data_port;              // S3: 9000/9443, NFS: 2049
} vast_endpoints_t;

int discover_vast_endpoints(const char* server_name, vast_endpoints_t* endpoints) {
    // Parse server name
    char* cluster = server_name + 9;  // Skip "VastData:"
    
    // Try to resolve and connect
    // 1. DNS lookup
    // 2. Try known ports
    // 3. Query management API for data endpoints
    
    return probe_endpoints(cluster, endpoints);
}
```

### 2.3 Capability Discovery

```c
// Query server capabilities during connection
int discover_server_capabilities(vast_server_handle_t* handle, 
                                sts_server_info_t* info) {
    // Set base capabilities
    info->srv_flags = STS_SRV_DISK | STS_SRV_CRED;
    
    // Query Vast-specific features
    if (vast_supports_s3(handle)) {
        STS_SET_SRV_CAP(info->srv_cap, STS_SRVC_WRITE_IMAGE_RANDOM);
        STS_SET_SRV_CAP(info->srv_cap, STS_SRVC_COPY_IMAGE);
    }
    
    if (vast_supports_dedup(handle)) {
        info->srv_flags |= STS_SRV_DEDUP;
    }
    
    return STS_EOK;
}
```

## 3. Control Path Implementation

### 3.1 Control Path Architecture

```c
// Control operations use REST API
typedef struct control_path {
    // HTTP client for REST
    http_client_t* rest_client;
    
    // Endpoints
    char base_url[256];         // https://vast-cluster:443
    
    // Authentication
    char api_token[512];
    time_t token_expiry;
    
    // Operation queues
    request_queue_t* pending_requests;
    pthread_t worker_threads[4];
} control_path_t;
```

### 3.2 Control Operations

```c
// Server management
int ctrl_connect_server(const char* server, sts_cred_t* cred) {
    // POST /api/v1/auth
    json_t* req = json_object();
    json_object_set(req, "username", cred->username);
    json_object_set(req, "password", cred->password);
    
    http_response_t* resp = http_post("/api/v1/auth", req);
    if (resp->status == 200) {
        extract_auth_token(resp->body);
    }
}

// LSU operations
int ctrl_list_buckets(control_path_t* ctrl, lsu_list_t* list) {
    // GET /api/v1/buckets
    http_response_t* resp = http_get("/api/v1/buckets", 
                                    ctrl->auth_token);
    parse_bucket_list(resp->body, list);
}

// Image metadata
int ctrl_create_image_metadata(const char* image_name, 
                              sts_image_def_t* def) {
    // PUT /api/v1/images/{image_name}/metadata
    json_t* metadata = create_metadata_json(def);
    http_put("/api/v1/images/{name}/metadata", metadata);
}
```

### 3.3 Control Path Flow

```
NetBackup              OST Plugin              Vast REST API
    |                      |                         |
    |--stsp_open_server--->|                         |
    |                      |---POST /auth---------->|
    |                      |<--Token----------------|
    |                      |                         |
    |--stsp_list_lsu------>|                         |
    |                      |---GET /buckets-------->|
    |                      |<--Bucket list----------|
    |                      |                         |
    |--stsp_create_image-->|                         |
    |                      |---PUT /metadata------->|
    |                      |<--Success-------------|
```

## 4. Data Path Implementation

### 4.1 Data Path Architecture

```c
// High-performance data transfer
typedef struct data_path {
    // Transfer method
    enum { DATA_S3, DATA_NFS } method;
    
    // S3 data path
    struct {
        s3_client_t* client;
        multipart_manager_t* mp_manager;
        thread_pool_t* upload_pool;
    } s3;
    
    // NFS data path
    struct {
        char mount_point[PATH_MAX];
        int mount_fd;
        buffer_pool_t* write_buffers;
    } nfs;
    
    // Performance
    rate_limiter_t* throttle;
    stats_collector_t* stats;
} data_path_t;
```

### 4.2 S3 Data Operations

```c
// Parallel multipart upload
typedef struct {
    char upload_id[128];
    atomic_int next_part;
    part_info_t parts[10000];
    pthread_mutex_t parts_lock;
} multipart_upload_t;

int data_write_s3(vast_image_handle_t* img, void* buffer, 
                  size_t size, off_t offset) {
    // Determine part number
    int part_num = (offset / PART_SIZE) + 1;
    
    // Queue for upload
    upload_task_t* task = create_upload_task(
        img->s3.bucket,
        img->s3.key_prefix,
        img->s3.upload_id,
        part_num,
        buffer,
        size
    );
    
    thread_pool_submit(data_path->s3.upload_pool, task);
}

// Upload worker
void* upload_worker(void* arg) {
    upload_task_t* task = (upload_task_t*)arg;
    
    // Upload part
    char* etag = s3_upload_part(
        task->bucket,
        task->key,
        task->upload_id,
        task->part_num,
        task->buffer,
        task->size
    );
    
    // Record ETag
    record_part_etag(task->upload_id, task->part_num, etag);
}
```

### 4.3 NFS Data Operations

```c
// Direct I/O for performance
int data_write_nfs(vast_image_handle_t* img, void* buffer,
                   size_t size, off_t offset) {
    // Use O_DIRECT for large writes
    if (size >= 1024 * 1024) {
        return pwrite(img->nfs.fd, buffer, size, offset);
    }
    
    // Buffer small writes
    buffer_write(&img->nfs.write_buffer, buffer, size, offset);
}
```

### 4.4 Data Path Flow

```
Backup Data Flow:
NetBackup              OST Plugin              Vast Data Path
    |                      |                         |
    |--stsp_write_image--->|                         |
    |    (64MB chunk)      |---Multipart Upload---->| (S3)
    |                      |      OR                 |
    |                      |---pwrite()------------>| (NFS)
    |                      |<--Success--------------|
    
Restore Data Flow:
    |--stsp_read_image---->|                         |
    |                      |---GET Object---------->| (S3)
    |                      |      OR                 |
    |                      |---pread()------------->| (NFS)
    |<--Data---------------|<--Data----------------|
```

## 5. RPC & Connection Management

### 5.1 Connection Pool Implementation

```c
typedef struct connection {
    int fd;                          // Socket/file descriptor
    enum { CONN_REST, CONN_S3, CONN_NFS } type;
    time_t last_used;
    int in_use;
    void* protocol_specific;         // HTTP client, S3 state, etc
} connection_t;

typedef struct connection_pool {
    connection_t* connections;
    int max_connections;
    int active_connections;
    pthread_mutex_t pool_lock;
    pthread_cond_t available;
    
    // Health checking
    pthread_t health_checker;
    int check_interval;
} connection_pool_t;

// Get connection from pool
connection_t* get_connection(connection_pool_t* pool, int timeout) {
    pthread_mutex_lock(&pool->pool_lock);
    
    while (1) {
        // Find available connection
        for (int i = 0; i < pool->max_connections; i++) {
            if (!pool->connections[i].in_use) {
                pool->connections[i].in_use = 1;
                pthread_mutex_unlock(&pool->pool_lock);
                return &pool->connections[i];
            }
        }
        
        // Wait for available connection
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout;
        
        if (pthread_cond_timedwait(&pool->available, 
                                   &pool->pool_lock, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->pool_lock);
            return NULL;
        }
    }
}
```

### 5.2 RPC Implementation

```c
// Generic RPC framework
typedef struct rpc_request {
    char method[32];              // "CreateImage", "WriteData", etc
    void* params;                 // Method-specific parameters
    int timeout;
    callback_fn callback;         // Async completion
} rpc_request_t;

typedef struct rpc_client {
    // Transport
    connection_pool_t* conn_pool;
    
    // Serialization
    int (*serialize)(rpc_request_t*, void** buffer, size_t* size);
    int (*deserialize)(void* buffer, size_t size, void** result);
    
    // Async handling
    request_tracker_t* pending;
    pthread_t response_handler;
} rpc_client_t;

// Make RPC call
int rpc_call(rpc_client_t* client, rpc_request_t* request, 
             void** response) {
    // Get connection
    connection_t* conn = get_connection(client->conn_pool, 5);
    if (!conn) return STS_ECONNECT;
    
    // Serialize request
    void* buffer;
    size_t size;
    client->serialize(request, &buffer, &size);
    
    // Send request
    send_data(conn, buffer, size);
    
    // Wait for response (sync) or return (async)
    if (request->callback) {
        track_async_request(client->pending, request);
        return STS_EOK;
    } else {
        return wait_for_response(conn, response);
    }
}
```

### 5.3 Connection Health Management

```c
// Health checking thread
void* health_checker(void* arg) {
    connection_pool_t* pool = (connection_pool_t*)arg;
    
    while (1) {
        sleep(pool->check_interval);
        
        for (int i = 0; i < pool->max_connections; i++) {
            if (!pool->connections[i].in_use) {
                if (!check_connection_health(&pool->connections[i])) {
                    reconnect(&pool->connections[i]);
                }
            }
        }
    }
}

// Reconnection with backoff
int reconnect(connection_t* conn) {
    int retry = 0;
    int delay = 1;
    
    while (retry < MAX_RETRIES) {
        if (establish_connection(conn) == SUCCESS) {
            return SUCCESS;
        }
        
        sleep(delay);
        delay = MIN(delay * 2, 60);  // Max 60 seconds
        retry++;
    }
    
    return FAILURE;
}
```

### 5.4 Protocol-Specific RPC

```c
// REST API RPC
int rest_rpc(const char* method, const char* path, 
             json_t* params, json_t** result) {
    http_request_t req = {
        .method = method,
        .path = path,
        .headers = {
            {"Authorization", auth_token},
            {"Content-Type", "application/json"}
        },
        .body = json_dumps(params)
    };
    
    http_response_t* resp = http_execute(&req);
    *result = json_loads(resp->body);
    return resp->status;
}

// S3 operations use AWS SDK or custom implementation
int s3_rpc(s3_operation_t op, s3_params_t* params, 
           s3_result_t** result) {
    switch (op) {
        case S3_PUT_OBJECT:
            return s3_put_object(params->bucket, params->key, 
                               params->data, params->size);
        case S3_GET_OBJECT:
            return s3_get_object(params->bucket, params->key,
                               &result->data, &result->size);
    }
}
```

## 6. Complete Integration Example

```c
// Full backup operation flow
int perform_backup(const char* client, const char* data, size_t size) {
    // 1. Control Path: Create image
    vast_image_handle_t* img;
    int rc = stsp_create_image(lsu, &image_def, &img);
    
    // 2. Data Path: Write data
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = MIN(size - offset, CHUNK_SIZE);
        rc = stsp_write_image(img, data + offset, chunk, offset);
        offset += chunk;
    }
    
    // 3. Control Path: Finalize
    rc = stsp_close_image(img, STS_CLOSEF_COMPLETE);
    
    // 4. Update catalog
    update_netbackup_catalog(client, img->image_id);
    
    return rc;
}
```

## 7. NetBackup Configuration and Integration

### 7.1 Configuration Overview

After installing the OST plugin on media servers, NetBackup administrators must configure the storage using specific CLI commands. This configuration bridges NetBackup with your OST plugin.

**Configuration Workflow:**
```
1. Install Plugin → 2. Add Storage Server → 3. Set Credentials → 4. Create Disk Pool → 5. Create Storage Unit
```

### 7.2 Required Configuration Commands

These commands are run on the **NetBackup Master Server** by administrators:

#### Add Storage Server
```bash
# Tell NetBackup about the Vast storage
nbdevconfig -creatests -storage_server VastData:prod-cluster-01 \
  -stype VastData \
  -media_server media1.company.com,media2.company.com

# Parameters:
# -storage_server: Name that your plugin will claim (must start with "VastData:")
# -stype: Must match your plugin prefix
# -media_server: Comma-separated list of media servers with plugin installed
```

**Plugin Implementation:**
```c
// NetBackup calls this to see if plugin handles this storage
int stsp_claim(const char* server_name) {
    if (strncmp(server_name, "VastData:", 9) == 0) {
        return STS_EOK;  // Yes, we handle this
    }
    return STS_ECLAIM;   // Not ours
}
```

#### Set Storage Credentials
```bash
# Configure authentication for Vast access
tpconfig -add -storage_server VastData:prod-cluster-01 \
  -stype VastData \
  -sts_user_id apiuser \
  -password 'api-token-here'

# For certificate-based auth:
tpconfig -add -storage_server VastData:prod-cluster-01 \
  -stype VastData \
  -sts_user_id apiuser \
  -password 'cert:/path/to/cert.pem'
```

**Plugin Receives Credentials:**
```c
int stsp_open_server(session_def, server_name, cred, interface, &handle) {
    // Parse credentials
    char* user = cred->cr_cert;
    char* pass = user + strlen(user) + 1;
    
    // Use for Vast authentication
    vast_auth_t auth = {
        .username = user,
        .password = pass,
        .type = cred->cr_type  // STS_CRED_CLEAR or STS_CRED_MD5
    };
    
    return vast_connect(server_name, &auth, handle);
}
```

#### Create Disk Pool
```bash
# Create NetBackup disk pool using discovered LSUs
nbdevconfig -createdp -dp Vast_Pool_01 \
  -storage_server VastData:prod-cluster-01 \
  -stype VastData \
  -dvlist backup-lsu-01,backup-lsu-02

# Parameters:
# -dp: Disk pool name (used in policies)
# -dvlist: Comma-separated LSU names from your storage
```

**Plugin Must Provide LSUs:**
```c
// NetBackup discovers LSUs by calling these functions
int stsp_open_lsu_list(server_handle, filter, &list_handle) {
    // Return handle for LSU enumeration
}

int stsp_list_lsu(list_handle, &lsu_name) {
    // Return "backup-lsu-01", "backup-lsu-02", etc.
    // These must exist on Vast storage
}
```

#### Create Storage Unit
```bash
# Create storage unit for backup policies
bpstuadd -label Vast_STU_01 -dp Vast_Pool_01

# Additional options:
bpstuadd -label Vast_STU_01 -dp Vast_Pool_01 \
  -maxmpx 10 \           # Max concurrent jobs
  -maxsize 0 \           # No size limit
  -odo                   # On-demand only
```

### 7.3 Complete Setup Script

```bash
#!/bin/bash
# Complete Vast OST Storage Setup
# Run on NetBackup Master Server

STORAGE_SERVER="VastData:vast-prod-cluster"
MEDIA_SERVERS="media1.example.com,media2.example.com"
API_USER="netbackup_api"
API_PASSWORD="SecureToken123"

echo "=== Vast OST Storage Configuration ==="

# 1. Add storage server
echo "Adding storage server..."
nbdevconfig -creatests -storage_server $STORAGE_SERVER \
  -stype VastData \
  -media_server $MEDIA_SERVERS

# 2. Configure credentials
echo "Setting credentials..."
tpconfig -add -storage_server $STORAGE_SERVER \
  -stype VastData \
  -sts_user_id $API_USER \
  -password "$API_PASSWORD"

# 3. Discover available LSUs
echo "Discovering LSUs..."
sleep 5  # Allow plugin to initialize
nbdevconfig -liststs -storage_server $STORAGE_SERVER -stype VastData

# 4. Create disk pools for different purposes
echo "Creating disk pools..."

# Production backups
nbdevconfig -createdp -dp VastPool_Production \
  -storage_server $STORAGE_SERVER \
  -stype VastData \
  -dvlist prod-lsu-01,prod-lsu-02

# Test/Dev backups  
nbdevconfig -createdp -dp VastPool_Test \
  -storage_server $STORAGE_SERVER \
  -stype VastData \
  -dvlist test-lsu-01

# 5. Create storage units
echo "Creating storage units..."
bpstuadd -label VastSTU_Prod -dp VastPool_Production
bpstuadd -label VastSTU_Test -dp VastPool_Test

# 6. Verify configuration
echo "=== Configuration Summary ==="
nbdevquery -listdp -U
nbdevquery -liststs -U

echo "Configuration complete!"
```

### 7.4 Verification Commands

```bash
# List configured storage servers
nbdevconfig -liststs -stype VastData

# Show disk pool details
nbdevquery -listdp -dp Vast_Pool_01

# Test storage server connection
nbdevconfig -teststs -storage_server VastData:prod-cluster-01 \
  -stype VastData

# Check media server access
bptestnetconn -a -s media1.example.com
```

### 7.5 Troubleshooting Configuration

```bash
# Check if plugin is loaded
/usr/openv/lib/ost-plugins/libstspiVastDataMT.so -version

# View plugin logs
tail -f /usr/openv/logs/nbemm/logs/nbemm.log
tail -f /usr/openv/logs/vast-ost.log

# Test credentials
tpconfig -dsh -storage_server VastData:prod-cluster-01

# Force LSU refresh
nbdevconfig -updatedp -dp Vast_Pool_01 -stype VastData
```

### 7.6 Integration with Backup Policies

Once configured, the storage can be used in backup policies:

```bash
# Create policy using Vast storage
bppolicynew PolicyName -M master.example.com \
  -st Standard \
  -stu VastSTU_Prod \
  -res Vast_Pool_01

# Add clients and schedules as normal
```

### 7.7 What Happens During Configuration

1. **Storage Server Addition**: NetBackup records the storage server and notifies media servers
2. **Plugin Loading**: Media servers load the plugin when needed
3. **Claim Process**: NetBackup calls `stsp_claim()` to verify plugin ownership
4. **Connection**: NetBackup calls `stsp_open_server()` with provided credentials
5. **Discovery**: NetBackup calls `stsp_list_lsu()` to discover available storage
6. **Disk Pool Creation**: NetBackup validates LSUs exist before creating pools

Without these configuration steps, your plugin will never be called by NetBackup!

## Summary

This implementation provides:

1. **Complete API Abstraction**: Maps OST concepts to Vast storage
2. **Server Discovery**: Explicit configuration with capability detection
3. **Dual Path Architecture**: Separated control (REST) and data (S3/NFS) paths
4. **Robust Connection Management**: Connection pooling, health checks, and reconnection

The architecture ensures high performance while maintaining reliability and OST compliance.