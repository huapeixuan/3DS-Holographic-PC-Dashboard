//! 系统信息采集模块
//! 
//! 采集 CPU 使用率、内存使用、CPU 温度、风扇转速等信息
//! 使用内置的 temp_sensor（IOKit HID API + AppleSMC）获取 Apple Silicon 硬件数据

use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::process::Command;
use sysinfo::{System, Components};
use libmacchina::{
    GeneralReadout, BatteryReadout, KernelReadout,
    traits::{GeneralReadout as _, BatteryReadout as _, KernelReadout as _},
};

/// 系统监控数据结构
#[derive(Debug, Serialize, Clone)]
pub struct SystemMetrics {
    /// CPU 使用率 (%)
    pub cpu_usage: f32,
    /// CPU 频率 (MHz)
    pub cpu_frequency_mhz: u64,
    /// 内存使用率 (%)
    pub memory_usage: f32,
    /// 总内存 (MB)
    pub memory_total: u64,
    /// 已用内存 (MB)
    pub memory_used: u64,
    /// Swap 使用率 (%)
    pub swap_usage: f32,
    /// CPU 温度 (°C)
    pub cpu_temp: Option<f32>,
    /// GPU 温度 (°C) - Apple Silicon 为 SOC 温度估算
    pub gpu_temp: Option<f32>,
    /// 风扇转速 (RPM) - 数组，支持多风扇
    pub fan_speeds: Vec<f32>,
    /// 估算功耗 (系统负荷分数)
    pub power_score: Option<f32>,
    
    // ===== libmacchina 新增字段 =====
    /// 主机名
    pub hostname: Option<String>,
    /// 操作系统名称
    pub os_name: Option<String>,
    /// 内核版本
    pub kernel_version: Option<String>,
    /// CPU 型号
    pub cpu_model: Option<String>,
    /// CPU 核心数
    pub cpu_cores: Option<usize>,
    /// 系统运行时间 (秒)
    pub uptime_secs: Option<u64>,
    /// 电池电量 (%)
    pub battery_percentage: Option<u8>,
    /// 电池状态 (Charging/Discharging/Full)
    pub battery_status: Option<String>,
    /// 分辨率
    pub resolution: Option<String>,
}

/// temp_sensor 工具的 JSON 输出格式
#[derive(Debug, Deserialize)]
struct SensorOutput {
    cpu_temp: f32,
    fan_speed: Vec<f32>,
    estimated_power_score: f32,
    // Optional because older temp_sensor binaries might not include these
    battery_percentage: Option<u8>,
    battery_status: Option<String>,
}

/// 系统监控器
pub struct Monitor {
    system: System,
    /// 跨平台温度组件信息
    components: Components,
    /// 温度传感器路径（如果可用）
    temp_sensor_path: Option<PathBuf>,
    /// 缓存的传感器数据
    cached_sensor_data: Option<SensorOutput>,
    /// 刷新计数器
    refresh_counter: u32,
    /// libmacchina readouts (缓存静态信息)
    general_readout: GeneralReadout,
    battery_readout: BatteryReadout,
    kernel_readout: KernelReadout,
    /// 缓存的静态系统信息
    cached_hostname: Option<String>,
    cached_os_name: Option<String>,
    cached_kernel_version: Option<String>,
    cached_cpu_model: Option<String>,
    cached_cpu_cores: Option<usize>,
    cached_resolution: Option<String>,
}

