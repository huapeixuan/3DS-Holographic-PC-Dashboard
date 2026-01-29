/**
 * 3DS System Monitor - 完整版 v1.7
 * 修复网络初始化问题
 */

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// ========================================
// Configuration
// ========================================
#define UDP_PORT 9001

// 3D Shader
extern u8 vshader_shbin[];
extern u32 vshader_shbin_size;

// 3D Geometry
typedef struct {
    float x, y, z;
    float r, g, b, a;
} vertex;

#define VBO_SIZE 2000
static vertex* g_vbo_buffers[2] = {NULL, NULL};
static int g_cur_buf_idx = 0;
static int g_vertex_count = 0; // Dynamic vertex count

static DVLB_s* g_vshader_dvlb = NULL;
static shaderProgram_s g_shader;
static int g_uLoc_projection;
static int g_uLoc_modelView;
static C3D_AttrInfo g_attrInfo;
static C3D_BufInfo g_bufInfo;


// 颜色定义
#define COL_BG        C2D_Color32(0x10, 0x10, 0x20, 0xFF)
#define COL_PANEL     C2D_Color32(0x20, 0x20, 0x40, 0xFF)
#define COL_CYAN      C2D_Color32(0x00, 0xF5, 0xFF, 0xFF)
#define COL_PURPLE    C2D_Color32(0x9D, 0x4E, 0xDD, 0xFF)
#define COL_GREEN     C2D_Color32(0x00, 0xFF, 0x88, 0xFF)
#define COL_ORANGE    C2D_Color32(0xFF, 0x6B, 0x35, 0xFF)
#define COL_TEXT      C2D_Color32(0xE0, 0xE0, 0xFF, 0xFF)
#define COL_WHITE     C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)

// ========================================
// Global State
// ========================================
typedef struct {
    float cpu_usage;    
    float memory_usage; 
    float swap_usage;     // 从 server 获取
    float cpu_temp;
    float gpu_temp;       // 从 server 获取
    float power_watts;
    int cpu_freq_mhz;     // 新增: CPU 频率
    int fan_rpm;
    bool connected;
    int uptime_seconds;
    int current_mode;
    
    // 新增字段
    char hostname[64];
    char os_name[64];
    char cpu_model[64];
    int cpu_cores;
    int battery_level;
    char battery_status[32];
    
    // 新增: 内存绝对值 (MB)
    int memory_total_mb;
    int memory_used_mb;
} AppState;

static void parse_json_string(const char* json, const char* key, char* dest, size_t dest_size) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":\"", key);
    char* p = strstr(json, search_key);
    if (p) {
        p += strlen(search_key);
        size_t i = 0;
        while (*p && *p != '"' && i < dest_size - 1) {
            dest[i++] = *p++;
        }
        dest[i] = '\0';
    }
}

static AppState g_state = {
    .cpu_usage = 25.0f,
    .memory_usage = 45.0f,
    .swap_usage = 10.0f,
    .cpu_temp = 42.0f,
    .gpu_temp = 48.0f,
    .power_watts = 15.0f,
    .gpu_temp = 48.0f,
    .power_watts = 15.0f,
    .cpu_freq_mhz = 2400,
    .hostname = "CONNECTING...",
    .os_name = "UNKNOWN",
    .cpu_model = "GENERIC CPU",
    .cpu_cores = 8,
    .battery_level = -1,
    .battery_status = "UNKNOWN",
    .memory_total_mb = 16384,
    .memory_used_mb = 8192,
    .fan_rpm = 1200,
    .connected = false,
    .uptime_seconds = 0,
    .current_mode = 3
};

// 网络状态
static int g_socket = -1;
static bool g_net_init = false;
static bool g_server_found = false;
static struct sockaddr_in g_server_addr;
static struct sockaddr_in g_broadcast_addr;
static u32* g_soc_buffer = NULL;

// 3D Props
// 3D Props - unused arrays removed




// 渲染目标

static C3D_RenderTarget* bottomScreen = NULL;
static C2D_TextBuf textBuf = NULL;

// 功率历史
#define POWER_HISTORY_SIZE 50
static float g_power_history[POWER_HISTORY_SIZE] = {0};
static int g_power_idx = 0;

// 动画
static float g_fan_angle = 0;
static int g_frame = 0;
static C2D_SpriteSheet g_spriteSheet;
static float g_cat_anim_frame = 0.0f;
static Result g_romfs_rc = -1;

// ========================================
// 3D Helper Functions
// ========================================
static void init_3d(void) {
    // Load Shader
    g_vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&g_shader);
    shaderProgramSetVsh(&g_shader, &g_vshader_dvlb->DVLE[0]);
    
    g_uLoc_projection = shaderInstanceGetUniformLocation(g_shader.vertexShader, "projection");
    g_uLoc_modelView = shaderInstanceGetUniformLocation(g_shader.vertexShader, "modelView");
    
    C3D_BindProgram(&g_shader);
    
    // Attribute Info
    AttrInfo_Init(&g_attrInfo);
    AttrInfo_AddLoader(&g_attrInfo, 0, GPU_FLOAT, 3); // v0 = position
    AttrInfo_AddLoader(&g_attrInfo, 1, GPU_FLOAT, 4); // v1 = color
    
    // Allocate VBO (Double Buffer)
    g_vbo_buffers[0] = (vertex*)linearAlloc(VBO_SIZE * sizeof(vertex));
    g_vbo_buffers[1] = (vertex*)linearAlloc(VBO_SIZE * sizeof(vertex));
    
    // Buf Info
    BufInfo_Init(&g_bufInfo);
    // Initial bind (will be updated per frame)
    BufInfo_Add(&g_bufInfo, g_vbo_buffers[0], sizeof(vertex), 2, 0x10);


}

