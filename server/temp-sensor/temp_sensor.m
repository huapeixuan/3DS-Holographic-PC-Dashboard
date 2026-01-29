/*
 * Apple Silicon 硬件监控工具 (温度 + 风扇 + 功耗 + 电池)
 */

#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// ==========================================
// SMC 定义
// ==========================================
// ... (omitted for brevity, assume existing SMC definitions are here or I need to keep them?)
// actually I should probably be careful not to delete the SMC stuff.
// I will rewrite the whole file to be safe since I need to insert headers at top and logic at bottom.
// Wait, replace_file_content with a huge block might be risky if I don't have the full content in context perfectly or if I mess up.
// I will use multi_replace for safer editing.

// Let's stick to the plan:
// 1. Add headers.
// 2. Add `getBatteryInfo` function.
// 3. Update `main` to call it and print JSON.

// I will do this in the next turn with multi_replace.


// ==========================================
// SMC 定义
// ==========================================

#define KERNEL_INDEX_SMC      2
#define SMC_CMD_READ_BYTES    5
#define SMC_CMD_WRITE_BYTES   6
#define SMC_CMD_READ_KEYINFO  9

typedef struct {
    char major;
    char minor;
    char build;
    char reserved;
    unsigned short release;
} SMCKeyData_vers;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpuPLimit;
    uint32_t gpuPLimit;
    uint32_t memPLimit;
} SMCKeyData_pLimitData;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    char dataAttributes;
} SMCKeyData_keyInfo;

typedef char SMCBytesT[32];

typedef struct {
    uint32_t key;
    SMCKeyData_vers vers;
    SMCKeyData_pLimitData pLimitData;
    SMCKeyData_keyInfo keyInfo;
    char result;
    char status;
    char data8;
    uint32_t data32;
    SMCBytesT bytes;
} SMCKeyData_t;

static io_connect_t smc_conn = 0;

int SMCOpen() {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("AppleSMC"));
    if (!service) return 0;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &smc_conn);
    IOObjectRelease(service);
    return (result == kIOReturnSuccess);
}

void SMCClose() {
    if (smc_conn) {
        IOServiceClose(smc_conn);
        smc_conn = 0;
    }
}

kern_return_t SMCCall(int index, SMCKeyData_t *inputStructure, SMCKeyData_t *outputStructure) {
    size_t structureInputSize = sizeof(SMCKeyData_t);
    size_t structureOutputSize = sizeof(SMCKeyData_t);
    return IOConnectCallStructMethod(smc_conn, index, inputStructure, structureInputSize, outputStructure, &structureOutputSize);
}

// 转换四字节编码为字符串 (用于调试 dataType)
void typeToString(uint32_t type, char *str) {
    str[0] = (type >> 24) & 0xFF;
    str[1] = (type >> 16) & 0xFF;
    str[2] = (type >> 8) & 0xFF;
    str[3] = type & 0xFF;
    str[4] = '\0';
}

double SMCReadKey(const char *key, int debug) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = (key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC GetInfo failed for key %s\n", key);
        return -1.0;
    }

    uint32_t size = outputStructure.keyInfo.dataSize;
    uint32_t type = outputStructure.keyInfo.dataType;
    
    // 如果是调试模式，打印类型信息
    if (debug) {
        char typeStr[5];
        typeToString(type, typeStr);
        printf("Key: %s, Size: %d, Type: %s (0x%08X)\n", key, size, typeStr, type);
    }

    inputStructure.keyInfo.dataSize = size;
    inputStructure.data8 = SMC_CMD_READ_BYTES;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC ReadBytes failed for key %s\n", key);
        return -1.0;
    }

    unsigned char *bytes = (unsigned char *)outputStructure.bytes;
    
    if (debug) {
        printf("Bytes: ");
        for(int i=0; i<size; i++) printf("%02X ", bytes[i]);
        printf("\n");
    }

    // 解析常见类型
    
    // flt (float)
    if (type == ('f' << 24 | 'l' << 16 | 't' << 8 | ' ')) {
        float f;
        memcpy(&f, bytes, 4);
        return (double)f;
    }
    
    // fpe2 (fixed point, 2 integer bits, 14 fractional bits?)
    // 很多地方说 F0Ac 是 fpe2。
    // fpe2 = unsigned fixed point 2.14
    if (type == ('f' << 24 | 'p' << 16 | 'e' << 8 | '2')) {
        uint16_t val = (bytes[0] << 8) | bytes[1];
        return (double)val / 4.0;
    }
    
    // ui8
    if (type == ('u' << 24 | 'i' << 16 | '8' << 8 | ' ')) {
        return (double)bytes[0];
    }
    
    // ui16
    if (type == ('u' << 24 | 'i' << 16 | '1' << 8 | '6')) {
        uint16_t val = (bytes[0] << 8) | bytes[1];
        return (double)val;
    }
    
    // ui32
    if (type == ('u' << 24 | 'i' << 16 | '3' << 8 | '2')) {
        uint32_t val = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
        return (double)val;
    }
    
    // 默认回退：尝试当作 float 解析（因为 Apple Silicon 很喜欢用 float）
    if (size == 4) {
        float f;
        memcpy(&f, bytes, 4);
        return (double)f;
    }
    
    return 0.0;
}