impl Monitor {
    /// 创建新的监控器实例
    pub fn new() -> Self {
        // 查找 temp_sensor 可执行文件
        let temp_sensor_path = Self::find_temp_sensor();
        
        if let Some(ref path) = temp_sensor_path {
            println!("✅ 硬件监控源: temp_sensor ({})", path.display());
            println!("   (包含: CPU温度, 风扇转速, 功耗估算)");
        } else {
            println!("⚠️  未找到 temp_sensor 工具");
            println!("   请编译: cd server/temp-sensor && clang -framework IOKit -framework Foundation -o temp_sensor temp_sensor.m");
        }
        
        // 初始化 libmacchina readouts
        let general_readout = GeneralReadout::new();
        let battery_readout = BatteryReadout::new();
        let kernel_readout = KernelReadout::new();
        
        // 缓存静态系统信息 (只需获取一次)
        let cached_hostname = general_readout.hostname().ok();
        let cached_os_name = general_readout.os_name().ok();
        let cached_kernel_version = kernel_readout.os_release().ok();
        let cached_cpu_model = general_readout.cpu_model_name().ok();
        let cached_cpu_cores = general_readout.cpu_cores().ok();
        let cached_resolution = general_readout.resolution().ok();
        
        println!("✅ libmacchina: 已初始化系统信息采集");
        if let Some(ref host) = cached_hostname {
            println!("   主机名: {}", host);
        }
        if let Some(ref cpu) = cached_cpu_model {
            println!("   CPU: {}", cpu);
        }

        Self {
            system: System::new_all(),
            components: Components::new_with_refreshed_list(),
            temp_sensor_path,
            cached_sensor_data: None,
            refresh_counter: 0,
            general_readout,
            battery_readout,
            kernel_readout,
            cached_hostname,
            cached_os_name,
            cached_kernel_version,
            cached_cpu_model,
            cached_cpu_cores,
            cached_resolution,
        }
    }
    
    /// 查找 temp_sensor 可执行文件
    fn find_temp_sensor() -> Option<PathBuf> {
        // 可能的路径列表
        let possible_paths = [
            // 相对于当前工作目录
            PathBuf::from("temp-sensor/temp_sensor"),
            // 相对于可执行文件目录（开发时）
            PathBuf::from("../temp-sensor/temp_sensor"),
            // server 目录下
            PathBuf::from("server/temp-sensor/temp_sensor"),
        ];
        
        for path in possible_paths {
            if path.exists() {
                // 验证可以执行并返回 JSON
                if let Ok(output) = Command::new(&path).arg("-j").output() {
                    if output.status.success() {
                        // 尝试解析一次 JSON 确保格式正确
                        let stdout = String::from_utf8_lossy(&output.stdout);
                        if serde_json::from_str::<SensorOutput>(&stdout).is_ok() {
                             return path.canonicalize().ok().or(Some(path));
                        }
                    }
                }
            }
        }
        
        // 尝试绝对路径
        if let Ok(cwd) = std::env::current_dir() {
            let abs_path = cwd.join("temp-sensor/temp_sensor");
            if abs_path.exists() {
                 return Some(abs_path);
            }
        }
        
        None
    }
    
    /// 从 temp_sensor 获取完整数据
    fn get_sensor_data(&self) -> Option<SensorOutput> {
        let path = self.temp_sensor_path.as_ref()?;
        
        // 运行 temp_sensor -j 获取 JSON 输出
        let output = Command::new(path)
            .arg("-j")
            .output()
            .ok()?;
        
        if !output.status.success() {
            return None;
        }
        
        let stdout = String::from_utf8_lossy(&output.stdout);
        serde_json::from_str(&stdout).ok()
    }

