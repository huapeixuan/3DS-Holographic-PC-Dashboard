/**
 * 赛博朋克仪表盘 - 主逻辑
 * 
 * 功能：
 * - WebSocket 连接接收实时数据
 * - 更新扁平化 UI 显示
 */

// ========================================
// 配置
// ========================================
const CONFIG = {
    wsUrl: 'ws://localhost:9000',
    reconnectDelay: 3000,
};

// ========================================
// 全局状态
// ========================================
let metrics = {
    cpu_usage: 0,
    memory_usage: 0,
    memory_total: 0,
    memory_used: 0,
    cpu_temp: null,
    fan_speeds: [],
    power_score: null,
};

// ========================================
// 初始化
// ========================================
function init() {
    initWebSocket();
}

// ========================================
// WebSocket 连接
// ========================================
function initWebSocket() {
    const statusEl = document.getElementById('connection-status');
    const textEl = statusEl.querySelector('.text');
    
    function connect() {
        const ws = new WebSocket(CONFIG.wsUrl);
        
        ws.onopen = () => {
            statusEl.classList.add('connected');
            statusEl.classList.remove('error');
            textEl.textContent = '已连接';
        };
        
        ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                if (data.type === 'connected') return;
                
                metrics = { ...metrics, ...data };
                updateUI();
            } catch (e) {
                console.error('解析数据失败:', e);
            }
        };
        
        ws.onclose = () => {
            statusEl.classList.remove('connected');
            textEl.textContent = '连接断开，重连中...';
            setTimeout(connect, CONFIG.reconnectDelay);
        };
        
        ws.onerror = (error) => {
            statusEl.classList.add('error');
            statusEl.classList.remove('connected');
            textEl.textContent = '连接错误';
            console.error('WebSocket 错误:', error);
        };
    }
    
    connect();
}

/**
 * 更新 UI 显示
 */
function updateUI() {
    // 更新文字指示器
    document.getElementById('cpu-text').textContent = 
        `CPU: ${metrics.cpu_usage.toFixed(1)}%`;
    document.getElementById('mem-text').textContent = 
        `内存: ${metrics.memory_usage.toFixed(1)}%`;
    document.getElementById('temp-text').textContent = 
        metrics.cpu_temp != null 
            ? `温度: ${metrics.cpu_temp.toFixed(0)}°C` 
            : '温度: N/A';
            
    document.getElementById('fan-text').textContent = 
        (metrics.fan_speeds && Array.isArray(metrics.fan_speeds) && metrics.fan_speeds.length > 0)
            ? `风扇: ${metrics.fan_speeds.map(s => typeof s === 'number' ? s.toFixed(0) : s).join(' / ')} RPM`
            : '风扇: 0 RPM';
            
    document.getElementById('power-text').textContent = 
        metrics.power_score != null
            ? `负荷: ${(metrics.power_score / 100000).toFixed(1)} W` 
            : '负荷: ---';
}

// 启动
document.addEventListener('DOMContentLoaded', init);
