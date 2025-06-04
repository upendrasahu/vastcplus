# Vast OST Plugin - Implementation Architecture

## Overview

The Vast Data OST plugin provides NetBackup OpenStorage API integration with Vast Data clusters. This document describes the architecture and implementation details.

## 1. API Abstraction Layer
The OST API abstracts physical storage into three hierarchical levels:

### 1.1 Storage Server Abstraction
The storage server represents the entire Vast cluster from NetBackup's perspective. It's the top-level entity that manages connections and capabilities.


```c
// Storage Server - Represents the Vast cluster
typedef struct {
    char name[STS_MAX_STSNAME];           // "vast://cluster-endpoint"
    char interfaces[STS_MAX_IF];          // IP addresses/hostnames
    sts_cred_t credentials;               // Authentication info
    sts_srv_cap_t capabilities;           // Supported features
    void* vendor_private;                 // Vast-specific data
} storage_server_t;
```
**Key Concepts:**
- **Name Format**: Always "vast://" prefix + cluster endpoint
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

#### Key Storage Server APIs
```c
// Server lifecycle functions that NetBackup calls
int stspi_open_server(session, server_name, credentials, interface, &handle);
int stspi_get_server_prop(handle, &server_info);
int stspi_get_server_prop_byname(session, server_name, &server_info);
int stspi_close_server(handle);

// Actual implementation in VastStorageServer class
class VastStorageServer {
    // Connection management
    VastRestClient* m_rest_client;         // VMS REST API client
    VastS3Client* m_s3_client;             // S3 data operations client

    // Authentication
    std::string m_auth_token;              // JWT token from VMS
    std::string m_refresh_token;           // Token refresh
    time_t m_token_expiry;                 // Token expiration tracking

    // Vast-specific configuration
    std::string m_server_name;             // Server identifier
    std::string m_vms_endpoint;            // VMS REST API endpoint
    std::string m_s3_endpoint;             // S3 data endpoint

    // LSU management
    std::map<std::string, VastLSU*> m_lsu_cache;  // LSU cache
};
```

### 1.2 LSU (Logical Storage Unit) Abstraction
LSUs are NetBackup's view of storage containers. In Vast, these map to S3 buckets.
```c
// LSU - Maps to Vast bucket or directory
typedef struct {
    char name[STS_MAX_LSUNAME];           // "netbackup-pool-01"
    sts_uint64_t capacity;                // Total space
    sts_uint64_t used;                    // Used space
    sts_uint32_t flags;                   // Capabilities
    void* vendor_private;                 // Vast-specific
} lsu_t;

// Implementation mapping
OST Concept          Vast Reality (S3)         Vast Reality (NFS)
─────────────────────────────────────────────────────────────────
LSU              →   S3 Bucket            →   Directory
LSU Name         →   Bucket name (1:1)    →   Directory name
LSU Properties   →   Bucket metadata      →   Directory stats
LSU List         →   List buckets         →   List directories
```

#### Key LSU APIs
NetBackup discovers available LSUs by calling these functions in sequence:
```c
// LSU discovery and management
int stspi_open_lsu_list_v11(server_handle, filter, &list_handle);
int stspi_open_lsu_list_v9(server_handle, filter, &list_handle);
int stspi_list_lsu(list_handle, &lsu_name);
int stspi_close_lsu_list(list_handle);
int stspi_get_lsu_prop_byname_v11(server_handle, lsu_name, &info);
int stspi_get_lsu_prop_byname_v9(server_handle, lsu_name, &info);
int stspi_label_lsu(server_handle, lsu_name, label);

// Actual implementation in VastLSU class
class VastLSU {
    // Basic properties
    std::string m_name;                    // LSU name
    std::string m_bucket_name;             // Corresponding S3 bucket
    std::string m_path;                    // Virtual path
    std::string m_status;                  // Online/offline status

    // Capacity information
    sts_uint64_t m_total_capacity;         // Total capacity
    sts_uint64_t m_used_capacity;          // Used capacity
    sts_uint64_t m_available_capacity;     // Available capacity
    sts_uint64_t m_image_count;            // Number of images

    // Server reference
    VastStorageServer* m_server;           // Parent server

    // Image management
    std::vector<VastImage*> m_image_cache; // Image cache
};
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
OST Concept          Vast Reality (S3)                           Vast Reality (NFS)
─────────────────────────────────────────────────────────────────────────────────────
Image            →   S3 Object with structured key         →   File(s)
Image Name       →   netbackup/{name}/{date}/data         →   Filename
Image Metadata   →   netbackup/{name}/{date}/metadata     →   Extended attributes
Image Write      →   Multipart upload                     →   File write
Image Read       →   Object GET with range                →   File read
Image Copy       →   Server-side copy                     →   cp/reflink
```