static void fill_cube(vertex* v, float x, float y, float z, float w, float h, float d, u32 color) {
    float r = ((color >> 0) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = ((color >> 16) & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    
    // Front
    vertex front[] = {
        {x, y, z+d, r, g, b, a}, {x+w, y, z+d, r, g, b, a}, {x+w, y+h, z+d, r, g, b, a},
        {x, y, z+d, r, g, b, a}, {x+w, y+h, z+d, r, g, b, a}, {x, y+h, z+d, r, g, b, a}
    };
    // Back (dimmer)
    float shade = 0.6f;
    vertex back[] = {
        {x+w, y, z, r*shade, g*shade, b*shade, a}, {x, y, z, r*shade, g*shade, b*shade, a}, {x, y+h, z, r*shade, g*shade, b*shade, a},
        {x+w, y, z, r*shade, g*shade, b*shade, a}, {x, y+h, z, r*shade, g*shade, b*shade, a}, {x+w, y+h, z, r*shade, g*shade, b*shade, a}
    };
    // Left
    vertex left[] = {
        {x, y, z, r*shade, g*shade, b*shade, a}, {x, y, z+d, r*shade, g*shade, b*shade, a}, {x, y+h, z+d, r*shade, g*shade, b*shade, a},
        {x, y, z, r*shade, g*shade, b*shade, a}, {x, y+h, z+d, r*shade, g*shade, b*shade, a}, {x, y+h, z, r*shade, g*shade, b*shade, a}
    };
    // Right
    vertex right[] = {
        {x+w, y, z+d, r*shade, g*shade, b*shade, a}, {x+w, y, z, r*shade, g*shade, b*shade, a}, {x+w, y+h, z, r*shade, g*shade, b*shade, a},
        {x+w, y, z+d, r*shade, g*shade, b*shade, a}, {x+w, y+h, z, r*shade, g*shade, b*shade, a}, {x+w, y+h, z+d, r*shade, g*shade, b*shade, a}
    };
    // Top
    vertex top[] = {
        {x, y+h, z+d, r, g, b, a}, {x+w, y+h, z+d, r, g, b, a}, {x+w, y+h, z, r, g, b, a},
        {x, y+h, z+d, r, g, b, a}, {x+w, y+h, z, r, g, b, a}, {x, y+h, z, r, g, b, a}
    };
    // Bottom
    vertex bottom[] = {
        {x, y, z, r*shade, g*shade, b*shade, a}, {x+w, y, z, r*shade, g*shade, b*shade, a}, {x+w, y, z+d, r*shade, g*shade, b*shade, a},
        {x, y, z, r*shade, g*shade, b*shade, a}, {x+w, y, z+d, r*shade, g*shade, b*shade, a}, {x, y, z+d, r*shade, g*shade, b*shade, a}
    };
    
    memcpy(v, front, sizeof(front)); v += 6;
    memcpy(v, back, sizeof(back)); v += 6;
    memcpy(v, left, sizeof(left)); v += 6;
    memcpy(v, right, sizeof(right)); v += 6;
    memcpy(v, top, sizeof(top)); v += 6;
    memcpy(v, bottom, sizeof(bottom));
}

// 简单的风扇叶片三角形
// Helper to rotate point around Y axis
static void rotate_point_y(float* x, float* z, float angle) {
    float rx = *x * cosf(angle) + *z * sinf(angle);
    float rz = -*x * sinf(angle) + *z * cosf(angle);
    *x = rx;
    *z = rz;
}

// Helper to pitch blade around its local X axis
static void rotate_point_x(float* y, float* z, float angle) {
    float ry = *y * cosf(angle) - *z * sinf(angle);
    float rz = *y * sinf(angle) + *z * cosf(angle);
    *y = ry;
    *z = rz;
}

// Dynamic Shading Helper
// Returns a brightness multiplier (0.3 to 1.0) based on normal dot product with light
static float calculate_shading(float nx, float ny, float nz) {
    // Light source coming from top-left-front: [0.3, 0.5, 0.8] normalized approx
    // Let's use specific simple light direction: Top-Right-Front
    // L = [0.4, 0.6, 0.7] (normalized: ~0.4, 0.6, 0.7)
    
    float lx = 0.4f;
    float ly = 0.6f;
    float lz = 0.7f;
    
    // Normalize normals just in case
    float len = sqrtf(nx*nx + ny*ny + nz*nz);
    if(len > 0.001f) { nx/=len; ny/=len; nz/=len; }
    
    float dot = nx*lx + ny*ly + nz*lz;
    
    // Clamp dot to [0, 1] for diffuse
    if(dot < 0) dot = 0;
    
    // Ambient + Diffuse
    float ambient = 0.4f;
    float diffuse = 0.6f;
    return ambient + dot * diffuse;
}

// 3D 风车叶片 (有厚度和倾角 - 渐变帆状)
// Now returns vertices with normals encoded? Or just pre-calculated shading?
// Since we apply global tilt later, pre-calculated shading on local geometry is WRONG if we want it dynamic.
// BUT, calculating normals for every vertex every frame in C on 3DS CPU is fine for ~300 verts.
// We will store normals in a temporary structure or just recalculate them in the update loop.
// Let's make `fill_windmill_blade` produce the LOCAL geometry, and `update_3d_geometry` handles rotation and shading.

typedef struct {
    float x, y, z;
    float nx, ny, nz; 
} geom_vtx;

// Helper to compute normal for a triangle (p1, p2, p3)
static void compute_normal(float x1, float y1, float z1,
                           float x2, float y2, float z2,
                           float x3, float y3, float z3,
                           float* nx, float* ny, float* nz) {
    float ax = x2 - x1, ay = y2 - y1, az = z2 - z1;
    float bx = x3 - x1, by = y3 - y1, bz = z3 - z1;
    *nx = ay*bz - az*by;
    *ny = az*bx - ax*bz;
    *nz = ax*by - ay*bx;
    float l = sqrtf(*nx * *nx + *ny * *ny + *nz * *nz);
    if(l > 0) { *nx/=l; *ny/=l; *nz/=l; }
}

static void fill_windmill_blade_geom(geom_vtx* v_out, int* v_count) {
    // Parameters for the blade
    int segments = 8;
    float length = 1.0f;
    float width_root = 0.15f;
    float width_tip = 0.5f; // Flared tip
    
    // Twist: Root at 45 deg, Tip at 10 deg -> twist by -35 deg
    float twist_start = 60.0f * M_PI / 180.0f;
    float twist_end = 15.0f * M_PI / 180.0f;
    
    // Curve: Bend forward/backward along Z
    float curve_amount = 0.15f;

    // Use a single strip of quads (flat blade) to prevent Z-fighting entirely
    
    float prev_x = 0;
    float prev_y_top = width_root/2;
    float prev_y_bot = -width_root/2;
    float prev_z_top = 0;
    float prev_z_bot = 0;
    
    // Rotate initial points
    rotate_point_x(&prev_y_top, &prev_z_top, twist_start);
    rotate_point_x(&prev_y_bot, &prev_z_bot, twist_start);

    int v_idx = 0;

    for (int i = 1; i <= segments; i++) {
        float t = (float)i / segments;
        float x = t * length;
        
        // Width Calculation
        float w;
        if (t < 0.2f) {
            w = width_root + (width_tip * 0.8f - width_root) * (t / 0.2f);
        } else {
            float t2 = (t - 0.2f) / 0.8f;
            w = width_tip * (0.4f + 0.6f * sinf(t2 * M_PI));
        }
        
        // Base coords
        float curr_y_top = w/2;
        float curr_y_bot = -w/2;
        float curr_z_top = 0;
        float curr_z_bot = 0;
        
        // Interpolate Twist
        float angle = twist_start + (twist_end - twist_start) * t;
        
        // Calculate Curve (Z-offset based on X)
        float z_offset = curve_amount * (t * t); 
        
        // Apply Twist
        rotate_point_x(&curr_y_top, &curr_z_top, angle);
        rotate_point_x(&curr_y_bot, &curr_z_bot, angle);
        
        // Apply Curve
        curr_z_top += z_offset;
        curr_z_bot += z_offset;

        // Build 1 Quad (2 Triangles)
        // Vertices:
        // A: prev_top
        // B: prev_bot
        // C: curr_bot
        // D: curr_top
        
        float ax = prev_x, ay = prev_y_top, az = prev_z_top;
        float bx = prev_x, by = prev_y_bot, bz = prev_z_bot;
        float cx = x,      cy = curr_y_bot, cz = curr_z_bot;
        float dx = x,      dy = curr_y_top, dz = curr_z_top;
        
        // Normal
        float nx, ny, nz;
        compute_normal(ax, ay, az, bx, by, bz, cx, cy, cz, &nx, &ny, &nz);
        
        // Triangle 1: ABC
        v_out[v_idx++] = (geom_vtx){ax, ay, az, nx, ny, nz};
        v_out[v_idx++] = (geom_vtx){bx, by, bz, nx, ny, nz};
        v_out[v_idx++] = (geom_vtx){cx, cy, cz, nx, ny, nz};
        
        // Triangle 2: ACD
        v_out[v_idx++] = (geom_vtx){ax, ay, az, nx, ny, nz};
        v_out[v_idx++] = (geom_vtx){cx, cy, cz, nx, ny, nz};
        v_out[v_idx++] = (geom_vtx){dx, dy, dz, nx, ny, nz};
        
        // Update prev
        prev_x = x;
        prev_y_top = curr_y_top;
        prev_y_bot = curr_y_bot;
        prev_z_top = curr_z_top;
        prev_z_bot = curr_z_bot;
    }
    
    *v_count = v_idx;
}



// ========================================
// Network Functions (容错版本)
// ========================================
static void init_network(void) {
    // 分配socket缓冲区
    g_soc_buffer = (u32*)memalign(0x1000, 0x10000);
    if (!g_soc_buffer) {
        return;
    }
    
    // 初始化socket服务
    Result ret = socInit(g_soc_buffer, 0x10000);
    if (R_FAILED(ret)) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return;
    }
    
    g_net_init = true;
    
    // 创建UDP socket
    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket < 0) {
        return;
    }
    
    // 设置非阻塞
    int flags = fcntl(g_socket, F_GETFL, 0);
    fcntl(g_socket, F_SETFL, flags | O_NONBLOCK);
    
    // 设置广播
    int broadcast = 1;
    setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // 绑定端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(g_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_socket);
        g_socket = -1;
        return;
    }
    
    // 设置广播地址
    u32 ip = gethostid();
    memset(&g_broadcast_addr, 0, sizeof(g_broadcast_addr));
    g_broadcast_addr.sin_family = AF_INET;
    g_broadcast_addr.sin_port = htons(UDP_PORT);
    g_broadcast_addr.sin_addr.s_addr = (ip & 0x00FFFFFF) | 0xFF000000;
}

