#ifndef CLEANUP_H
#define CLEANUP_H

#include <atomic>
#include <cstdint>
#include <string>

// 파일 정리 스레드 함수
// save_dir: 삭제할 대상 디렉토리 경로
// retention_sec: 파일 보관 기간 (초 단위, 기본 86400초 = 1일)
void run_file_cleanup_worker(std::atomic<bool>& running_flag,
                             const std::string& save_dir,
                             long retention_sec = 86400,
                             std::uintmax_t max_storage_bytes = 5368709120ULL);
void run_fraud_image_cleanup_worker(std::atomic<bool>& running_flag,
                                    const std::string& save_dir,
                                    long retention_sec = 86400);
void run_pending_image_cleanup_worker(std::atomic<bool>& running_flag,
                                      const std::string& save_dir,
                                      long retention_sec = 86400);

#endif