#### Key Image APIs
```c
// Image operations
int stspi_create_image_v9(server_handle, lsu_name, image_def, &image_handle);
int stspi_open_image_v9(server_handle, lsu_name, image_def, mode, &handle);
int stspi_delete_image_v9(server_handle, lsu_name, image_def, async_flag);
int stspi_write_image(image_handle, buffer, offset, length, &bytes_written);
int stspi_read_image(image_handle, buffer, offset, length, &bytes_read);
int stspi_close_image(image_handle, flags);
int stspi_flush_image(image_handle, stat);
int stspi_get_image_info(image_handle, image_info);
int stspi_copy_image_v9(server_handle, src_lsu, src_image, dst_lsu, dst_image);
int stspi_async_copy_image_v11(server_handle, src_lsu, src_image, dst_lsu, dst_image, &event_id);

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
Since OST doesn't support automatic discovery, administrators must explicitly configure storage servers. The plugin then validates and connects to them.

### 2.1 Discovery Mechanisms

```c
// Storage server naming convention:
// Format: "vast://endpoint"
// Examples:
//   "vast://prod-cluster-01.company.com"     - Named cluster
//   "vast://10.0.1.100"                      - IP-based
//   "vast://vast.company.com"                - DNS name

// This is the FIRST function NetBackup calls when it sees a storage server
int stsp_claim(const char* server_name) {
    if (strncmp(server_name, "vast://", 7) == 0) {
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
    char* cluster = server_name + 7;  // Skip "vast://"

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
// Control operations use VMS REST API
typedef struct control_path {
    // HTTP client for VMS REST API
    http_client_t* rest_client;

    // Endpoints
    char base_url[256];         // https://vast-cluster:443

    // Authentication
    char api_token[512];        // JWT token from /api/token/
    time_t token_expiry;

    // S3 credentials from /api/s3keys/
    char s3_access_key[256];
    char s3_secret_key[512];

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

// S3 key creation for bucket access
int ctrl_create_s3_keys(control_path_t* ctrl) {
    // POST /api/s3keys/ - Create S3 access/secret keys
    json_t* req = json_object();
    json_object_set(req, "name", "netbackup-s3-access");

    http_response_t* resp = http_post("/api/s3keys/", req, ctrl->auth_token);
    if (resp->status == 200) {
        extract_s3_credentials(resp->body, ctrl->s3_access_key, ctrl->s3_secret_key);
    }
}

// Bucket operations using AWS S3 SDK with Vast credentials
int ctrl_list_buckets(control_path_t* ctrl, lsu_list_t* list) {
    // Use AWS S3 SDK with credentials from /api/s3keys/
    aws_s3_client_t* s3_client = aws_s3_create_client(
        ctrl->s3_access_key,
        ctrl->s3_secret_key,
        vast_s3_endpoint
    );
    aws_s3_list_buckets(s3_client, list);
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
nbdevconfig -creatests -storage_server vast://prod-cluster-01.company.com \
  -stype VastData \
  -media_server media1.company.com,media2.company.com

# Parameters:
# -storage_server: Name that your plugin will claim (must start with "vast://")
# -stype: Must match your plugin prefix
# -media_server: Comma-separated list of media servers with plugin installed
```

**Plugin Implementation:**
```c
// NetBackup calls this to see if plugin handles this storage
int stsp_claim(const char* server_name) {
    if (strncmp(server_name, "vast://", 7) == 0) {
        return STS_EOK;  // Yes, we handle this
    }
    return STS_ECLAIM;   // Not ours
}
```

#### Set Storage Credentials
```bash
# Configure authentication for Vast access
tpconfig -add -storage_server vast://prod-cluster-01.company.com \
  -stype VastData \
  -sts_user_id apiuser \
  -password 'api-token-here'

# For certificate-based auth:
tpconfig -add -storage_server vast://prod-cluster-01.company.com \
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
  -storage_server vast://prod-cluster-01.company.com \
  -stype VastData \
  -dvlist netbackup-pool-01,netbackup-pool-02

# Parameters:
# -dp: Disk pool name (used in policies)
# -dvlist: Comma-separated LSU names (must match S3 bucket names)
```

**Plugin Must Provide LSUs:**
```c
// NetBackup discovers LSUs by calling these functions
int stsp_open_lsu_list(server_handle, filter, &list_handle) {
    // Return handle for LSU enumeration
}

int stsp_list_lsu(list_handle, &lsu_name) {
    // Return "netbackup-pool-01", "netbackup-pool-02", etc.
    // These must exist as S3 buckets on Vast storage
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

STORAGE_SERVER="vast://vast-prod-cluster.company.com"
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
  -dvlist netbackup-prod-01,netbackup-prod-02

# Test/Dev backups
nbdevconfig -createdp -dp VastPool_Test \
  -storage_server $STORAGE_SERVER \
  -stype VastData \
  -dvlist netbackup-test-01

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
nbdevconfig -teststs -storage_server vast://prod-cluster-01.company.com \
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
tpconfig -dsh -storage_server vast://prod-cluster-01.company.com

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

## 8. Complete OST Interface to Vast Data API Mapping

This section provides a comprehensive mapping of every OST interface function to the corresponding Vast Data API calls.

### 8.1 Plugin Lifecycle Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_init()` | None (local initialization) | Initialize plugin globals, logging, connection pools |
| `stspi_claim()` | None (string matching) | Check if server name starts with "vast://" |
| `stspi_terminate()` | None (cleanup) | Clean up resources, close connections |

### 8.2 Server Management Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_open_server()` | **VMS REST API:**<br/>• `GET /api/clusters/` (verify cluster exists)<br/>• `POST /api/token/` (authentication)<br/>• `POST /api/s3keys/` (S3 credentials)<br/>• `GET /api/tenants/` (tenant discovery)<br/>• `GET /api/views/` (view discovery)<br/>**S3 API:**<br/>• Initialize S3 client with credentials | 1. **Verify existing cluster is operational**<br/>2. Authenticate with VMS using username/password<br/>3. Get JWT access/refresh tokens<br/>4. Create S3 access/secret keys for bucket operations<br/>5. Discover available tenants and views<br/>6. Initialize S3 client for data operations<br/>**Note**: Assumes cluster already exists and is deployed |
| `stspi_close_server()` | **VMS REST API:**<br/>• `POST /api/token/logout/` (logout)<br/>• `DELETE /api/s3keys/{access_key}/` (cleanup S3 keys) | 1. Invalidate JWT tokens<br/>2. Clean up S3 credentials<br/>3. Clean up connection pools<br/>4. Release S3 client resources |
| `stspi_get_server_prop()` | **VMS REST API:**<br/>• `GET /api/clusters/` (cluster info)<br/>• `GET /api/vms/` (VMS system info)<br/>• `GET /api/quotas/` (capacity info) | 1. Get cluster name, version, status<br/>2. Get VMS system information<br/>3. Get total/used capacity from quotas<br/>4. Get supported features and capabilities |
| `stspi_get_server_prop_byname()` | Same as `stspi_get_server_prop()` | Same implementation, different calling pattern |

### 8.3 LSU (Logical Storage Unit) Management Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_open_lsu_list_v11()` | **VMS REST API:**<br/>• `GET /api/tenants/` (list tenants)<br/>• `POST /api/tenants/` (create default tenant if none)<br/>• `GET /api/views/` (list views per tenant)<br/>• `POST /api/views/` (create default S3 view if none)<br/>**S3 API:**<br/>• `GET /` (list buckets)<br/>• `PUT /{bucket}` (create default buckets if none) | 1. Discover or create default tenant ("netbackup")<br/>2. Discover or create default S3-enabled view<br/>3. List all S3 buckets in the cluster<br/>4. Create default buckets if none exist<br/>5. Create LSU objects for each bucket<br/>6. Cache LSU list for enumeration |
| `stspi_open_lsu_list_v9()` | Same as v11 | Same implementation, different version |
| `stspi_list_lsu()` | None (enumerate cached list) | Return next LSU name from cached bucket list |
| `stspi_close_lsu_list()` | None (cleanup) | Clean up LSU list cache and handles |
| `stspi_get_lsu_prop_byname_v11()` | **S3 API:**<br/>• `HEAD /{bucket}` (bucket exists)<br/>• `GET /{bucket}?list-type=2` (object count)<br/>**VMS REST API:**<br/>• `GET /api/quotas/` (quota info)<br/>• `GET /api/views/{view_name}/` (view details) | 1. Verify bucket exists<br/>2. Calculate used space from object sizes<br/>3. Get quota/capacity information<br/>4. Get view context and policies<br/>5. Return LSU properties |
| `stspi_get_lsu_prop_byname_v9()` | Same as v11 | Same implementation, different structure |
| `stspi_label_lsu()` | **S3 API:**<br/>• `PUT /{bucket}?tagging` (bucket tags)<br/>**VMS REST API:**<br/>• `PATCH /api/views/{view_name}/` (update view metadata) | 1. Set bucket tags with label information<br/>2. Update view metadata if applicable<br/>3. Update bucket metadata |

### 8.4 Image Management Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_create_image_v10()` | **S3 API:**<br/>• `POST /{bucket}/netbackup/{name}/{date}/data?uploads` (multipart init)<br/>• `PUT /{bucket}/netbackup/{name}/{date}/metadata` (metadata) | 1. Initialize multipart upload for large images<br/>2. Create metadata object with structured key<br/>3. Return image handle for writing |
| `stspi_create_image_v7()` | Same as v10 | Version conversion then call v10 implementation |
| `stspi_open_image_v10()` | **S3 API:**<br/>• `HEAD /{bucket}/netbackup/{name}/{date}/data` (object exists)<br/>• `GET /{bucket}/netbackup/{name}/{date}/metadata` (metadata) | 1. Verify image object exists<br/>2. Load image metadata<br/>3. Return image handle for reading |
| `stspi_open_image_v9()` | Same as v10 | Version conversion then call v10 implementation |
| `stspi_delete_image_v10()` | **S3 API:**<br/>• `DELETE /{bucket}/netbackup/{name}/{date}/data` (delete object)<br/>• `DELETE /{bucket}/netbackup/{name}/{date}/metadata` (delete metadata) | 1. Delete main image object<br/>2. Delete metadata object<br/>3. Clean up any incomplete multipart uploads |
| `stspi_delete_image_v9()` | Same as v10 | Version conversion then call v10 implementation |

### 8.5 Image I/O Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_write_image()` | **S3 API:**<br/>• `PUT /{bucket}/netbackup/{name}/{date}/data?partNumber=N&uploadId=X` (upload part)<br/>• `POST /{bucket}/netbackup/{name}/{date}/data?uploadId=X` (complete multipart) | 1. Upload data as multipart upload parts<br/>2. Track part ETags for completion<br/>3. Complete multipart upload when done |
| `stspi_read_image()` | **S3 API:**<br/>• `GET /{bucket}/netbackup/{name}/{date}/data` (get object)<br/>• `GET /{bucket}/netbackup/{name}/{date}/data?range=bytes=X-Y` (range read) | 1. Read object data with optional range<br/>2. Handle large objects efficiently<br/>3. Return requested data to NetBackup |
| `stspi_flush_image()` | **S3 API:**<br/>• `POST /{bucket}/netbackup/{name}/{date}/data?uploadId=X` (complete multipart)<br/>• `PUT /{bucket}/netbackup/{name}/{date}/metadata` (update metadata) | 1. Complete any pending multipart uploads<br/>2. Update metadata with final size/status<br/>3. Ensure data is committed to storage |
| `stspi_close_image()` | **S3 API:**<br/>• `POST /{bucket}/netbackup/{name}/{date}/data?uploadId=X` (complete multipart)<br/>• `DELETE /{bucket}/netbackup/{name}/{date}/data?uploadId=X` (abort if incomplete) | 1. Complete multipart upload if successful<br/>2. Abort multipart upload if failed<br/>3. Clean up resources and handles |

### 8.6 Image Metadata Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_get_image_prop_byname_v10()` | **S3 API:**<br/>• `HEAD /{bucket}/netbackup/{name}/{date}/data` (object info)<br/>• `GET /{bucket}/netbackup/{name}/{date}/metadata` (metadata) | 1. Get object size, modification time<br/>2. Load detailed metadata from metadata object<br/>3. Return image properties |
| `stspi_get_image_prop_byname_v9()` | Same as v10 | Version conversion then call v10 implementation |
| `stspi_write_image_meta()` | **S3 API:**<br/>• `PUT /{bucket}/netbackup/{name}/{date}/metadata` (write metadata) | 1. Write metadata to dedicated metadata object<br/>2. Include image properties, checksums, etc. |
| `stspi_read_image_meta()` | **S3 API:**<br/>• `GET /{bucket}/netbackup/{name}/{date}/metadata` (read metadata)<br/>• `GET /{bucket}/netbackup/{name}/{date}/metadata?range=bytes=X-Y` (range read) | 1. Read metadata from metadata object<br/>2. Support range reads for large metadata<br/>3. Return metadata to NetBackup |

### 8.7 Image List Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_open_image_list_v10()` | **S3 API:**<br/>• `GET /{bucket}?list-type=2&prefix=netbackup/` (list objects) | 1. List objects in bucket with netbackup/ prefix<br/>2. Filter out metadata objects<br/>3. Create image list for enumeration |
| `stspi_list_image_v10()` | None (enumerate cached list) | Return next image info from cached object list |
| `stspi_close_image_list()` | None (cleanup) | Clean up image list cache and handles |

### 8.8 Advanced Copy Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_copy_image_v9()` | **S3 API:**<br/>• `PUT /{dest-bucket}/{dest-key}` with `x-amz-copy-source` header | 1. Use S3 server-side copy for efficiency<br/>2. Copy metadata object separately<br/>3. Handle cross-bucket copies |
| `stspi_async_copy_image_v11()` | **S3 API:**<br/>• `POST /{dest-bucket}/{dest-key}?uploads` (multipart copy)<br/>• `PUT /{dest-bucket}/{dest-key}?partNumber=N&uploadId=X` with copy-source | 1. Use multipart copy for large images<br/>2. Return operation ID for tracking<br/>3. Handle copy asynchronously |

### 8.9 Operation Status Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_query_operation_v11()` | **S3 API:**<br/>• `GET /{bucket}?uploads&upload-id-marker=X` (check multipart status) | 1. Check status of async operations<br/>2. Return progress information<br/>3. Handle operation completion |
| `stspi_cancel_operation_v11()` | **S3 API:**<br/>• `DELETE /{bucket}/{key}?uploadId=X` (abort multipart) | 1. Cancel ongoing multipart uploads<br/>2. Clean up partial data<br/>3. Return cancellation status |

### 8.10 Health and Configuration Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_health_check_v11()` | **VMS REST API:**<br/>• `GET /api/clusters/` (cluster health)<br/>• `GET /api/vms/` (VMS health)<br/>• `GET /api/tenants/` (tenant health)<br/>**S3 API:**<br/>• `HEAD /` (S3 service health) | 1. Check VMS API connectivity<br/>2. Check cluster status and health<br/>3. Verify tenant accessibility<br/>4. Check S3 service availability<br/>5. Return overall health status |
| `stspi_get_config_v11()` | **VMS REST API:**<br/>• `GET /api/clusters/` (cluster config)<br/>• `GET /api/views/` (view configurations)<br/>• `GET /api/quotas/` (quota settings) | 1. Get cluster configuration<br/>2. Get view-specific settings<br/>3. Get quota configurations<br/>4. Return requested config values |
| `stspi_set_config_v11()` | **VMS REST API:**<br/>• `PATCH /api/views/{view_name}/` (update view config)<br/>• `PUT /api/quotas/` (update quotas) | 1. Update view configurations<br/>2. Update quota settings<br/>3. Handle configuration changes |
| `stspi_get_statistics_v11()` | **VMS REST API:**<br/>• `GET /api/clusters/` (cluster stats)<br/>• `GET /api/vms/` (VMS stats)<br/>• `GET /api/quotas/` (capacity stats)<br/>• `GET /api/tenants/` (tenant stats)<br/>**S3 API:**<br/>• `GET /?list-type=2` (object counts) | 1. Get cluster performance statistics<br/>2. Get VMS system statistics<br/>3. Get capacity utilization from quotas<br/>4. Get tenant-specific statistics<br/>5. Get object count statistics<br/>6. Return comprehensive stats |

### 8.11 Event Management Functions

| OST Interface | Vast Data API Calls | Implementation Details |
|---------------|---------------------|------------------------|
| `stspi_open_evchannel_v11()` | **VMS REST API:**<br/>• `POST /api/events/subscribe/` (event subscription) | 1. Subscribe to cluster events<br/>2. Set up event channel<br/>3. Configure event filters |
| `stspi_get_event_v11()` | **VMS REST API:**<br/>• `GET /api/events/` (poll events) | 1. Poll for new events<br/>2. Return event data to NetBackup<br/>3. Handle event acknowledgment |
| `stspi_close_evchannel_v9()` | **VMS REST API:**<br/>• `DELETE /api/events/subscribe/{id}` (unsubscribe) | 1. Unsubscribe from events<br/>2. Clean up event channel resources |

### 8.12 API Call Flow Examples

#### Complete Backup Operation Flow:
```
NetBackup Operation          OST Interface                 Vast Data API Calls
─────────────────────────────────────────────────────────────────────────────
1. Discover Storage    →     stspi_open_lsu_list_v11()  →  GET / (list buckets)
                       →     stspi_list_lsu()           →  (enumerate cached buckets)

2. Create Backup       →     stspi_create_image_v10()   →  POST /{bucket}/netbackup/{name}/{date}/data?uploads
                                                        →  PUT /{bucket}/netbackup/{name}/{date}/metadata

3. Write Data          →     stspi_write_image()        →  PUT /{bucket}/netbackup/{name}/{date}/data?partNumber=1&uploadId=X
                       →     stspi_write_image()        →  PUT /{bucket}/netbackup/{name}/{date}/data?partNumber=2&uploadId=X
                       →     ... (multiple parts)       →  ... (multiple parts)

4. Complete Backup     →     stspi_close_image()        →  POST /{bucket}/netbackup/{name}/{date}/data?uploadId=X (complete)
                                                        →  PUT /{bucket}/netbackup/{name}/{date}/metadata (final)
```

#### Complete Restore Operation Flow:
```
NetBackup Operation          OST Interface                 Vast Data API Calls
─────────────────────────────────────────────────────────────────────────────
1. Find Images         →     stspi_open_image_list_v10() →  GET /{bucket}?list-type=2&prefix=netbackup/
                       →     stspi_list_image_v10()      →  (enumerate cached objects)

2. Open Image          →     stspi_open_image_v10()      →  HEAD /{bucket}/netbackup/{name}/{date}/data
                                                        →  GET /{bucket}/netbackup/{name}/{date}/metadata

3. Read Data           →     stspi_read_image()          →  GET /{bucket}/netbackup/{name}/{date}/data?range=bytes=0-1048575
                       →     stspi_read_image()          →  GET /{bucket}/netbackup/{name}/{date}/data?range=bytes=1048576-2097151
                       →     ... (multiple ranges)      →  ... (multiple ranges)

4. Complete Restore    →     stspi_close_image()         →  (cleanup only, no API calls)
```

### 8.13 Deployment Prerequisites

#### What Must Exist Before Plugin Deployment:

**Vast Data Cluster (Vast Admin Responsibility):**
- ✅ **Physical cluster deployed** with nodes, networking, storage
- ✅ **VMS (Vast Management System) running** and accessible
- ✅ **S3 service enabled** on the cluster
- ✅ **Network connectivity** from NetBackup media servers to Vast cluster
- ✅ **Administrative credentials** for VMS API access

**NetBackup Environment (NetBackup Admin Responsibility):**
- ✅ **NetBackup master and media servers** deployed
- ✅ **OST plugin installed** on media servers
- ✅ **Network connectivity** to Vast Data cluster
- ✅ **Storage server configuration** in NetBackup

#### What Plugin Creates Automatically:

**Application-Level Resources (Plugin Responsibility):**
- 🔧 **Tenant**: "netbackup" for data isolation
- 🔧 **View**: "netbackup-s3" for S3 operations
- 🔧 **Buckets**: "netbackup-pool-01" for backup storage
- 🔧 **S3 Keys**: Access credentials for bucket operations

#### Deployment Flow:

```
Phase 1: Infrastructure (Vast Admin)
├── Deploy Vast Data cluster hardware
├── Configure VMS and basic cluster settings
├── Enable S3 service
└── Provide VMS endpoint and credentials to NetBackup admin

Phase 2: NetBackup Configuration (NetBackup Admin)
├── Install OST plugin on media servers
├── Configure storage server: vast://cluster.company.com
├── Set VMS credentials in NetBackup
└── Create disk pools using discovered LSUs

Phase 3: Automatic Setup (Plugin)
├── Verify cluster connectivity on first use
├── Create "netbackup" tenant if needed
├── Create "netbackup-s3" view if needed
├── Create default buckets if needed
└── Ready for backup operations
```

### 8.14 Resource Creation Strategy

#### When Resources Are Created:

**Tenant Creation (First-time setup):**
- **Trigger**: `stspi_open_lsu_list_v11()` when no tenants exist
- **API Call**: `POST /api/tenants/`
- **Default**: Create "netbackup" tenant with S3 enabled
- **Frequency**: Once per cluster

**View Creation (First-time setup):**
- **Trigger**: `stspi_open_lsu_list_v11()` when no S3 views exist in tenant
- **API Call**: `POST /api/views/`
- **Default**: Create "netbackup-s3" view with S3 enabled
- **Frequency**: Once per tenant

**Bucket Creation (On-demand):**
- **Trigger**: `stspi_create_image_v10()` when bucket doesn't exist
- **API Call**: `PUT /{bucket}` (via S3 API)
- **Default**: Create bucket named after LSU
- **Frequency**: Once per LSU/bucket

#### Resource Creation Flow:

```
NetBackup First Use          OST Interface                 Vast Data API Calls
─────────────────────────────────────────────────────────────────────────────
1. Initial Discovery   →     stspi_open_lsu_list_v11()  →  GET /api/tenants/
                                                        →  POST /api/tenants/ (if none)
                                                        →  GET /api/views/
                                                        →  POST /api/views/ (if none S3)
                                                        →  GET / (list buckets)
                                                        →  PUT /netbackup-pool-01 (if none)

2. First Backup        →     stspi_create_image_v10()   →  HEAD /netbackup-pool-01
                                                        →  PUT /netbackup-pool-01 (if not exists)
                                                        →  POST /netbackup-pool-01/netbackup/{name}/{date}/data?uploads
```

#### Default Resource Names:

**Tenant**: `netbackup`
- Purpose: Isolate NetBackup data from other applications
- Configuration: S3 enabled, default VIP pool

**View**: `netbackup-s3`
- Purpose: S3-enabled volume for bucket operations
- Configuration: S3 enabled, NFS/SMB disabled, in "netbackup" tenant

**Buckets**: `netbackup-pool-{N}` (e.g., `netbackup-pool-01`)
- Purpose: Storage containers for backup images
- Configuration: Created in "netbackup-s3" view context

### 8.14 Complete Vast Data API Reference

#### VMS REST API (Control Plane) - Complete List:

**Authentication & Session Management:**
- `POST /api/token/` - Initial authentication with username/password
- `POST /api/token/refresh/` - Refresh JWT tokens
- `POST /api/token/logout/` - Logout and invalidate tokens
- `DELETE /api/token/` - Alternative logout endpoint

**Cluster & System Information:**
- `GET /api/clusters/` - Cluster information, status, and capabilities
- `GET /api/vms/` - VMS system information and health
- `GET /api/quotas/` - Quota information and capacity statistics

**Tenant Management:**
- `GET /api/tenants/` - List all tenants
- `GET /api/tenants/{name}/` - Get specific tenant information
- `POST /api/tenants/` - Create new tenant
- `DELETE /api/tenants/{name}/` - Delete tenant

**View Management (NFS/SMB/S3 Volumes):**
- `GET /api/views/` - List all views across tenants
- `GET /api/views/{name}/` - Get specific view information
- `POST /api/views/` - Create new view
- `PATCH /api/views/{name}/` - Update view configuration
- `DELETE /api/views/{name}/` - Delete view

**S3 Key Management:**
- `GET /api/s3keys/` - List S3 access keys
- `GET /api/s3keys/{name}/` - Get specific S3 key information
- `POST /api/s3keys/` - Create new S3 access/secret key pair
- `DELETE /api/s3keys/{access_key}/` - Delete S3 key pair

**Event Management (Optional):**
- `POST /api/events/subscribe/` - Subscribe to cluster events
- `GET /api/events/` - Poll for new events
- `DELETE /api/events/subscribe/{id}/` - Unsubscribe from events

#### S3 API (Data Plane) - Complete List:

**Bucket Operations:**
- `GET /` - List all buckets
- `HEAD /{bucket}` - Check bucket existence and get metadata
- `PUT /{bucket}` - Create bucket (via AWS S3 SDK)
- `DELETE /{bucket}` - Delete bucket (via AWS S3 SDK)
- `GET /{bucket}?location` - Get bucket location
- `PUT /{bucket}?tagging` - Set bucket tags
- `GET /{bucket}?tagging` - Get bucket tags

**Object Operations:**
- `GET /{bucket}?list-type=2` - List objects in bucket (v2 API)
- `GET /{bucket}?list-type=2&prefix={prefix}` - List objects with prefix
- `HEAD /{bucket}/{key}` - Get object metadata
- `GET /{bucket}/{key}` - Get object data
- `GET /{bucket}/{key}?range=bytes=X-Y` - Get object data range
- `PUT /{bucket}/{key}` - Put object (simple upload)
- `DELETE /{bucket}/{key}` - Delete object
- `PUT /{bucket}/{key}` with `x-amz-copy-source` - Server-side copy

**Multipart Upload Operations:**
- `POST /{bucket}/{key}?uploads` - Initialize multipart upload
- `PUT /{bucket}/{key}?partNumber=N&uploadId=X` - Upload part
- `POST /{bucket}/{key}?uploadId=X` - Complete multipart upload
- `DELETE /{bucket}/{key}?uploadId=X` - Abort multipart upload
- `GET /{bucket}?uploads` - List active multipart uploads
- `GET /{bucket}?uploads&upload-id-marker=X` - Check multipart status

**Object Metadata Operations:**
- `PUT /{bucket}/{key}?metadata` - Set object metadata
- `GET /{bucket}/{key}?metadata` - Get object metadata
- `HEAD /{bucket}/{key}` - Get object headers and metadata

#### API Usage Patterns by OST Operation:

**Server Connection (One-time setup):**
- VMS: `POST /api/token/` → `POST /api/s3keys/` → `GET /api/tenants/` → `GET /api/views/`
- S3: Initialize client with credentials

**LSU Discovery (Per backup policy setup):**
- VMS: `GET /api/tenants/` → `GET /api/views/` → `GET /api/quotas/`
- S3: `GET /` → `HEAD /{bucket}` (per bucket)

**Backup Operation (Per backup job):**
- S3: `POST /{bucket}/netbackup/{name}/{date}/data?uploads` → Multiple `PUT /{bucket}/netbackup/{name}/{date}/data?partNumber=N&uploadId=X` → `POST /{bucket}/netbackup/{name}/{date}/data?uploadId=X` → `PUT /{bucket}/netbackup/{name}/{date}/metadata`

**Restore Operation (Per restore job):**
- S3: `GET /{bucket}?list-type=2&prefix=netbackup/` → `HEAD /{bucket}/netbackup/{name}/{date}/data` → `GET /{bucket}/netbackup/{name}/{date}/metadata` → Multiple `GET /{bucket}/netbackup/{name}/{date}/data?range=bytes=X-Y`

**Health Monitoring (Periodic):**
- VMS: `GET /api/clusters/` → `GET /api/vms/` → `GET /api/tenants/`
- S3: `HEAD /`

#### Data Flow Architecture:

```
NetBackup OST Interface Layer
           ↓
    Plugin Translation Layer
           ↓
┌─────────────────┬─────────────────┐
│   VMS REST API  │    S3 API       │
│  (Control)      │   (Data)        │
├─────────────────┼─────────────────┤
│ • Authentication│ • Bucket Ops    │
│ • Tenant/View   │ • Object I/O    │
│ • LSU Discovery │ • Multipart     │
│ • Health Check  │ • Metadata      │
│ • Statistics    │ • Copy Ops      │
└─────────────────┴─────────────────┘
           ↓
    Vast Data Cluster
```

#### Critical Integration Points:

1. **Authentication Flow**: OST → VMS REST API → JWT tokens → S3 credentials
2. **LSU Management**: OST LSUs ↔ S3 Buckets (1:1 mapping)
3. **Image Management**: OST Images ↔ S3 Objects + Metadata Objects
4. **Data Transfer**: OST I/O ↔ S3 Multipart Upload/Download
5. **Error Handling**: Vast API errors → OST error codes → NetBackup

## Summary

This Vast Data OST plugin implementation provides:

### **Implemented Components**

1. **Complete OST API Implementation**: All required NetBackup OST functions implemented
   - Plugin lifecycle: `stspi_init()`, `stspi_claim()`, `stspi_terminate()`
   - Server management: `stspi_open_server()`, `stspi_close_server()`, `stspi_get_server_prop()`
   - LSU operations: `stspi_open_lsu_list()`, `stspi_list_lsu()`, `stspi_get_lsu_prop_byname()`
   - Image operations: `stspi_create_image()`, `stspi_read_image()`, `stspi_write_image()`, `stspi_delete_image()`
   - Advanced features: `stspi_copy_image()`, `stspi_async_copy_image()`, `stspi_flush_image()`

2. **Complete Backend Integration**: Full Vast Data API integration
   - **VastRestClient**: Complete VMS REST API client with libcurl and jsoncpp
   - **VastS3Client**: Complete S3 API client with AWS Signature V4 authentication
   - **VastStorageServer**: Connection management and authentication
   - **VastLSU**: LSU management mapping to Vast buckets
   - **VastImage**: Image I/O operations with S3 backend

3. **Production-Ready Features**:
   - JWT token authentication and refresh
   - S3 multipart uploads for large images
   - Error handling and retry logic
   - Connection pooling and management
   - Comprehensive logging and debugging

### **Next Steps**

1. **Testing**: Comprehensive testing with actual Vast Data environments
2. **Performance Tuning**: Optimization for production workloads
3. **Documentation**: Deployment and configuration guides
4. **Monitoring**: Enhanced observability for production use

The architecture ensures high performance while maintaining reliability and full OST compliance.