static void network_update(void) {
    if (g_socket < 0) return;
    
    static int send_counter = 0;
    send_counter++;
    
    // 每60帧发送一次
    if (send_counter >= 60) {
        send_counter = 0;
        if (g_server_found) {
            sendto(g_socket, "PING", 4, 0, 
                   (struct sockaddr*)&g_server_addr, sizeof(g_server_addr));
        } else {
            sendto(g_socket, "DISCOVER", 8, 0, 
                   (struct sockaddr*)&g_broadcast_addr, sizeof(g_broadcast_addr));
        }
    }
    
    // 接收数据
    char buf[4096];
    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);
    
    int n = recvfrom(g_socket, buf, sizeof(buf) - 1, 0, 
                     (struct sockaddr*)&sender, &len);
    
    if (n > 0) {
        buf[n] = '\0';
        
        if (strncmp(buf, "SERVER", 6) == 0 && !g_server_found) {
            g_server_found = true;
            g_server_addr = sender;
            g_state.connected = true;
        } 
        else if (buf[0] == '{') {
            // 解析JSON数据
            char* p;
            if ((p = strstr(buf, "\"cpu_usage\":"))) {
                g_state.cpu_usage = strtof(p + 12, NULL);
            }
            if ((p = strstr(buf, "\"cpu_temp\":"))) {
                g_state.cpu_temp = strtof(p + 11, NULL);
            }
            if ((p = strstr(buf, "\"gpu_temp\":"))) {
                g_state.gpu_temp = strtof(p + 11, NULL);
            }
            if ((p = strstr(buf, "\"memory_usage\":"))) {
                g_state.memory_usage = strtof(p + 15, NULL);
            }
            if ((p = strstr(buf, "\"memory_total\":"))) {
                g_state.memory_total_mb = atoi(p + 15);
            }
            if ((p = strstr(buf, "\"memory_used\":"))) {
                g_state.memory_used_mb = atoi(p + 14);
            }
            if ((p = strstr(buf, "\"swap_usage\":"))) {
                g_state.swap_usage = strtof(p + 13, NULL);
            }
            if ((p = strstr(buf, "\"power_score\":"))) {
                g_state.power_watts = strtof(p + 14, NULL) / 100000.0f;
            }
            if ((p = strstr(buf, "\"fan_speeds\":["))) {
                g_state.fan_rpm = atoi(p + 14);
            }
            if ((p = strstr(buf, "\"cpu_frequency_mhz\":"))) {
                g_state.cpu_freq_mhz = atoi(p + 20);
            }
            
            // 解析新字段
            parse_json_string(buf, "hostname", g_state.hostname, sizeof(g_state.hostname));
            parse_json_string(buf, "os_name", g_state.os_name, sizeof(g_state.os_name));
            parse_json_string(buf, "cpu_model", g_state.cpu_model, sizeof(g_state.cpu_model));
            parse_json_string(buf, "battery_status", g_state.battery_status, sizeof(g_state.battery_status));
            
            if ((p = strstr(buf, "\"cpu_cores\":"))) {
                g_state.cpu_cores = atoi(p + 12);
            }
            if ((p = strstr(buf, "\"battery_percentage\":"))) {
                g_state.battery_level = atoi(p + 21);
            }
            if ((p = strstr(buf, "\"uptime_secs\":"))) {
                g_state.uptime_seconds = atoi(p + 14);
            }
        }
    }
}

