# SFEPS Server - 코드 품질 & 보안 검토

## 📋 코드 검토 점수

| 모듈 | 구조 | 스레드 안전성 | 에러 처리 | 보안 | 전반 평가 |
|------|------|------------|----------|------|---------|
| 인증 (auth) | ✅ A | ✅ A | ⚠️ C | ❌ D | ⚠️ C+ |
| 로깅 (log) | ✅ A | ✅ A | ✅ A | ✅ A | ✅ A |
| 녹화 (recorder) | ✅ A | ✅ A | ⚠️ B | ✅ A | ✅ A |
| 정리 (cleanup) | ✅ A | ✅ A | ✅ A | ✅ A | ✅ A |
| RFID (rfid_monitor) | ✅ A | ✅ A | ⚠️ B | ✅ A | ✅ A |
| **전체 평가** | ✅ A | ✅ A | ⚠️ B | ⚠️ C | ✅ **B+** |

---

## ✅ 우수 사항

### 1. 멀티스레드 안전성 (A+)

#### ✅ 올바른 뮤텍스 사용
```cpp
// auth.h - 동시 DB 접근 보호
std::lock_guard<std::mutex> dbLock(dbMutex);
if (mysql_query(conn, query.c_str())) { ... }
```

**평가**: RAII 패턴으로 안전한 잠금 관리

#### ✅ 조건 변수를 통한 효율적 대기
```cpp
// log.cpp - 워커 스레드가 필요할 때만 깨어남
std::unique_lock<std::mutex> lock(queueMutex);
cv.wait(lock, [this] { return !logQueue.empty() || !isRunning; });
```

**평가**: 바쁜 대기(busy-wait) 없음, CPU 효율적

#### ✅ 원자적 연산 사용
```cpp
// main.cpp, recorder.cpp - 경합 없는 플래그 체크
std::atomic<bool> running_flag;
while (running_flag) { ... }
```

**평가**: 메모리 배리어 자동 관리

### 2. 모듈화 설계 (A)

각 기능이 명확하게 분리:
- `Authenticator`: 인증 전담
- `DBLogger`: 로깅 전담
- `RTSPRecorder`: 녹화 전담
- `RfidMonitor`: RFID 수신 전담

**평가**: 단일 책임 원칙(SRP) 준수

### 3. 에러 복구 (A)

#### ✅ 자동 재연결
```cpp
// recorder.cpp - RTSP 연결 실패 시 자동 재시도
while (running_flag) {
    if (!connect_and_record()) 
        std::cerr << "[System] Connection Retry in 5s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
}
```

#### ✅ 신호 처리
```cpp
// main.cpp - 안전한 종료
std::signal(SIGINT, signal_handler);   // Ctrl+C
std::signal(SIGTERM, signal_handler);  // 강제 종료
```

### 4. 타임스탬프 관리 (A+)

```cpp
// recorder.cpp - FFmpeg 타임스탬프 보정
if (last_dts != AV_NOPTS_VALUE && pkt.dts <= last_dts) {
    int64_t diff = last_dts + 1 - pkt.dts;
    pkt.dts += diff;
    if (pkt.pts != AV_NOPTS_VALUE) pkt.pts += diff;
}
```

**평가**: 파일 이음새 시크 없이 연속 재생 가능

### 5. 메모리 관리 (A)

```cpp
// recorder.cpp - 리소스 정리
RTSPRecorder::~RTSPRecorder() { cleanup(); }

void RTSPRecorder::cleanup() {
    close_current_file();
    if (input_ctx) avformat_close_input(&input_ctx);
}
```

**평가**: RAII 패턴으로 누수 방지

---

## ⚠️ 보안 이슈

### 1️⃣ **CRITICAL: SQL Injection 취약점**

#### 문제 코드
```cpp
// auth.cpp - SQL Injection 취약
std::string query = "SELECT id FROM users WHERE id = '" + id + "' AND password = '" + pw + "'";
// 공격: id = "' OR '1'='1"; pw = "anything"
// 결과: SELECT id FROM users WHERE id = '' OR '1'='1' AND password = 'anything'
//       → 모든 행 반환 (인증 우회)
```

