# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    ensure_monitoring_tab()
    
    # =========================================================================
    # TC-FUNC-TRACK-01: Track/Untrack 상태 확인 테스트
    # (set_tracking_active 헬퍼 사용 - track_02와 유사하지만 더 자세한 검증)
    # =========================================================================
    
    object_id = "TRACK-TEST-001"
    
    # ---- Stage 1: 초기 상태 확인 ----
    state_label = text_name("trackingStateLabel", 3000)
    test.compare("TRACKING OFF", state_label, "Initial state is TRACKING OFF")
    
    # ---- Stage 2: Track 활성화 ----
    set_tracking_active(True, object_id)
    snooze(0.5)
    state_label = text_name("trackingStateLabel", 3000)
    test.compare("TRACKING ON", state_label, "After Track: state is TRACKING ON")
    
    snooze(0.5)
    
    # ---- Stage 3: Track 비활성화 ----
    set_tracking_active(False)
    snooze(0.5)
    state_label = text_name("trackingStateLabel", 3000)
    test.compare("TRACKING OFF", state_label, "After Untrack: state is TRACKING OFF")
    
    # =========================================================================
    # 테스트 완료
    # =========================================================================
    test.passes("TC-FUNC-TRACK-01 PASS - Track/Untrack state transitions verified")