static void cleanup_network(void) {
    if (g_socket >= 0) {
        close(g_socket);
        g_socket = -1;
    }
    if (g_net_init) {
        socExit();
        g_net_init = false;
    }
    if (g_soc_buffer) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
    }
}

        
// ========================================
// Render Helper
// ========================================
// ========================================
// Render Helper
// ========================================

static void update_3d_geometry(void) {
    // Write to the next buffer (CPU side)
    int next_buf_idx = (g_cur_buf_idx + 1) % 2;
    vertex* vtx = g_vbo_buffers[next_buf_idx];
    
    // Coordinates (Scaled to fit in -2.0 to 2.0 View Space approx)

    // Screen 400x240. Center (200, 120). Scale 1/50 = 0.02.
    // X: 20 -> (20-200)*0.02 = -3.6. Too wide for standard FOV?
    // Let's use smaller scale or push camera back. z = -4.0.
    // If z=-4, FOV 40 deg. Visible height at z=0 is tan(20)*4*2 = 2.9. Width = 2.9 * 1.66 = 4.8.
    // So X range -2.4 to 2.4.
    // CPU X=20 -> -2.1 approx.
    
    float scale = 0.012f;
    float cx = 200.0f;
    float cy = 120.0f;
    
    // Bevel/Depth
    float d = 20.0f * scale; 
    
    // CPU Bar (X=20, Y=50, W=35)
    // Y is inverted in 3D (up is positive).
    // Screen Y=50 is high up.
    // Bottom of bar is at Y=50+140 = 190.
    float cpuH = 140 * (g_state.cpu_usage / 100.0f);
    if (cpuH > 140) cpuH = 140;
    
    float x = (20 - cx) * scale;
    float y = (cy - (190)) * scale; // Bottom Y
    float w = 35 * scale;
    float h = cpuH * scale;
    
    fill_cube(vtx, x, y, 0, w, h, d, COL_GREEN);
    vtx += 36;
    
    // RAM Bar (X=65)
    float ramH = 140 * (g_state.memory_usage / 100.0f);
    x = (65 - cx) * scale;
    h = ramH * scale;
    fill_cube(vtx, x, y, 0, w, h, d, COL_CYAN);
    vtx += 36;
    
    // SWAP Bar (X=110)
    float swapH = 140 * (g_state.swap_usage / 100.0f);
    x = (110 - cx) * scale;
    h = swapH * scale;
    fill_cube(vtx, x, y, 0, w, h, d, COL_PURPLE);
    vtx += 36;
    
    // Fan (X=332, Y=190 - Aligned Middle)
    
    // Scale down fan size (0.6 -> 0.3)
    float fanScale = scale * 0.3f;
    
    float fanX = (332 - cx) * scale;
    float fanY = (cy - 190) * scale;
    float fanZ = 0.0f;
    
    // Remember start of fan vertices to apply global tilt later
    vertex* fan_start_vtx = vtx;
    
    // Draw Hub (Center - Disc/Cylinder)
    float hubRadius = 16.0f * fanScale;
    float hubDepth = 20.0f * fanScale;
    int hubSides = 16;
    for (int i = 0; i < hubSides; i++) {
        float a1 = (float)i * 2.0f * M_PI / hubSides;
        float a2 = (float)(i+1) * 2.0f * M_PI / hubSides;
        
        // Cap
        vtx->x = fanX + cosf(a1) * hubRadius; vtx->y = fanY + sinf(a1) * hubRadius; vtx->z = fanZ + hubDepth/2; vtx->r=0.4; vtx->g=0.4; vtx->b=0.5; vtx->a=1; vtx++;
        vtx->x = fanX + cosf(a2) * hubRadius; vtx->y = fanY + sinf(a2) * hubRadius; vtx->z = fanZ + hubDepth/2; vtx->r=0.4; vtx->g=0.4; vtx->b=0.5; vtx->a=1; vtx++;
        vtx->x = fanX;                        vtx->y = fanY;                        vtx->z = fanZ + hubDepth/2; vtx->r=0.4; vtx->g=0.4; vtx->b=0.5; vtx->a=1; vtx++;
        
        // Side quads
        float x1 = cosf(a1) * hubRadius, y1 = sinf(a1) * hubRadius;
        float x2 = cosf(a2) * hubRadius, y2 = sinf(a2) * hubRadius;
        
        vtx->x = fanX + x1; vtx->y = fanY + y1; vtx->z = fanZ + hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;
        vtx->x = fanX + x2; vtx->y = fanY + y2; vtx->z = fanZ + hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;
        vtx->x = fanX + x2; vtx->y = fanY + y2; vtx->z = fanZ - hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;

        vtx->x = fanX + x1; vtx->y = fanY + y1; vtx->z = fanZ + hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;
        vtx->x = fanX + x2; vtx->y = fanY + y2; vtx->z = fanZ - hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;
        vtx->x = fanX + x1; vtx->y = fanY + y1; vtx->z = fanZ - hubDepth/2; vtx->r=0.3; vtx->g=0.3; vtx->b=0.4; vtx->a=1; vtx++;
    }

    // Inner small hub detail
    float innerHubR = 6.0f * fanScale;
    for (int i = 0; i < hubSides; i++) {
        float a1 = (float)i * 2.0f * M_PI / hubSides;
        float a2 = (float)(i+1) * 2.0f * M_PI / hubSides;
        vtx->x = fanX + cosf(a1) * innerHubR; vtx->y = fanY + sinf(a1) * innerHubR; vtx->z = fanZ + hubDepth/2 + 0.01f; vtx->r=0.0; vtx->g=0.5; vtx->b=0.8; vtx->a=1; vtx++;
        vtx->x = fanX + cosf(a2) * innerHubR; vtx->y = fanY + sinf(a2) * innerHubR; vtx->z = fanZ + hubDepth/2 + 0.01f; vtx->r=0.0; vtx->g=0.5; vtx->b=0.8; vtx->a=1; vtx++;
        vtx->x = fanX;                        vtx->y = fanY;                        vtx->z = fanZ + hubDepth/2 + 0.01f; vtx->r=0.0; vtx->g=0.5; vtx->b=0.8; vtx->a=1; vtx++;
    }

    // Generate Blade Geometry once (local buffer)
    geom_vtx blade_geom[256]; 
    int blade_v_count = 0;
    fill_windmill_blade_geom(blade_geom, &blade_v_count);
    
    // Draw 3 Blades (Updated from 4)
    for (int i=0; i<3; i++) {
        float angle = g_fan_angle + i * (2.0f * M_PI / 3.0f);
        float ca = cosf(angle);
        float sa = sinf(angle);
        
        float bScale = 50.0f * fanScale; // Smaller blades
        
        for(int k=0; k<blade_v_count; k++) {
            // Local pos
            float lx = blade_geom[k].x * bScale;
            float ly = blade_geom[k].y * bScale;
            float lz = blade_geom[k].z * bScale;
            
            // 1. Rotate Z (Spin)
            float rx = lx * ca - ly * sa;
            float ry = lx * sa + ly * ca;
            float rz = lz; // Z doesn't change with Z-rotation
            
            // 2. Normals Rotation (Spin around Z)
            float nx = blade_geom[k].nx;
            float ny = blade_geom[k].ny;
            float nz = blade_geom[k].nz;
            
            float rnx = nx * ca - ny * sa;
            float rny = nx * sa + ny * ca;
            float rnz = nz;

            // 3. Write Vertex
            vtx->x = fanX + rx;
            vtx->y = fanY + ry;
            vtx->z = fanZ + rz;
            
            // 4. Calculate Lighting/Shading
            // Apply a "Global Tilt" to normal for lighting calculation to simulate
            // the fan being tilted towards the camera/light.
            // Tilt same as global tilt below: X=-25, Y=15
            float tX = -25.0f * M_PI / 180.0f;
            float tY = 15.0f * M_PI / 180.0f;
            
            float fnx = rnx, fny = rny, fnz = rnz;
            rotate_point_x(&fny, &fnz, tX);
            rotate_point_y(&fnx, &fnz, tY);
            
            float shade = calculate_shading(fnx, fny, fnz);
            
            // Adjust Z-offset for blades to be slightly in front of hub to avoid flicker
            float blade_z_bias = 0.005f; 
            
            // Material Color: Silver/White with Glow hints

            // Root: Silver, Tip: White
            float t = blade_geom[k].x / 1.0f; // Normalized length [0, 1]
            float r0 = 180.0f/255.0f, g0 = 190.0f/255.0f, b0 = 210.0f/255.0f; // Metallic Blueish Silver
            float r1 = 250.0f/255.0f, g1 = 252.0f/255.0f, b1 = 255.0f/255.0f; // Near White
            
            float r = r0 + (r1-r0)*t;
            float g = g0 + (g1-g0)*t;
            float b = b0 + (b1-b0)*t;
            
            // Add a subtle blue glow based on shading and position
            float glow = 0.2f * sinf(t * M_PI);
            vtx->r = (r * shade) + glow * 0.2f;
            vtx->g = (g * shade) + glow * 0.8f;
            vtx->b = (b * shade) + glow * 1.0f;
            vtx->a = 1.0f;
            
            // Fix Z-fighting: Move blades to sit ON TOP of the hub face
            // Hub extends from -hubDepth/2 to +hubDepth/2
            float hub_face_z = 20.0f * fanScale / 2.0f;
            float z_clearance = 0.02f; // Lift blades slightly off the hub
            
            vtx->x = fanX + rx;
            vtx->y = fanY + ry;
            vtx->z = fanZ + rz + hub_face_z + z_clearance;
            
            vtx++;

        }
    }
    
    // Apply Global Tilt to all Fan Vertices (Hub + Blades)
    // Tilt back around X (~ -25 deg) and Turn around Y (~ 15 deg)
    float tiltX = -35.0f * M_PI / 180.0f;
    float tiltY = 15.0f * M_PI / 180.0f;
    
    vertex* p = fan_start_vtx;
    while(p < vtx) {
        float relX = p->x - fanX;
        float relY = p->y - fanY;
        float relZ = p->z - fanZ;
        
        rotate_point_x(&relY, &relZ, tiltX);
        rotate_point_y(&relX, &relZ, tiltY);
        
        p->x = fanX + relX;
        p->y = fanY + relY;
        p->z = fanZ + relZ;
        
        p++;
    }
    
    
    // Flush cache so GPU sees the new data
    GSPGPU_FlushDataCache(g_vbo_buffers[next_buf_idx], VBO_SIZE * sizeof(vertex));
    
    // Store vertex count for draw call
    g_vertex_count = vtx - g_vbo_buffers[next_buf_idx];
    
    // Swap buffer index for rendering
    g_cur_buf_idx = next_buf_idx;
}