// ==========================================
// SMC 写入功能
// ==========================================

int SMCWriteKey(const char *key, uint16_t value, int debug) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = (key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC GetInfo failed for key %s\n", key);
        return 0;
    }

    uint32_t size = outputStructure.keyInfo.dataSize;
    
    inputStructure.keyInfo.dataSize = size;
    inputStructure.data8 = SMC_CMD_WRITE_BYTES;
    
    // fpe2 格式: 乘以 4 然后大端存储
    uint16_t encoded = value * 4;
    inputStructure.bytes[0] = (encoded >> 8) & 0xFF;
    inputStructure.bytes[1] = encoded & 0xFF;
    
    if (debug) {
        printf("Writing key %s: value=%d, encoded=0x%04X\n", key, value, encoded);
    }

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC WriteBytes failed for key %s\n", key);
        return 0;
    }
    
    return 1;
}

// ==========================================
// SMC 写入功能 (参考 Stats 项目实现)
// ==========================================

// 获取键的数据类型
uint32_t SMCGetKeyType(const char *key) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = (key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        return 0;
    }
    
    return outputStructure.keyInfo.dataType;
}

// 写入风扇速度 (支持 flt 和 fpe2 格式)
int SMCWriteFanSpeed(const char *key, int speed, int debug) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = (key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC GetInfo failed for key %s\n", key);
        return 0;
    }

    uint32_t size = outputStructure.keyInfo.dataSize;
    uint32_t type = outputStructure.keyInfo.dataType;
    
    inputStructure.keyInfo.dataSize = size;
    inputStructure.data8 = SMC_CMD_WRITE_BYTES;
    
    // 根据数据类型选择编码方式
    // flt (0x666c7420 = "flt ")
    if (type == (('f' << 24) | ('l' << 16) | ('t' << 8) | ' ')) {
        float f = (float)speed;
        memcpy(inputStructure.bytes, &f, 4);
        if (debug) printf("Writing key %s (flt): speed=%d, float=%.2f\n", key, speed, f);
    }
    // fpe2 (0x66706532 = "fpe2")
    else if (type == (('f' << 24) | ('p' << 16) | ('e' << 8) | '2')) {
        // Stats 项目的 FPE2 编码: speed >> 6 和 (speed << 2) ^ ((speed >> 6) << 8)
        inputStructure.bytes[0] = (uint8_t)(speed >> 6);
        inputStructure.bytes[1] = (uint8_t)((speed << 2) ^ ((speed >> 6) << 8));
        if (debug) printf("Writing key %s (fpe2): speed=%d, bytes=[0x%02X, 0x%02X]\n", 
                         key, speed, inputStructure.bytes[0], inputStructure.bytes[1]);
    }
    else {
        // 未知类型，尝试 fpe2 格式
        inputStructure.bytes[0] = (uint8_t)(speed >> 6);
        inputStructure.bytes[1] = (uint8_t)((speed << 2) ^ ((speed >> 6) << 8));
        if (debug) {
            char typeStr[5];
            typeStr[0] = (type >> 24) & 0xFF;
            typeStr[1] = (type >> 16) & 0xFF;
            typeStr[2] = (type >> 8) & 0xFF;
            typeStr[3] = type & 0xFF;
            typeStr[4] = '\0';
            printf("Writing key %s (unknown type '%s'): speed=%d\n", key, typeStr, speed);
        }
    }

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC WriteBytes failed for key %s\n", key);
        return 0;
    }
    
    return 1;
}

