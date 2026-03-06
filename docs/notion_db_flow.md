# SFEPS DB Flow (Table-Centric)

Generated on 2026-03-05.

## 1) ER Diagram

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
        varchar frame_time
        varchar object_type
        timestamp created_at
        int estimated_age
        varchar photo_path
        double x
        double y
        varchar event
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
    F0["auth.cpp::Authenticator::connect"]:::func
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
    F0["main.cpp::evaluate_auth"]:::func
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
    F0["recorder.cpp::RTSPRecorder::connect_and_record"]:::func
    F1["analytics.cpp::AnalyticsProcessor::publishRaw"]:::func
    F2["analytics.cpp::AnalyticsProcessor::workerLoop/processLine"]:::func
    F3["analytics.cpp::AnalyticsProcessor::insertAnalyticsRow"]:::func
    F4["main.cpp::t_db_cleanup + log.cpp::requestDbCleanup/processQueue"]:::func

    F0 --> F1 --> F2 --> F3
    F3 -- "INSERT" --> T
    F4 -- "DELETE old rows" --> T
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
    F3["main.cpp::t_db_cleanup + log.cpp::requestDbCleanup/processQueue"]:::func

    F0 --> F1 --> F2
    F2 -- "INSERT" --> T
    F3 -- "DELETE old rows" --> T
```

### 2-5) CARD_LOGS

```mermaid
flowchart TB
    classDef table fill:#f3f6fa,stroke:#2f3a4a,stroke-width:2px,color:#111;
    classDef func fill:#ffffff,stroke:#8a94a6,color:#111;

    T[(card_logs)]:::table
    F0["rfid_monitor.cpp::RfidMonitor::save_to_db"]:::func

    F0 -. "SQL INSERT/SELECT 미구현" .-> T
```

Notes:
- 물리 FK는 없고 `login_logs.username -> users.id`는 논리 참조입니다.
- `card_logs`는 스키마는 있지만 현재 코드에서 SQL로 접근하지 않습니다.