static void render_3d_view(float iod) {
    C3D_BindProgram(&g_shader);
    C3D_SetAttrInfo(&g_attrInfo);
    
    // Update Buffer Info to point to the current active buffer
    BufInfo_Init(&g_bufInfo);
    BufInfo_Add(&g_bufInfo, g_vbo_buffers[g_cur_buf_idx], sizeof(vertex), 2, 0x10);
    C3D_SetBufInfo(&g_bufInfo);


    
    // Texture Env (Vertex Color)
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    
    // Disable culling for single-plane blades to be visible from both sides
    C3D_CullFace(GPU_CULL_NONE);





    
    // Matrix
    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(40.0f), C3D_AspectRatioTop, 0.5f, 100.0f, iod, 2.0f, false);
    
    C3D_Mtx modelView;
    Mtx_Identity(&modelView);
    Mtx_Translate(&modelView, 0.0f, 0.0f, -4.0f, false); // Camera depth
    
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_projection, &projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_modelView, &modelView);

    
    // Draw (3 bars * 36 + Hub * 36 + 3 blades * 36)
    // Use GEQUAL with 0 clear as it was known to be visible
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_DrawArrays(GPU_TRIANGLES, 0, g_vertex_count);


    
    // Restore State for C2D
    C3D_DepthTest(false, GPU_ALWAYS, 0);
    
    // Reset TexEnv to default (Modulate Texture * Color) for C2D Text/Sprites
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    
    // Restore C2D State (Shader, Attributes, etc.)
    C2D_Prepare();
}