#### 심각도
🔴 **CRITICAL** - 인증 우회 가능

#### 해결책: Prepared Statement
```cpp
// ✅ 개선된 코드
const char* query = "SELECT id FROM users WHERE id = ? AND password = ?";
MYSQL_STMT* stmt = mysql_stmt_init(conn);
mysql_stmt_prepare(stmt, query, strlen(query));

MYSQL_BIND bind[2];
memset(bind, 0, sizeof(bind));

// 파라미터 바인딩
bind[0].buffer_type = MYSQL_TYPE_STRING;
bind[0].buffer = (void*)id.c_str();
bind[0].buffer_length = id.length();

bind[1].buffer_type = MYSQL_TYPE_STRING;
bind[1].buffer = (void*)pw.c_str();
bind[1].buffer_length = pw.length();

mysql_stmt_bind_param(stmt, bind);
mysql_stmt_execute(stmt);
```

**개선 효과**: 입력값이 완전히 격리됨

### 2️⃣ **HIGH: 비밀번호 평문 저장**

#### 문제점
```sql
-- DB 테이블 설계
CREATE TABLE users (
    id VARCHAR(50) PRIMARY KEY,
    password VARCHAR(100) NOT NULL,  -- ❌ 평문 저장!
    ...
);
```

#### 심각도
🟠 **HIGH** - DB 유출 시 모든 사용자 계정 탈취

#### 해결책: 해시 저장
```sql
-- ✅ 개선된 스키마
CREATE TABLE users (
    id VARCHAR(50) PRIMARY KEY,
    password_hash VARCHAR(255) NOT NULL,  -- bcrypt, Argon2, scrypt
    salt VARCHAR(32) NOT NULL,
    ...
);
```

```cpp
// ✅ bcrypt 사용 예시
#include <bcrypt/bcrypt.h>

std::string hashed = bcrypt::generateHash(password);
// DB에 저장: INSERT INTO users VALUES ('admin', hashed);

// 인증 시 검증
bool verified = bcrypt::validatePassword(input_pw, stored_hash);
```

### 3️⃣ **MEDIUM: 하드코딩된 DB 자격증명**

#### 문제 코드
```cpp
// main.cpp
#define DB_HOST "192.168.0.92"
#define DB_USER "pi"
#define DB_PASS "raspberry"
#define DB_NAME "Client_db"
```

#### 심각도
🟠 **MEDIUM** - 소스코드 노출 시 팀원 모두 영향

#### 해결책: 환경 변수 사용
```cpp
// ✅ 개선된 코드
const char* db_host = std::getenv("DB_HOST") ?: "localhost";
const char* db_user = std::getenv("DB_USER") ?: "root";
const char* db_pass = std::getenv("DB_PASS") ?: "";
const char* db_name = std::getenv("DB_NAME") ?: "app_db";
```

또는 설정 파일 사용:
```ini
# /etc/sfeps/server.conf
DB_HOST=192.168.0.92
DB_USER=pi
DB_PASS=raspberry
DB_NAME=Client_db
RTSP_URL=rtsps://192.168.0.92:8332/cam1
```

```cpp
// 설정 파일 파싱
std::ifstream config("/etc/sfeps/server.conf");
std::string line;
while (std::getline(config, line)) {
    if (line.find("DB_HOST=") == 0) {
        db_host = line.substr(8);
    }
    // ...
}
```

### 4️⃣ **MEDIUM: 입력 검증 부족**

#### 문제 코드
```cpp
// rfid_monitor.cpp - JSON 파싱
std::string uid = extract_json_value(json_line, "id");
save_to_db(uid, age_group, now);  // ❌ 유효성 검사 없음
```

#### 심각도
🟠 **MEDIUM** - 비정상 데이터 DB 저장 가능

