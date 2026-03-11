#ifndef AUTH_H
#define AUTH_H

#include <string>
#include <mysql/mysql.h>
#include <mutex>

/**
 * @class Authenticator
 * @brief 사용자 로그인 인증을 담당하는 전용 클래스
 */
class Authenticator {
public:
    // 생성자: DB 접속 정보를 초기 설정함
    Authenticator(const char* host, const char* user, const char* pass, const char* db);
    ~Authenticator();

    // DB 연결 함수
    bool connect();
    
    // ID와 PW를 DB에서 대조하여 인증 여부 반환
    bool authenticate(const std::string& id, const std::string& pw);

private:
    MYSQL* conn;            // MariaDB 커넥션 객체
    MYSQL_STMT* authStmt;   // prepared statement handle
    std::string host;       // DB 호스트 주소
    std::string user;       // DB 사용자 ID
    std::string pass;       // DB 비밀번호
    std::string db_name;    // DB 이름
    std::mutex dbMutex;     // 멀티스레드 환경 보호용 뮤텍스
};

#endif