    /// 刷新并获取最新的系统指标
    pub fn refresh(&mut self) -> SystemMetrics {
        // 刷新 CPU、内存和温度信息
        self.system.refresh_cpu_usage();
        self.system.refresh_memory();
        self.components.refresh();

        // 计算 CPU 平均使用率
        let cpu_usage = self.system.cpus().iter()
            .map(|cpu| cpu.cpu_usage())
            .sum::<f32>() / self.system.cpus().len() as f32;

        // 获取 CPU 频率 (取平均值)
        let cpu_frequency_mhz = self.system.cpus().iter()
            .map(|cpu| cpu.frequency())
            .sum::<u64>() / self.system.cpus().len() as u64;

        // 计算内存使用
        let memory_total = self.system.total_memory() / 1024 / 1024;
        let memory_used = self.system.used_memory() / 1024 / 1024;
        let memory_usage = (memory_used as f32 / memory_total as f32) * 100.0;

        // 计算 Swap 使用率
        let swap_total = self.system.total_swap();
        let swap_used = self.system.used_swap();
        let swap_usage = if swap_total > 0 {
            (swap_used as f32 / swap_total as f32) * 100.0
        } else {
            0.0
        };

        // 获取传感器数据（每 10 次刷新调用一次，避免频繁调用外部进程）
        if self.temp_sensor_path.is_some() {
            self.refresh_counter += 1;
            if self.refresh_counter >= 10 || self.cached_sensor_data.is_none() {
                self.refresh_counter = 0;
                self.cached_sensor_data = self.get_sensor_data();
            }
        } else {
            self.cached_sensor_data = None;
        }

        // 提取数据
        let (mut cpu_temp, fan_speeds, mut power_score) = if let Some(ref data) = self.cached_sensor_data {
            (Some(data.cpu_temp), data.fan_speed.clone(), Some(data.estimated_power_score))
        } else {
            (None, vec![], None)
        };

        // 如果 temp_sensor 不可用或数据为空，尝试从 sysinfo::Components 获取温度 (Linux/Windows 兼容)
        if cpu_temp.is_none() {
            let mut max_temp = 0.0f32;
            for component in &self.components {
                let label = component.label().to_lowercase();
                // 寻找包含 core, package, cpu 等关键词的传感器
                if label.contains("core") || label.contains("package") || label.contains("cpu") || label.contains("soc") {
                    let t = component.temperature();
                    if t > max_temp && t < 150.0 {
                        max_temp = t;
                    }
                }
            }
            if max_temp > 0.0 {
                cpu_temp = Some(max_temp);
            }
        }

        // 功耗分数回退逻辑：基于 CPU 使用率估算 (适用于非 macOS)
        if power_score.is_none() {
            // 基准功耗 2.0 (空闲) + CPU 贡献 + 内存压力贡献
            let estimated = 2.0 + (cpu_usage * 0.15) + (memory_usage * 0.05);
            power_score = Some(estimated);
        }

        // GPU 温度 = CPU 温度 + 少量偏移 (Apple Silicon SOC 集成)
        let gpu_temp = cpu_temp.map(|t| t + 3.0 + cpu_usage * 0.05);
        
        // 获取动态数据 (电池状态, uptime)
        // 优先使用 temp_sensor 的数据，因为它更准确 (能识别 AC Attached)
        let (ts_battery_limit, ts_battery_status) = if let Some(ref data) = self.cached_sensor_data {
            (data.battery_percentage, data.battery_status.clone())
        } else {
            (None, None)
        };

        let battery_percentage = ts_battery_limit.or_else(|| self.battery_readout.percentage().ok());
        
        // 电池状态逻辑: temp_sensor > libmacchina > 推断
        let battery_status = if let Some(status) = ts_battery_status {
             Some(status)
        } else if let Some(pct) = battery_percentage {
            match self.battery_readout.status() {
                Ok(s) => Some(match s {
                    libmacchina::traits::BatteryState::Charging => "Charging".to_string(),
                    libmacchina::traits::BatteryState::Discharging => "Discharging".to_string(),
                }),
                Err(_) => {
                    // libmacchina currently only supports Charging/Discharging.
                    // If status fails but we have percentage, try to infer.
                    if pct >= 95 {
                        Some("Full".to_string())
                    } else {
                        Some("Unknown".to_string())
                    }
                }
            }
        } else {
            None
        };
        let uptime_secs = self.general_readout.uptime().ok().map(|v| v as u64);

        SystemMetrics {
            cpu_usage,
            cpu_frequency_mhz,
            memory_usage,
            memory_total,
            memory_used,
            swap_usage,
            cpu_temp,
            gpu_temp,
            fan_speeds,
            power_score,
            // libmacchina 字段
            hostname: self.cached_hostname.clone(),
            // hostname: Some("MY-MacBook".to_string()),
            os_name: self.cached_os_name.clone(),
            kernel_version: self.cached_kernel_version.clone(),
            cpu_model: self.cached_cpu_model.clone(),
            cpu_cores: self.cached_cpu_cores,
            uptime_secs,
            battery_percentage,
            battery_status,
            resolution: self.cached_resolution.clone(),
        }
    }
}

impl Default for Monitor {
    fn default() -> Self {
        Self::new()
    }
}