// 写入单字节值 (用于 F0Md 模式设置)
int SMCWriteByte(const char *key, uint8_t value, int debug) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = (key[0] << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC GetInfo failed for key %s\n", key);
        return 0;
    }

    inputStructure.keyInfo.dataSize = outputStructure.keyInfo.dataSize;
    inputStructure.data8 = SMC_CMD_WRITE_BYTES;
    inputStructure.bytes[0] = value;
    
    if (debug) printf("Writing key %s: value=%d\n", key, value);

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC WriteBytes failed for key %s\n", key);
        return 0;
    }
    
    return 1;
}

// 写入 FS! 键 (2字节)
int SMCWriteFS(uint8_t mode, int debug) {
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;
    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = ('F' << 24) | ('S' << 16) | ('!' << 8) | ' ';
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC GetInfo failed for FS!\n");
        return 0;
    }

    inputStructure.keyInfo.dataSize = outputStructure.keyInfo.dataSize;
    inputStructure.data8 = SMC_CMD_WRITE_BYTES;
    inputStructure.bytes[0] = 0;
    inputStructure.bytes[1] = mode;
    
    if (debug) printf("Writing FS!: mode=%d\n", mode);

    if (SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure) != kIOReturnSuccess) {
        if (debug) printf("SMC WriteBytes failed for FS!\n");
        return 0;
    }
    
    return 1;
}

// 设置风扇模式 (参考 Stats 项目)
int setFanMode(const char *mode, int debug) {
    if (!SMCOpen()) {
        printf("Failed to open SMC. Need sudo?\n");
        return 0;
    }
    
    int fanCount = (int)SMCReadKey("FNum", 0);
    if (fanCount < 1) fanCount = 1;
    if (fanCount > 2) fanCount = 2;
    
    if (debug) printf("Fan count: %d\n", fanCount);
    
    // 读取风扇极限值
    double minRPM = SMCReadKey("F0Mn", debug);
    double maxRPM = SMCReadKey("F0Mx", debug);
    
    if (minRPM < 500) minRPM = 1200;   // 默认最小
    if (maxRPM < 2000) maxRPM = 6000;  // 默认最大
    
    if (debug) printf("Fan limits: min=%.0f, max=%.0f\n", minRPM, maxRPM);
    
    int targetRPM = 0;
    int useAuto = 0;
    
    if (strcmp(mode, "turbo") == 0) {
        targetRPM = (int)maxRPM;
        printf("TURBO mode: Setting fans to MAX (%d RPM)\n", targetRPM);
    } else if (strcmp(mode, "silent") == 0) {
        targetRPM = (int)minRPM;
        printf("SILENT mode: Setting fans to MIN (%d RPM)\n", targetRPM);
    } else if (strcmp(mode, "custom") == 0) {
        targetRPM = (int)((minRPM + maxRPM) / 2);
        printf("CUSTOM mode: Setting fans to MID (%d RPM)\n", targetRPM);
    } else if (strcmp(mode, "auto") == 0) {
        useAuto = 1;
        printf("AUTO mode: Restoring automatic fan control\n");
    } else {
        printf("Unknown mode: %s\n", mode);
        printf("Valid modes: turbo, silent, custom, auto\n");
        SMCClose();
        return 0;
    }
    
    if (useAuto) {
        // 方法1: 重设 FS! 为 0
        SMCWriteFS(0, debug);
        
        // 方法2: 设置各风扇模式为自动 (F0Md = 0)
        for (int i = 0; i < fanCount; i++) {
            char key[5];
            snprintf(key, sizeof(key), "F%dMd", i);
            SMCWriteByte(key, 0, debug);
        }
        
        printf("Auto mode restored.\n");
    } else {
        // 设置强制模式
        
        // 方法1: 使用 F0Md (Stats 项目的首选方法)
        // F0Md = 1 表示强制模式
        int useMd = 0;
        for (int i = 0; i < fanCount; i++) {
            char key[5];
            snprintf(key, sizeof(key), "F%dMd", i);
            if (SMCReadKey(key, 0) >= 0) {
                useMd = 1;
                SMCWriteByte(key, 1, debug);  // 1 = 强制模式
                if (debug) printf("Set %s = 1 (forced mode)\n", key);
            }
        }
        
        if (!useMd) {
            // 方法2: 使用 FS! 位掩码 (老方法)
            uint8_t fsMask = (fanCount > 1) ? 0x03 : 0x01;
            SMCWriteFS(fsMask, debug);
        }
        
        // 设置各风扇目标转速
        for (int i = 0; i < fanCount; i++) {
            char key[5];
            snprintf(key, sizeof(key), "F%dTg", i);
            SMCWriteFanSpeed(key, targetRPM, debug);
        }
        
        printf("Fan speed set to %d RPM.\n", targetRPM);
    }
    
    // 短暂等待后读取实际速度验证
    usleep(100000);  // 100ms
    double actualSpeed = SMCReadKey("F0Ac", 0);
    if (actualSpeed > 0) {
        printf("Current fan speed: %.0f RPM\n", actualSpeed);
    }
    
    SMCClose();
    return 1;
}