static void DrawTopScreen(float offset, int layer) {
    C2D_Text text;
    char buf[64];
    
    // ---------------------------------------------------------
    // DEPTH LAYERS
    // ---------------------------------------------------------

    float d_back  = offset * 1.0f;
    float d_mid   = offset * 0.2f;   // Text/UI
    float d_super = offset * -2.5f;
    float d_max   = offset * -4.5f;
    
    if (layer == 0) {
        float fx = 210, fy = 125;
        float px = fx - 35;
        float py = fy + 52;
        // RPM Panel at bottom
        C2D_DrawRectangle(px, py, 70, 35, C2D_Color32(0, 0, 0, 180), 10, 10, 10, 10);
        C2D_DrawRectSolid(px, py, 0.51, 70, 35, C2D_Color32(0, 20, 30, 200));
        // Rounded border panel
        C2D_DrawCircleSolid(px+15, py+17, 0.51, 15, C2D_Color32(0, 20, 30, 200));
        C2D_DrawCircleSolid(px+55, py+17, 0.51, 15, C2D_Color32(0, 20, 30, 200));
        
        snprintf(buf, sizeof(buf), "%d", g_state.fan_rpm);
        C2D_TextBufClear(textBuf);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        snprintf(buf, sizeof(buf), "%d", g_state.fan_rpm);
        C2D_TextBufClear(textBuf);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        // Move Text Below Fan (Fan Y=190, R=15 => Bottom=205)
        C2D_DrawText(&text, C2D_WithColor, 332 + d_super - text.width*0.5f/2, 210, 0.52f, 0.5f, 0.5f, COL_CYAN);
        
        C2D_TextParse(&text, textBuf, "RPM");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 332 + d_super - text.width*0.35f/2, 222, 0.52f, 0.35f, 0.35f, COL_TEXT);
    }
    if (layer == 0) {
        // === LAYER 0: BACKGROUND (Frames, Slots) ===
        
        // 标题栏背景
        C2D_DrawRectSolid(10 + d_back, 8, 0, 380, 28, COL_PANEL);
        
        // CPU Frame
        C2D_DrawRectSolid(20 + d_back, 50, 0, 35, 140, COL_PANEL); 

        // RAM Frame
        C2D_DrawRectSolid(65 + d_back, 50, 0, 35, 140, COL_PANEL);
        
        // SWAP Frame
        C2D_DrawRectSolid(110 + d_back, 50, 0, 35, 140, COL_PANEL);
        
        // Temp BG
        C2D_DrawRectSolid(300 + d_mid, 55, 0, 90, 55, COL_PANEL); // CPU Temp
        C2D_DrawRectSolid(300 + d_mid, 55, 0, 90, 2, COL_CYAN);
        
        C2D_DrawRectSolid(300 + d_mid, 118, 0, 90, 55, COL_PANEL); // GPU Temp
        C2D_DrawRectSolid(300 + d_mid, 118, 0, 90, 2, COL_PURPLE);
        
        // 底部装饰线
        C2D_DrawRectSolid(0 + d_back, 237, 0, 400, 3, COL_CYAN);
        
        // LIVE指示灯
        C2D_DrawRectSolid(355 + d_super, 12, 0, 30, 16, COL_PANEL);
        C2D_DrawRectSolid(355 + d_super, 12, 0, 30, 2, COL_GREEN);
        C2D_DrawRectSolid(355 + d_super, 26, 0, 30, 2, COL_GREEN);
        
        // 数值数值背景 (Parallax Panels)
        // CPU/RAM/SWAP Bottom Panels
        C2D_DrawRectSolid(20 + d_super, 207, 0, 35, 16, COL_PANEL);
        C2D_DrawRectSolid(20 + d_super, 207, 0, 35, 2, COL_GREEN);
        C2D_DrawRectSolid(20 + d_super, 221, 0, 35, 2, COL_GREEN);
        
        C2D_DrawRectSolid(65 + d_super, 207, 0, 35, 16, COL_PANEL);
        C2D_DrawRectSolid(65 + d_super, 207, 0, 35, 2, COL_CYAN);
        C2D_DrawRectSolid(65 + d_super, 221, 0, 35, 2, COL_CYAN);
        
        C2D_DrawRectSolid(110 + d_super, 207, 0, 35, 16, COL_PANEL);
        C2D_DrawRectSolid(110 + d_super, 207, 0, 35, 2, COL_PURPLE);
        C2D_DrawRectSolid(110 + d_super, 221, 0, 35, 2, COL_PURPLE);
        
        // Temp Value Panels
        C2D_DrawRectSolid(312 + d_super, 75, 0, 75, 32, COL_PANEL);
        C2D_DrawRectSolid(312 + d_super, 75, 0, 75, 2, COL_CYAN);
        C2D_DrawRectSolid(312 + d_super, 105, 0, 75, 2, COL_CYAN);
        
        C2D_DrawRectSolid(312 + d_super, 138, 0, 75, 32, COL_PANEL);
        C2D_DrawRectSolid(312 + d_super, 138, 0, 75, 2, COL_PURPLE);
        C2D_DrawRectSolid(312 + d_super, 168, 0, 75, 2, COL_PURPLE);

        // Fan RPM Panel
        C2D_DrawRectSolid(192 + d_super, 186, 0, 36, 32, COL_PANEL);
        C2D_DrawRectSolid(192 + d_super, 186, 0, 36, 2, COL_CYAN);
        C2D_DrawRectSolid(192 + d_super, 216, 0, 36, 2, COL_CYAN);
        
    } else {
        // === LAYER 1: FOREGROUND (Text, Labels, Overlays) ===
        
        // 标题文字
        char display_host[64];
        strncpy(display_host, g_state.hostname, sizeof(display_host));
        char* dot = strstr(display_host, ".local");
        if (dot) *dot = '\0';
        snprintf(buf, sizeof(buf), "%s  %s", display_host, g_state.os_name);
        
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        
        float scale = 0.45f;
        if (strlen(buf) > 25) scale = 0.38f;
        if (strlen(buf) > 35) scale = 0.32f;
        
        C2D_DrawText(&text, C2D_WithColor, 18 + d_mid, 12, 0, scale, scale, COL_CYAN);
        
        // 运行时间
        int h = g_state.uptime_seconds / 3600;
        int m = (g_state.uptime_seconds % 3600) / 60;
        int s = g_state.uptime_seconds % 60;
        snprintf(buf, sizeof(buf), "UPTIME: %02d:%02d:%02d", h, m, s);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 245 + d_mid, 14, 0, 0.4f, 0.4f, COL_PURPLE);
        
        // LIVE
        C2D_TextParse(&text, textBuf, "LIVE");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 358 + d_super, 13, 0, 0.38f, 0.38f, COL_GREEN);

        // CPU Labels
        C2D_TextParse(&text, textBuf, "CPU");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 25 + d_mid, 195, 0, 0.45f, 0.45f, COL_GREEN);
        
        snprintf(buf, sizeof(buf), "%.0f%%", g_state.cpu_usage);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 22 + d_super, 208, 0, 0.35f, 0.35f, COL_TEXT);

        // RAM Labels
        C2D_TextParse(&text, textBuf, "RAM");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 70 + d_mid, 195, 0, 0.45f, 0.45f, COL_CYAN);
        
        if (g_state.memory_total_mb > 0) {
            snprintf(buf, sizeof(buf), "%.0f/%.0fG", g_state.memory_used_mb / 1024.0f, g_state.memory_total_mb / 1024.0f);
        } else {
            snprintf(buf, sizeof(buf), "%.0f%%", g_state.memory_usage);
        }
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 66 + d_super, 208, 0, 0.28f, 0.28f, COL_TEXT);
        
        // SWAP Labels
        C2D_TextParse(&text, textBuf, "SWAP");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 115 + d_mid, 195, 0, 0.45f, 0.45f, COL_PURPLE);
        
        float swapPct = g_state.swap_usage;
        snprintf(buf, sizeof(buf), "%.0f%%", swapPct);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 112 + d_super, 208, 0, 0.35f, 0.35f, COL_TEXT);
        
        // Fan (Overlay removed - moved to right)
        
        // CPU Temp Text
        C2D_TextParse(&text, textBuf, "CPU TEMP");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 305 + d_mid, 60, 0, 0.35f, 0.35f, COL_CYAN);
        
        snprintf(buf, sizeof(buf), "%.0f", g_state.cpu_temp);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 315 + d_super, 78, 0, 0.85f, 0.85f, COL_WHITE);
        
        C2D_TextParse(&text, textBuf, "C");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 360 + d_super, 82, 0, 0.5f, 0.5f, COL_CYAN);
        
        // GPU Temp Text
        C2D_TextParse(&text, textBuf, "GPU TEMP");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 305 + d_mid, 123, 0, 0.35f, 0.35f, COL_PURPLE);
        
        snprintf(buf, sizeof(buf), "%.0f", g_state.gpu_temp);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 315 + d_super, 141, 0, 0.85f, 0.85f, COL_WHITE);
        
        C2D_TextParse(&text, textBuf, "C");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 360 + d_super, 145, 0, 0.5f, 0.5f, COL_PURPLE);

        // RunCat Animation
        if (g_spriteSheet) {
            int cat_idx = (int)g_cat_anim_frame % 5;
            C2D_Image cat_img = C2D_SpriteSheetGetImage(g_spriteSheet, cat_idx);
            
            // Depth for cat (Pop out more than text)
            float d_cat = offset * -3.0f;
            
            float cat_x = 178.0f + d_cat; // Centered at 210. 32pix*2.0=64. 210-32=178.
            float cat_y = 93.0f;          // Centered at 125. 32pix*2.0=64. 125-32=93.
            
            // Force White Color (Blend 1.0)
            // Since texture is A8, the tint color defines the RGB, and Texture defines A.
            C2D_ImageTint tint;
            C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, 255), 1.0f);
            
            C2D_DrawImageAt(cat_img, cat_x, cat_y, 0.5f, &tint, 2.0f, 2.0f);
        }
    }
}


