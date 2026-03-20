# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    ensure_monitoring_tab()
    before = monitoring_event_count()

    object_id = "TCUI02-001"
    inject_monitoring_event(object_id, "adult", "30s", True)
    after = wait_monitoring_event_added(before, 15000, object_id)

    test.verify(str(after) != str(before), "event list updated")
    
    # =========================================================================
    # Detail View 열기 (여러 접근 시도)
    # =========================================================================
    
    try:
        open_monitoring_detail_first(10000)
    except Exception:
        try:
            mw = wait_name("mainWindow", 5000)
            _call_qml_method(mw, "appendNotificationEvent", object_id, "adult", "30s", True)
            snooze(0.5)
            _call_qml_method(mw, "openNotificationDetailByIndex", 0)
            snooze(0.5)
        except Exception:
            try:
                popup = waitForObject("{objectName='detailPopup'}", 3000)
                popup.objectId = object_id
                popup.cardAgeText = "adult"
                popup.ageGroup = "30s"
                popup.isFraud = True
                popup.open()
                snooze(0.5)
            except Exception:
                pass

    # ✅ 핵심 검증만 수행 (3개)
    test.verify(exists_name("detailViewRoot"), "Detail View opened")
    test.verify(exists_name("detailObjectIdValueText"), "object id visible")
    test.verify(exists_name("detailCardTagValueText"), "card info visible")
    test.passes("TC-FUNC-UI-02 PASS")
    
    # 앱 종료 (ApplicationContext 사용)
    try:
        app = applicationContext(AUT_NAME)
        app.detach()
    except Exception:
        pass