#### 해결책: 입력 검증
```cpp
// ✅ 유효성 검사
bool is_valid_uid(const std::string& uid) {
    // UID는 10자리 알파벳/숫자
    if (uid.length() != 10) return false;
    for (char c : uid) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

bool is_valid_age_group(const std::string& age) {
    // 형식: "5_15", "16_25" 등
    if (age.length() < 3) return false;
    size_t underscore_pos = age.find('_');
    if (underscore_pos == std::string::npos) return false;
    
    std::string min_str = age.substr(0, underscore_pos);
    std::string max_str = age.substr(underscore_pos + 1);
    
    try {
        int min_age = std::stoi(min_str);
        int max_age = std::stoi(max_str);
        return (min_age >= 0 && max_age <= 130 && min_age < max_age);
    } catch (...) {
        return false;
    }
}

// 사용
if (is_valid_uid(uid) && is_valid_age_group(age_group)) {
    save_to_db(uid, age_group, now);
} else {
    std::cerr << "[RFID] Invalid data: " << uid << ", " << age_group << std::endl;
}
```

### 5️⃣ **MEDIUM: TLS 인증서 검증 비활성화**

#### 문제 코드
```cpp
// recorder.cpp - 보안 우회
av_dict_set(&opts, "tls_verify", "0", 0);  // ❌ 인증서 검증 안 함
```

#### 심각도
🟠 **MEDIUM** - 중간자 공격(MITM) 가능

#### 해결책: 인증서 검증 활성화
```cpp
// ✅ 인증서 검증 활성화 (프로덕션)
#ifdef PRODUCTION
av_dict_set(&opts, "tls_verify", "1", 0);
av_dict_set(&opts, "tls_ca_file", "/etc/ssl/certs/ca-bundle.crt", 0);
#else
av_dict_set(&opts, "tls_verify", "0", 0);  // 개발 환경만
#endif
```

또는 인증서 핀(Cert Pinning):
```cpp
// ✅ 특정 인증서만 신뢰
av_dict_set(&opts, "tls_cert", "/etc/sfeps/camera-cert.pem", 0);
```

---

## ⚠️ 에러 처리 개선 사항

### 1️⃣ **DB 연결 실패 처리**

#### 현재 코드
```cpp
// auth.cpp
if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) == NULL) {
    std::cerr << "[Auth DB Error] " << mysql_error(conn) << std::endl;
    return false;
}
```

#### 문제점
- 재시도 로직 없음
- 타임아웃 설정 없음

#### 개선안
```cpp
bool Authenticator::connect_with_retry(int max_retries = 3, int timeout_sec = 5) {
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        conn = mysql_init(NULL);
        if (conn == NULL) continue;
        
        // 타임아웃 설정
        unsigned int conn_timeout = timeout_sec;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &conn_timeout);
        
        if (mysql_real_connect(conn, host, user, pass, db_name, 3306, NULL, 0) != NULL) {
            std::cout << "[Auth] DB Connected (attempt " << attempt << ")" << std::endl;
            return true;
        }
        
        std::cerr << "[Auth DB Error] Attempt " << attempt << ": " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        
        if (attempt < max_retries) {
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        }
    }
    return false;
}
```

### 2️⃣ **파일 접근 권한 확인**

#### 현재 코드
```cpp
// cleanup.cpp - 권한 검사 없음
for (const auto& entry : fs::directory_iterator(save_dir)) {
    if (entry.is_regular_file()) {
        fs::remove(entry.path());  // ❌ 권한 에러 무시
    }
}
```

#### 개선안
```cpp
bool remove_with_error_handling(const fs::path& file_path) {
    try {
        // 파일 접근 권한 확인
        if (!fs::is_regular_file(file_path)) {
            std::cerr << "[Cleanup] Not a regular file: " << file_path << std::endl;
            return false;
        }
        
        // 읽기 권한 확인
        if (!(fs::status(file_path).permissions() & fs::perm::owner_read)) {
            std::cerr << "[Cleanup] No read permission: " << file_path << std::endl;
            return false;
        }
        
        fs::remove(file_path);
        std::cout << "[Cleanup] Deleted: " << file_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Cleanup Error] " << file_path << " - " << e.what() << std::endl;
        return false;
    }
}
```

