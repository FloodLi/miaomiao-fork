# 课程表功能使用说明

## 功能概述

课程表功能允许你在墨水屏上显示每周的课程安排，支持：
- 7 天（周一至周日）课程管理
- 每天最多 12 节课
- 每节课包含：课程名称、开始时间、结束时间、教室位置
- 自动显示当前课程和下一节课信息
- 通过 Web API 配置课程表

## API 接口

### 1. 获取课程表配置（JSON）

**请求：**
```
GET /timetable.json
```

**说明：** 浏览器打开 **`GET /timetable`** 返回的是课程表配置网页（HTML）；拉取 JSON 请用 **`/timetable.json`**。

**响应示例：**
```json
{
  "enabled": true,
  "days": [
    {
      "day": 0,
      "slots": [
        {
          "name": "高等数学",
          "start_hour": 8,
          "start_minute": 0,
          "end_hour": 9,
          "end_minute": 35,
          "room": "A101",
          "enabled": true
        }
      ]
    }
  ]
}
```

### 2. 设置课程表配置

**请求：**
```
POST /timetable
Content-Type: application/json

{
  "enabled": true,
  "days": [
    {
      "day": 0,
      "slots": [
        {
          "name": "高等数学",
          "start_hour": 8,
          "start_minute": 0,
          "end_hour": 9,
          "end_minute": 35,
          "room": "A101",
          "enabled": true
        }
      ]
    }
  ]
}
```

**参数说明：**
- `enabled`: 是否启用课程表功能
- `days`: 包含 7 个元素的数组（周一到周日）
  - `day`: 星期几（0=周一，1=周二，...，6=周日）
  - `slots`: 课程列表
    - `name`: 课程名称（最多 32 字符）
    - `start_hour`: 开始小时（24 小时制）
    - `start_minute`: 开始分钟
    - `end_hour`: 结束小时
    - `end_minute`: 结束分钟
    - `room`: 教室位置（最多 16 字符）
    - `enabled`: 是否启用该课程

### 3. 显示课程表

**请求：**
```
POST /timetable_show
```

**响应：**
```
OK
```

在墨水屏上显示完整的课程表。

## 使用示例

### 使用 curl 设置课程表

```bash
# 设置课程表
curl -X POST http://192.168.4.1/timetable \
  -H "Content-Type: application/json" \
  -d @timetable_example.json

# 获取课程表 JSON
curl http://192.168.4.1/timetable.json

# 显示课程表
curl -X POST http://192.168.4.1/timetable_show
```

### 使用网页配置

1. 连接到 ESP32 的热点：`ESP32_EPD_XXXXXX`
2. 打开浏览器访问：`http://192.168.4.1`
3. 进入课程表配置页面
4. 添加/编辑课程
5. 保存配置

## 显示效果

### 完整课程表显示
- 顶部显示"课程表"标题
- 显示当前课程信息（如果有）
- 显示下一节课信息（如果有）
- 一周七天概览，显示每天的课程数量

### 自动显示
当启用课程表功能后，系统会自动：
- 检测当前是否有课
- 显示剩余时间
- 提示下一节课信息

## 数据结构

```c
#define TIMETABLE_MAX_DAYS    7    // 周一到周日
#define TIMETABLE_MAX_SLOTS   12   // 每天最多 12 节课
#define TIMETABLE_MAX_NAME_LEN 32  // 课程名称最大长度

typedef struct {
    char     name[TIMETABLE_MAX_NAME_LEN];  // 课程名称
    uint8_t  start_hour;                     // 开始小时
    uint8_t  start_minute;                   // 开始分钟
    uint8_t  end_hour;                       // 结束小时
    uint8_t  end_minute;                     // 结束分钟
    uint8_t  room[16];                       // 教室
    bool     enabled;                        // 是否启用
} timetable_slot_t;
```

## 注意事项

1. 课程表数据保存在 NVS（非易失性存储）中，断电不会丢失
2. 需要先同步时间（通过 WiFi 或 NTP）才能正确显示当前课程
3. 建议定期通过 Web 界面备份课程表配置
4. 课程时间不能重叠（系统不做检查，请自行安排）

## 示例课程表

参考项目根目录的 `timetable_example.json` 文件。
