//! 3D 全息仪表盘服务端
//!
//! 采集系统信息并通过 WebSocket 和 UDP 实时推送给客户端
//! - WebSocket (端口 9000): 用于 Web 仪表盘
//! - UDP (端口 9001): 用于 3DS 客户端 (自动发现)

mod monitor;

use futures_util::{SinkExt, StreamExt};
use monitor::Monitor;
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use tokio::{
    net::{TcpListener, TcpStream, UdpSocket},
    sync::broadcast,
    time::interval,
};
use tokio_tungstenite::{accept_async, tungstenite::Message};

/// WebSocket 服务端口
const WS_PORT: u16 = 9000;
/// UDP 服务端口 (接收 3DS 心跳，发送数据)
const UDP_PORT: u16 = 9001;
/// 数据推送间隔 (毫秒)
const PUSH_INTERVAL_MS: u64 = 100;
/// 3DS 客户端超时时间 (秒)
const CLIENT_TIMEOUT_SECS: u64 = 10;

/// 已注册的 3DS 客户端
type ClientMap = Arc<Mutex<HashMap<SocketAddr, Instant>>>;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("🚀 3D 全息仪表盘服务端启动中...");

    // 创建广播通道，用于向所有 WebSocket 客户端推送数据
    let (tx, _rx) = broadcast::channel::<String>(16);
    let tx = Arc::new(tx);

    // 创建 UDP socket (绑定固定端口，接收 3DS 心跳)
    let udp_socket = Arc::new(UdpSocket::bind(format!("0.0.0.0:{}", UDP_PORT)).await?);
    
    // 已注册的 3DS 客户端列表
    let clients: ClientMap = Arc::new(Mutex::new(HashMap::new()));

    // 启动 UDP 接收任务 (接收 3DS 心跳和发现请求)
    let recv_socket = udp_socket.clone();
    let recv_clients = clients.clone();
    tokio::spawn(async move {
        let mut buf = [0u8; 64];
        loop {
            if let Ok((len, addr)) = recv_socket.recv_from(&mut buf).await {
                let msg = String::from_utf8_lossy(&buf[..len]);
                
                if msg.starts_with("DISCOVER") {
                    // 3DS 发送发现请求，回复 SERVER
                    println!("🔍 收到发现请求: {}", addr);
                    let _ = recv_socket.send_to(b"SERVER", addr).await;
                    
                    // 同时注册为客户端
                    let mut map = recv_clients.lock().unwrap();
                    map.insert(addr, Instant::now());
                }
                else if msg.starts_with("HELLO") || msg.starts_with("PING") {
                    let mut map = recv_clients.lock().unwrap();
                    let is_new = !map.contains_key(&addr);
                    map.insert(addr, Instant::now());
                    if is_new {
                        println!("🎮 新 3DS 客户端: {}", addr);
                    }
                }
                else if msg.starts_with("FAN:") {
                    // 处理风扇控制命令
                    let mode = msg.trim_start_matches("FAN:").trim().to_lowercase();
                    println!("🌀 收到风扇控制命令: {} (来自 {})", mode, addr);
                    
                    // 查找 temp_sensor 路径并执行
                    let mut possible_paths = vec![
                        "temp-sensor/temp_sensor".to_string(),
                        "../temp-sensor/temp_sensor".to_string(),
                        "server/temp-sensor/temp_sensor".to_string(),
                    ];

                    // 增加对当前目录下二进制文件的支持 (用于打包后运行)
                    if let Ok(exe_path) = std::env::current_exe() {
                        if let Some(parent) = exe_path.parent() {
                            possible_paths.push(parent.join("temp_sensor").to_string_lossy().to_string());
                        }
                    }
                    
                    let mut executed = false;
                    for path in possible_paths {
                        if std::path::Path::new(&path).exists() {
                            let output = std::process::Command::new("sudo")
                                .args([&path, "-s", &mode])
                                .output();
                            
                            match output {
                                Ok(out) => {
                                    let stdout = String::from_utf8_lossy(&out.stdout);
                                    let stderr = String::from_utf8_lossy(&out.stderr);
                                    if out.status.success() {
                                        println!("✅ 风扇模式已设置: {}", mode);
                                        println!("{}", stdout);
                                        let _ = recv_socket.send_to(format!("FAN_OK:{}", mode).as_bytes(), addr).await;
                                    } else {
                                        println!("❌ 设置失败: {}", stderr);
                                        let _ = recv_socket.send_to(format!("FAN_ERR:{}", stderr).as_bytes(), addr).await;
                                    }
                                    executed = true;
                                    break;
                                }
                                Err(e) => {
                                    println!("❌ 执行失败: {}", e);
                                }
                            }
                        }
                    }
                    
                    if !executed {
                        println!("❌ 未找到 temp_sensor 工具");
                        let _ = recv_socket.send_to(b"FAN_ERR:temp_sensor not found", addr).await;
                    }
                    
                    // 更新客户端心跳
                    let mut map = recv_clients.lock().unwrap();
                    map.insert(addr, Instant::now());
                }
            }
        }
    });

    // 启动系统监控线程
    let monitor_tx = tx.clone();
    let monitor_udp = udp_socket.clone();
    let monitor_clients = clients.clone();
    tokio::spawn(async move {
        let monitor = Arc::new(Mutex::new(Monitor::new()));
        let mut tick = interval(Duration::from_millis(PUSH_INTERVAL_MS));

        // 首次需要等待一小段时间让 sysinfo 收集初始数据
        tokio::time::sleep(Duration::from_millis(500)).await;

        loop {
            tick.tick().await;
            
            let metrics = {
                let mut m = monitor.lock().unwrap();
                m.refresh()
            };

            if let Ok(json) = serde_json::to_string(&metrics) {
                // 通过 WebSocket 广播
                let _ = monitor_tx.send(json.clone());
                
                // 发送给所有已注册的 3DS 客户端
                let addrs: Vec<SocketAddr> = {
                    let mut map = monitor_clients.lock().unwrap();
                    let timeout = Duration::from_secs(CLIENT_TIMEOUT_SECS);
                    // 清理超时的客户端
                    map.retain(|addr, last_seen| {
                        let alive = last_seen.elapsed() < timeout;
                        if !alive {
                            println!("⏰ 3DS 客户端超时: {}", addr);
                        }
                        alive
                    });
                    map.keys().cloned().collect()
                };
                
                for addr in addrs {
                    let _ = monitor_udp.send_to(json.as_bytes(), addr).await;
                }
            }
        }
    });

    // 启动 WebSocket 服务器
    let addr = SocketAddr::from(([0, 0, 0, 0], WS_PORT));
    let listener = TcpListener::bind(&addr).await?;
    
    println!("✅ WebSocket 服务已启动: ws://localhost:{}", WS_PORT);
    println!("✅ UDP 服务已启动: 端口 {} (等待 3DS 连接)", UDP_PORT);
    println!("📊 数据推送频率: 每 {}ms", PUSH_INTERVAL_MS);
    println!("\n💡 3DS 会自动发送心跳包注册自己\n");

    while let Ok((stream, peer)) = listener.accept().await {
        println!("🔗 新 WebSocket 连接: {}", peer);
        let tx = tx.clone();
        tokio::spawn(handle_connection(stream, peer, tx));
    }

    Ok(())
}