// ========================================
// Main
// ========================================
int main(int argc, char* argv[]) {
    // 初始化图形
    gfxInitDefault();
    gfxSet3D(true); // 开启3D模式
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    
    // 创建屏幕目标 (左右眼)
    C3D_RenderTarget* topScreenLeft = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* topScreenRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    
    bottomScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    
    // 创建文本缓冲
    textBuf = C2D_TextBufNew(2048);
    
    // 初始化网络 (在图形之后)
    init_network();
    
    // Initialize RomFS
    g_romfs_rc = romfsInit();
    if (R_FAILED(g_romfs_rc)) {
        printf("romfsInit failed: %08lx\n", g_romfs_rc);
    }
    
    // Load Sprite Sheet
    g_spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/cat.t3x");
    if (!g_spriteSheet) {
        printf("Failed to load sprites: romfs:/gfx/cat.t3x\n");
    }

    // 初始化 3D 资源
    init_3d();
    
    int uptime_counter = 0;

    
    // 主循环
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;
        
        // 触摸处理
        if (kDown & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);
            
            // 检测按钮
            if (touch.py >= 110 && touch.py <= 160) {
                for (int i = 0; i < 4; i++) {
                    float bx = 10 + i * 77;
                    if (touch.px >= bx && touch.px <= bx + 72) {
                        if (g_state.current_mode != i) {
                            g_state.current_mode = i;
                            
                            // 发送 FAN 命令到 server
                            if (g_socket >= 0 && g_server_found) {
                                const char* modes[] = {"FAN:TURBO", "FAN:SILENT", "FAN:CUSTOM", "FAN:AUTO"};
                                sendto(g_socket, modes[i], strlen(modes[i]), 0,
                                       (struct sockaddr*)&g_server_addr, sizeof(g_server_addr));
                            }
                        }
                        break;
                    }
                }
            }
        }
        
        // 更新网络
        network_update();
        
        // 更新时间
        uptime_counter++;
        if (uptime_counter >= 60) {
            uptime_counter = 0;
            g_state.uptime_seconds++;
        }
        
        // 更新动画
        g_frame++;
        float rpm_factor = (g_state.fan_rpm > 0) ? g_state.fan_rpm / 3000.0f : 0.5f;
        // Slower base, steeper curve for high RPM
        g_fan_angle -= 0.005f + rpm_factor * 0.08f;
        
        // Cat Animation Speed based on CPU Usage
        // Base speed + cpu dependent speed
        float cpu_factor = g_state.cpu_usage / 100.0f; // 0.0 to 1.0
        float cat_speed = 0.05f + cpu_factor * 0.5f; // Min 0.05, Max 0.55 per frame
        g_cat_anim_frame += cat_speed;
        
        // 更新功率历史
        g_power_history[g_power_idx] = g_state.power_watts;
        g_power_idx = (g_power_idx + 1) % POWER_HISTORY_SIZE;
        
        // 获取3D滑块值 (0.0 - 1.0)
        float slider = osGet3DSliderState();
        float base_offset = slider * 0.8f; 
        float iod = slider * 0.06f; // 3D Interocular distance

        // 更新 3D 几何
        update_3d_geometry();

        // ===== 渲染 =====
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        
        C2D_TextBufClear(textBuf); // 共用 buffer，每帧清空一次即可
        
        // === 左眼 ===
        // 1. BG Layer
        // Use C2D_TargetClear for correct color format (prevents red screen)
        C2D_TargetClear(topScreenLeft, COL_BG);
        // Clear Depth to 0 for GEQUAL compatibility
        C3D_RenderTargetClear(topScreenLeft, C3D_CLEAR_DEPTH, 0, 0);
        C2D_SceneBegin(topScreenLeft);



        
        DrawTopScreen(-base_offset, 0);
        C3D_DepthTest(false, GPU_ALWAYS, 0); // BG: No Depth Write
        C2D_Flush(); // Render background

        
        // 2. 3D Pass

        render_3d_view(-iod);
        
        // 3. FG Layer
        DrawTopScreen(-base_offset, 1);
        
        // === 右眼 ===
        // 1. BG Layer
        // Use C2D_TargetClear for correct color format
        C2D_TargetClear(topScreenRight, COL_BG);
        // Clear Depth to 0
        C3D_RenderTargetClear(topScreenRight, C3D_CLEAR_DEPTH, 0, 0);
        C2D_SceneBegin(topScreenRight);



        
        DrawTopScreen(base_offset, 0);
        C3D_DepthTest(false, GPU_ALWAYS, 0); // BG: No Depth Write
        C2D_Flush();

        
        // 2. 3D Pass

        render_3d_view(iod);
        
        // 3. FG Layer
        DrawTopScreen(base_offset, 1);
        
        // === 下屏 (2D) ===


        C2D_TargetClear(bottomScreen, COL_BG);
        C2D_SceneBegin(bottomScreen);
        
        C2D_Text text;
        char buf[64];
        
        // 功率图
        C2D_DrawRectSolid(8, 8, 0, 195, 88, COL_PANEL);
        C2D_DrawRectSolid(8, 8, 0, 195, 2, COL_CYAN);
        C2D_TextParse(&text, textBuf, "POWER CONSUMPTION (W)");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 12, 12, 0, 0.32f, 0.32f, COL_CYAN);
        
        snprintf(buf, sizeof(buf), "%.1fW", g_state.power_watts);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 130, 12, 0, 0.32f, 0.32f, COL_GREEN);
        
        // 波形
        float maxP = 50.0f;
        for (int i = 0; i < POWER_HISTORY_SIZE - 1; i++) {
            int idx = (g_power_idx + i) % POWER_HISTORY_SIZE;
            int nxt = (g_power_idx + i + 1) % POWER_HISTORY_SIZE;
            float x1 = 15 + i * 3.5f;
            float x2 = 15 + (i + 1) * 3.5f;
            float v1 = g_power_history[idx];
            float v2 = g_power_history[nxt];
            if (v1 < 1) v1 = 10 + 5 * sinf((i + g_frame) * 0.1f);
            if (v2 < 1) v2 = 10 + 5 * sinf((i + 1 + g_frame) * 0.1f);
            float y1 = 85 - (v1 / maxP) * 55;
            float y2 = 85 - (v2 / maxP) * 55;
            C2D_DrawLine(x1, y1, COL_CYAN, x2, y2, COL_CYAN, 2, 0);
        }
        
        // 频率
        C2D_DrawRectSolid(212, 8, 0, 100, 42, COL_PANEL);
        C2D_TextParse(&text, textBuf, "CORE CLOCK");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 216, 12, 0, 0.28f, 0.28f, COL_TEXT);
        snprintf(buf, sizeof(buf), "%.1f GHz", g_state.cpu_freq_mhz / 1000.0f);
        C2D_TextParse(&text, textBuf, buf);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 218, 28, 0, 0.48f, 0.48f, COL_CYAN);
        
        C2D_DrawRectSolid(212, 54, 0, 100, 42, COL_PANEL);
        C2D_TextParse(&text, textBuf, "HOST BATTERY");
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 216, 58, 0, 0.28f, 0.28f, COL_TEXT);
        
        u32 batCol = COL_GREEN;
        if (g_state.battery_level < 20) batCol = C2D_Color32(0xFF, 0x40, 0x40, 0xFF); // Red
        
        if (g_state.battery_level >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", g_state.battery_level);
            C2D_TextParse(&text, textBuf, buf);
            C2D_TextOptimize(&text);
            C2D_DrawText(&text, C2D_WithColor, 218, 74, 0, 0.48f, 0.48f, batCol);
            
            // 状态图标/文字
            const char* status = "";
            bool is_charging = false;
            
            if (strstr(g_state.battery_status, "Charging")) {
                status = "CHG";
                is_charging = true;
            } else if (strstr(g_state.battery_status, "Discharging")) {
                status = "BAT";
            } else if (strstr(g_state.battery_status, "Full")) {
                status = "FULL";
                is_charging = true; // Full usually implies connected
            } else if (strstr(g_state.battery_status, "AC Attached")) {
                status = "AC";
                is_charging = true; // Connected to power
            }
            
            // 覆盖颜色逻辑：如果充电中/接电源，不显示红色
            if (is_charging) {
                batCol = COL_GREEN;
            }
            
            C2D_TextParse(&text, textBuf, status);
            C2D_TextOptimize(&text);
            C2D_DrawText(&text, C2D_WithColor, 270, 78, 0, 0.35f, 0.35f, COL_TEXT);
        } else {
             C2D_TextParse(&text, textBuf, "N/A");
             C2D_TextOptimize(&text);
             C2D_DrawText(&text, C2D_WithColor, 218, 74, 0, 0.48f, 0.48f, COL_TEXT);
        }
        
        // 模式按钮
        const char* modes[] = {"TURBO", "SILENT", "CUSTOM", "CONFIG"};
        for (int i = 0; i < 4; i++) {
            float bx = 10 + i * 77;
            bool sel = (g_state.current_mode == i);
            u32 bg = sel ? C2D_Color32(0x00, 0x40, 0x60, 0xFF) : COL_PANEL;
            u32 border = sel ? COL_CYAN : COL_PURPLE;
            
            C2D_DrawRectSolid(bx, 108, 0, 72, 52, bg);
            C2D_DrawRectSolid(bx, 108, 0, 72, 2, border);
            C2D_DrawRectSolid(bx, 158, 0, 72, 2, border);
            C2D_DrawRectSolid(bx, 108, 0, 2, 52, border);
            C2D_DrawRectSolid(bx + 70, 108, 0, 2, 52, border);
            
            C2D_TextParse(&text, textBuf, modes[i]);
            C2D_TextOptimize(&text);
            C2D_DrawText(&text, C2D_WithColor, bx + 12, 128, 0, 0.42f, 0.42f, COL_TEXT);
        }
        
        // 状态栏
        C2D_DrawRectSolid(0, 218, 0, 320, 22, COL_PANEL);
        u32 dotCol = g_state.connected ? COL_GREEN : COL_ORANGE;
        C2D_DrawCircleSolid(14, 229, 0, 4, dotCol);
        
        const char* status = g_state.connected ? "CONNECTED // UDP:9001" : "SEARCHING...";
        C2D_TextParse(&text, textBuf, status);
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 24, 223, 0, 0.35f, 0.35f, COL_TEXT);
        
        C2D_TextOptimize(&text);
        C2D_DrawText(&text, C2D_WithColor, 280, 223, 0, 0.35f, 0.35f, COL_PURPLE);
        
        C3D_FrameEnd(0);
    }
    
    // 清理
    // 清理
    if (g_spriteSheet) C2D_SpriteSheetFree(g_spriteSheet);
    cleanup_network();
    romfsExit();
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    
    return 0;
}
