# SFEPS DB Flow (Table-Centric)

Generated on 2026-03-19.

## 1) ER Diagram (Runtime-Used Columns)

```mermaid
erDiagram
    USERS {
        varchar id PK
        varchar password
        varchar name
    }

    LOGIN_LOGS {
        int id PK
        varchar username
        varchar ip_address
        varchar status
        timestamp created_at
    }

    ANALYTICS_LOGS {
        int id PK
        varchar object_id
        varchar card_age_text
        varchar age
        tinyint is_fraud
        timestamp created_at
    }

    RECORDINGS {
        int id PK
        varchar filename
        timestamp created_at
    }

    CARD_LOGS {
        int id PK
        varchar card_uid
        datetime tag_time
        varchar age_group
    }

    USERS ||..o{ LOGIN_LOGS : "logical reference (username -> users.id)"
```

## 2) Table-Centric Access Map (One Mermaid Per Table)

### 2-1) USERS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(users)]:::table
    F0["services/auth_service.cpp::run_login_auth_impl"]:::func
    F1["auth.cpp::Authenticator::authenticate"]:::func

    F0 --> F1
    F1 -- "SELECT" --> T
```

### 2-2) LOGIN_LOGS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(login_logs)]:::table
    F0["services/auth_service.cpp::run_login_auth_impl"]:::func
    F1["log.cpp::DBLogger::enqueueLogin"]:::func
    F2["log.cpp::DBLogger::processQueue"]:::func
    F3["main.cpp::t_db_cleanup + log.cpp::requestDbCleanup/processQueue"]:::func

    F0 --> F1 --> F2
    F2 -- "INSERT" --> T
    F3 -- "DELETE old rows" --> T
```

### 2-3) ANALYTICS_LOGS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(analytics_logs)]:::table
    F0["recorder.cpp::RTSPRecorder::process_meta_xml_chunk"]:::func
    F1["analytics.cpp::AnalyticsProcessor::publishRaw"]:::func
    F2["rfid_monitor.cpp::RfidMonitor::process_rfid_tag"]:::func
    F3["analytics.cpp::AnalyticsProcessor::onRfidRead"]:::func
    F4["analytics.cpp::AnalyticsProcessor::workerLoop"]:::func
    F5["analytics.cpp::AnalyticsProcessor::insertAnalyticsRow"]:::func
    F6["main.cpp::t_db_cleanup + log.cpp::requestDbCleanup/processQueue"]:::func

    F0 --> F1
    F2 --> F3 --> F4 --> F5
    F5 -- "INSERT (fraud=Y only)" --> T
    F6 -- "DELETE old rows" --> T
```

### 2-4) RECORDINGS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(recordings)]:::table
    F0["recorder.cpp::RTSPRecorder::close_current_file"]:::func
    F1["log.cpp::DBLogger::enqueueRecording"]:::func
    F2["log.cpp::DBLogger::processQueue"]:::func
    F3["services/video_catalog_service.cpp::run_video_catalog_service_impl"]:::func
    F4["main.cpp::t_db_cleanup + log.cpp::requestDbCleanup/processQueue"]:::func

    F0 --> F1 --> F2
    F2 -- "INSERT" --> T
    F3 -- "SELECT/COUNT" --> T
    F4 -- "DELETE old rows" --> T
```

### 2-5) CARD_LOGS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(card_logs)]:::table
    F0["rfid_monitor.cpp::RfidMonitor::run_loop"]:::func

    F0 -. "DB SQL 접근 없음" .-> T
```

Notes:
- DB는 런타임에서 `SFEPS_DB_NAME_ANALYTICS` 단일 스키마를 사용합니다.
- 물리 FK는 없고 `login_logs.username -> users.id`는 논리 참조입니다.
- `analytics_logs`는 현재 `object_id/card_age_text/age/is_fraud/created_at` 중심으로 기록합니다.
- `card_logs`는 코드에서 SQL INSERT/SELECT를 수행하지 않습니다.