---

## 🔐 보안 체크리스트

### 배포 전 확인사항

- [ ] **인증**
  - [ ] SQL Injection 방지 (Prepared Statement)
  - [ ] 비밀번호 해싱 (bcrypt/Argon2)
  - [ ] 세션 만료 시간 설정
  - [ ] 로그인 시도 제한

- [ ] **네트워크**
  - [ ] HTTPS/TLS 사용
  - [ ] 인증서 검증 활성화
  - [ ] 포트 화이트리스트 설정
  - [ ] 방화벽 규칙 적용

- [ ] **데이터베이스**
  - [ ] 최소 권한 원칙 (Least Privilege)
    ```sql
    -- ✅ 최소 권한 사용자 생성
    CREATE USER 'app_user'@'localhost' IDENTIFIED BY 'strong_password';
    GRANT SELECT, INSERT, UPDATE ON app_db.* TO 'app_user'@'localhost';
    ```
  - [ ] 데이터 암호화
  - [ ] 정기 백업

- [ ] **로깅**
  - [ ] 모든 인증 시도 기록
  - [ ] 에러 로그 보안
  - [ ] 감사 로그 활성화

- [ ] **코드**
  - [ ] 주석에 민감한 정보 없음
  - [ ] 하드코딩된 비밀정보 없음
  - [ ] 의존성 최신 버전 사용
  - [ ] 정적 분석 도구 통과 (cppcheck)

---

## 🧪 정적 분석

### cppcheck 실행

```bash
# 설치
sudo apt-get install cppcheck

# 분석
cd /home/iam/finalProject/SFEPS/server
cppcheck --enable=all --report-progress --output-file=cppcheck.txt \
    include/ src/

# 결과 확인
cat cppcheck.txt
```

### 가능한 문제

```
[src/auth.cpp:25]: (warning) Possible style: Variable 'query' is not const
[src/recorder.cpp:47]: (information) Ignoring return value of 'avio_open'
[src/RFID/rfid_monitor.cpp:89]: (style) Variable 'close_socket' assigned but not used
```

---

## 🔍 코드 리뷰 체크리스트

### 각 PR 제출 전

- [ ] 멀티스레드 안전성
  - [ ] 공유 변수에 뮤텍스 사용?
  - [ ] 데이터 경합 없음?
  - [ ] 데드락 위험 없음?

- [ ] 메모리 관리
  - [ ] 메모리 누수 없음?
  - [ ] 이중 해제 없음?
  - [ ] 범위 벗어난 접근 없음?

- [ ] 에러 처리
  - [ ] 모든 API 호출 반환값 확인?
  - [ ] NULL 포인터 체크?
  - [ ] 예외 안전?

- [ ] 성능
  - [ ] O(n²) 알고리즘 없음?
  - [ ] 필요 없는 복사 없음?
  - [ ] 바쁜 대기 없음?

---

## 📊 개선 우선순위

| 우선순위 | 항목 | 노력 | 효과 |
|---------|------|------|------|
| 🔴 P0 | SQL Injection 방지 | 중간 | 높음 |
| 🔴 P0 | 암호 해싱 | 낮음 | 높음 |
| 🟠 P1 | TLS 검증 활성화 | 낮음 | 중간 |
| 🟠 P1 | 입력 검증 | 중간 | 중간 |
| 🟡 P2 | 설정 파일화 | 낮음 | 낮음 |
| 🟡 P2 | 로깅 레벨 | 낮음 | 낮음 |

---

## 📚 참고 자료

- [OWASP Top 10](https://owasp.org/www-project-top-ten/)
- [CWE/SANS Top 25](https://cwe.mitre.org/top25/)
- [C++ Guidelines](https://github.com/isocpp/CppCoreGuidelines)
- [FFmpeg Security](https://ffmpeg.org/security.html)

---

마지막 업데이트: 2026-02-13