/// 处理单个 WebSocket 连接
async fn handle_connection(
    stream: TcpStream,
    peer: SocketAddr,
    tx: Arc<broadcast::Sender<String>>,
) {
    let ws_stream = match accept_async(stream).await {
        Ok(ws) => ws,
        Err(e) => {
            eprintln!("❌ WebSocket 握手失败 {}: {}", peer, e);
            return;
        }
    };

    let (mut ws_sender, mut ws_receiver) = ws_stream.split();
    let mut rx = tx.subscribe();

    // 发送欢迎消息
    let welcome = serde_json::json!({
        "type": "connected",
        "message": "欢迎连接到 3D 全息仪表盘"
    });
    let _ = ws_sender.send(Message::Text(welcome.to_string().into())).await;

    // 同时处理：接收客户端消息 & 推送监控数据
    loop {
        tokio::select! {
            // 接收广播的监控数据并发送给客户端
            result = rx.recv() => {
                match result {
                    Ok(msg) => {
                        if ws_sender.send(Message::Text(msg.into())).await.is_err() {
                            break;
                        }
                    }
                    Err(_) => continue,
                }
            }
            // 接收客户端消息（主要用于检测断开）
            msg = ws_receiver.next() => {
                match msg {
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Err(_)) => break,
                    _ => {}
                }
            }
        }
    }

    println!("🔌 WebSocket 断开: {}", peer);
}