// ... (HID 相关代码保持不变，为了节省空间这里省略，但我会包含完整的 main 函数) ...
// 但为了编译，我必须包含 HID 代码

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
#ifdef __LP64__
typedef double IOHIDFloat;
#else
typedef float IOHIDFloat;
#endif

IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef client, CFDictionaryRef match);
IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef, int64_t, int32_t, int64_t);
CFStringRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service, CFStringRef property);
IOHIDFloat IOHIDEventGetFloatValue(IOHIDEventRef event, int32_t field);
CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef);
#define IOHIDEventFieldBase(type) (type << 16)
#define kIOHIDEventTypeTemperature 15
#define kIOHIDEventTypePower 25

CFDictionaryRef matching(int page, int usage) {
    CFNumberRef nums[2];
    CFStringRef keys[2];
    keys[0] = CFStringCreateWithCString(0, "PrimaryUsagePage", 0);
    keys[1] = CFStringCreateWithCString(0, "PrimaryUsage", 0);
    nums[0] = CFNumberCreate(0, kCFNumberSInt32Type, &page);
    nums[1] = CFNumberCreate(0, kCFNumberSInt32Type, &usage);
    CFDictionaryRef dict = CFDictionaryCreate(0, (const void**)keys, (const void**)nums, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(keys[0]); CFRelease(keys[1]); CFRelease(nums[0]); CFRelease(nums[1]);
    return dict;
}

typedef struct { char name[64]; double value; } SensorData;

int getHIDData(int page, int usage, int eventType, SensorData *sensors, int maxCount) {
    CFDictionaryRef dict = matching(page, usage);
    IOHIDEventSystemClientRef system = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    IOHIDEventSystemClientSetMatching(system, dict);
    CFArrayRef services = IOHIDEventSystemClientCopyServices(system);
    if (!services) { CFRelease(dict); return 0; }
    long count = CFArrayGetCount(services);
    int valid = 0;
    for (int i = 0; i < count && valid < maxCount; i++) {
        IOHIDServiceClientRef sc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
        CFStringRef name = IOHIDServiceClientCopyProperty(sc, CFSTR("Product"));
        IOHIDEventRef event = IOHIDServiceClientCopyEvent(sc, eventType, 0, 0);
        if (event) {
            if (name) {
                NSString *nsName = (__bridge NSString *)name;
                const char *cName = [nsName UTF8String];
                if (cName) {
                    strncpy(sensors[valid].name, cName, 63);
                    sensors[valid].name[63] = '\0';
                    sensors[valid].value = IOHIDEventGetFloatValue(event, IOHIDEventFieldBase(eventType));
                    valid++;
                }
                CFRelease(name);
            }
            CFRelease(event);
        }
    }
    CFRelease(services); CFRelease(dict);
    return valid;
}

double getPeakTemp() {
    SensorData sensors[64];
    int count = getHIDData(0xff00, 5, kIOHIDEventTypeTemperature, sensors, 64);
    double max = 0;
    for(int i=0; i<count; i++) {
        if(strstr(sensors[i].name, "battery")) continue;
        if(sensors[i].value > max && sensors[i].value < 150) max = sensors[i].value;
    }
    return max;
}

int main(int argc, char *argv[]) {
    int jsonMode = 0;
    int debugMode = 0;
    char *setMode = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0) {
            jsonMode = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debugMode = 1;
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--set") == 0) && i + 1 < argc) {
            setMode = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Apple Silicon Hardware Monitor\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -j, --json     Output in JSON format\n");
            printf("  -d, --debug    Debug mode (show raw SMC data)\n");
            printf("  -s, --set MODE Set fan mode (turbo/silent/custom/auto)\n");
            printf("  -h, --help     Show this help\n");
            return 0;
        }
    }
    
    // 如果指定了 -s，只执行设置模式然后退出
    if (setMode) {
        return setFanMode(setMode, debugMode) ? 0 : 1;
    }

    // SMC 风扇
    double fanSpeeds[2] = {0, 0};
    int fanCount = 0;
    
    if (SMCOpen()) {
        if (debugMode) printf("--- SMC Debug ---\n");
        double fNum = SMCReadKey("FNum", debugMode);
        fanCount = (int)fNum;
        if (debugMode) printf("Fan Count: %d\n", fanCount);
        
        if (fanCount > 2) fanCount = 2;
        
        if (fanCount >= 1) fanSpeeds[0] = SMCReadKey("F0Ac", debugMode);
        if (fanCount >= 2) fanSpeeds[1] = SMCReadKey("F1Ac", debugMode);
        
        SMCClose();
    } else {
        if (debugMode) printf("Failed to open SMC (need root?)\n");
    }

    // HID 数据
    SensorData powerSensors[64];
    SensorData voltageSensors[64];
    int pCount = getHIDData(0xff08, 2, kIOHIDEventTypePower, powerSensors, 64);
    int vCount = getHIDData(0xff08, 3, kIOHIDEventTypePower, voltageSensors, 64);
    
    double avgVoltage = 0;
    int validVoltages = 0;
    for(int i=0; i<vCount; i++) {
        if(voltageSensors[i].value > 0) { avgVoltage += voltageSensors[i].value; validVoltages++; }
    }
    if (validVoltages > 0) avgVoltage /= validVoltages;
    
    double sumAmps = 0;
    for(int i=0; i<pCount; i++) {
        double val = powerSensors[i].value;
        if (val > 1000) val /= 1000.0;
        sumAmps += val;
    }
    double estimatedPower = sumAmps * (avgVoltage > 0 ? avgVoltage : 3.8);
    double cpuTemp = getPeakTemp();

    // 电池信息
    typedef struct {
        int percent;
        char status[32]; // "Charging", "Discharging", "Full", "AC Attached", "Unknown"
    } BatteryInfo;
    
    BatteryInfo batt = { -1, "Unknown" };
    
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    CFArrayRef sources = IOPSCopyPowerSourcesList(blob);
    if (sources) {
        int count = CFArrayGetCount(sources);
        if (count > 0) {
            // 只取第一个电源
            CFTypeRef source = CFArrayGetValueAtIndex(sources, 0);
            CFDictionaryRef dict = IOPSGetPowerSourceDescription(blob, source);
            if (dict) {
                // Percentage
                CFNumberRef capacity = CFDictionaryGetValue(dict, CFSTR(kIOPSCurrentCapacityKey));
                if (capacity) {
                    CFNumberGetValue(capacity, kCFNumberIntType, &batt.percent);
                }
                
                // Status
                CFStringRef state = CFDictionaryGetValue(dict, CFSTR(kIOPSPowerSourceStateKey));
                CFBooleanRef isCharging = CFDictionaryGetValue(dict, CFSTR(kIOPSIsChargingKey));
                CFBooleanRef isFull = CFDictionaryGetValue(dict, CFSTR(kIOPSIsChargedKey));
                
                // 默认状态
                if (isCharging && CFBooleanGetValue(isCharging)) {
                    strcpy(batt.status, "Charging");
                } else if (isFull && CFBooleanGetValue(isFull)) {
                     strcpy(batt.status, "Full");
                } else {
                     // 检查 AC Power
                     if (state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) {
                         strcpy(batt.status, "AC Attached");
                     } else {
                         strcpy(batt.status, "Discharging");
                     }
                }
            }
        }
        CFRelease(sources);
    }
    CFRelease(blob);

    if (jsonMode) {
        printf("{\n");
        printf("  \"cpu_temp\": %.1f,\n", cpuTemp);
        printf("  \"fan_speed\": [%.0f, %.0f],\n", fanSpeeds[0], fanSpeeds[1]);
        printf("  \"estimated_power_score\": %.2f,\n", estimatedPower);
        printf("  \"battery_percentage\": %d,\n", batt.percent);
        printf("  \"battery_status\": \"%s\"\n", batt.status);
        printf("}\n");
    } else if (debugMode) {
        printf("\n--- Result ---\n");
        printf("Temp: %.1f C\n", cpuTemp);
        printf("Fan: %.1f / %.1f RPM\n", fanSpeeds[0], fanSpeeds[1]);
    } else {
        printf("Temp: %.1f C\n", cpuTemp);
        printf("Fan: %.0f RPM / %.0f RPM\n", fanSpeeds[0], fanSpeeds[1]);
    }
    
    return 0;
}
