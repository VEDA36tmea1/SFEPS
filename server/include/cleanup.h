#ifndef CLEANUP_H
#define CLEANUP_H

#include <atomic>
#include <string>

// 파일 정리 스레드 함수
// save_dir: 삭제할 대상 디렉토리 경로
// retention_sec: 파일 보관 기간 (초 단위, 기본 60초)
void run_file_cleanup_worker(std::atomic<bool>& running_flag, const std::string& save_dir, long retention_sec = 60);

#endif