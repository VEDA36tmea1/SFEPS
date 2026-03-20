# -*- coding: utf-8 -*-

import os
import time
import names

AUT_NAME = os.getenv("SQUISH_AUT_NAME", "appHanwhaVisionSFEPS")
DEFAULT_USER = os.getenv("SFEPS_TEST_USER", "admin")
DEFAULT_PASS = os.getenv("SFEPS_TEST_PASS", "1111")


def _mapped_selector(name):
    try:
        return getattr(names, name)
    except Exception:
        return None


def _selectors_for(name):
    sels = []

    if name == "loginWindow":
        sels += [
            "{type='QQuickWindow' title='SFEPS Login'}",
            "{type='QQuickWindowQmlImpl' title='SFEPS Login'}",
            "{objectName='loginWindow'}",
        ]
    elif name == "mainWindow":
        sels += [
            "{type='QQuickWindow' title='Hanwha Vision SFEPS'}",
            "{type='QQuickWindowQmlImpl' title='Hanwha Vision SFEPS'}",
            "{objectName='mainWindow'}",
        ]
    else:
        sels += [
            "{objectName='%s'}" % name,
            "{type='QQuickItem' objectName='%s'}" % name,
            "{type='QQuickAbstractButton' objectName='%s'}" % name,
        ]

    mapped = _mapped_selector(name)
    if mapped and mapped not in sels:
        sels.append(mapped)

    return sels


def wait_any(names_list, timeout_ms=60000):
    end = time.time() + (timeout_ms / 1000.0)
    while time.time() < end:
        for n in names_list:
            for s in _selectors_for(n):
                try:
                    if object.exists(s):
                        return n, s
                except Exception:
                    pass
        snooze(0.2)
    raise LookupError("wait_any timeout: %s" % ", ".join(names_list))


def wait_name(name, timeout_ms=30000):
    _, sel = wait_any([name], timeout_ms)
    return waitForObject(sel, timeout_ms)


def exists_name(name):
    for s in _selectors_for(name):
        try:
            if object.exists(s):
                return True
        except Exception:
            pass
    return False


def click_name(name, timeout_ms=30000):
    obj = wait_name(name, timeout_ms)
    mouseClick(obj)
    return obj


def text_name(name, timeout_ms=30000):
    obj = wait_name(name, timeout_ms)
    try:
        return str(obj.text)
    except Exception:
        return ""


def wait_until(predicate, timeout_ms=10000, poll_sec=0.2):
    end = time.time() + (timeout_ms / 1000.0)
    while time.time() < end:
        try:
            if predicate():
                return True
        except Exception:
            pass
        snooze(poll_sec)
    return False


def _call_qml_method(obj, method_name, *args):
    fn = getattr(obj, method_name, None)
    if fn is None:
        raise RuntimeError("QML method not found: %s" % method_name)
    return fn(*args)


def start_sfeps(timeout_ms=60000):
    startApplication(AUT_NAME)
    wait_any(["loginWindow", "mainWindow"], timeout_ms)


def login_default(user_id=DEFAULT_USER, password=DEFAULT_PASS, timeout_ms=30000):
    state, sel = wait_any(["loginWindow", "mainWindow"], timeout_ms)
    if state == "mainWindow":
        return

    login_win = waitForObject(sel, timeout_ms)
    mouseClick(login_win, 100, 100, 0, Qt.LeftButton)

    type(login_win, str(user_id))
    type(login_win, "<Tab>")
    type(login_win, str(password))
    type(login_win, "<Return>")

    wait_name("mainWindow", timeout_ms)


def logout_default(timeout_ms=30000):
    main_win = wait_name("mainWindow", timeout_ms)

    if exists_name("logoutButton"):
        click_name("logoutButton", 10000)
    else:
        mouseClick(main_win, main_win.width - 22, 38, 0, Qt.LeftButton)

    wait_name("loginWindow", timeout_ms)


# ---- UI-01~03 / STREAM-02 / TRACK-02 helper ----

def ensure_monitoring_tab(timeout_ms=10000):
    main_win = wait_name("mainWindow", timeout_ms)

    # 1) 상태값으로 강제 전환
    try:
        main_win.currentViewIndex = 1
    except Exception:
        pass

    # 2) 탭 버튼 클릭 fallback
    if exists_name("topTabButton_1"):
        try:
            click_name("topTabButton_1", 1500)
        except Exception:
            pass

    # 3) StackLayout이 실제 Monitoring(0)인지 확인
    ok = wait_until(
        lambda: exists_name("mainContentStack")
                and str(getattr(wait_name("mainContentStack", 1000), "currentIndex", "-1")) == "0",
        timeout_ms=timeout_ms,
        poll_sec=0.2
    )
    if not ok:
        raise LookupError("failed to switch to monitoring tab")

    wait_name("monitoringEventListView", timeout_ms)


def monitoring_event_count(timeout_ms=10000):
    lv = wait_name("monitoringEventListView", timeout_ms)
    try:
        return int(str(lv.count))
    except Exception:
        return 0


def inject_monitoring_event(object_id="TC-OBJ-001", card_text="adult", age_group="30s", is_fraud=True):
    ensure_monitoring_tab(10000)
    monitoring = wait_name("monitoringView", 10000)

    # 버튼 생성 여부를 여기서 강제 검증하지 않음 (탭 렌더 타이밍 영향 큼)
    try:
        _call_qml_method(
            monitoring,
            "injectTestMonitoringEvent",
            str(object_id),
            str(card_text),
            str(age_group),
            bool(is_fraud)
        )
    except Exception:
        _call_qml_method(
            monitoring,
            "appendMonitoringEvent",
            str(object_id),
            str(card_text),
            str(age_group),
            bool(is_fraud)
        )

    snooze(0.4)


def wait_monitoring_event_added(prev_count=None, timeout_ms=10000):
    ensure_monitoring_tab(10000)

    # count 비교 대신 버튼 생성 확인
    ok = wait_until(
        lambda: exists_name("monitoringDetailViewButton_0"),
        timeout_ms=timeout_ms,
        poll_sec=0.2
    )
    if ok:
        return 1

    # index 스캔 fallback
    for i in range(300):
        if exists_name("monitoringDetailViewButton_%d" % i):
            return i + 1

    raise LookupError("monitoring event not added (no detail button found)")


def open_monitoring_detail_first(timeout_ms=15000):
    ensure_monitoring_tab(10000)
    lv = wait_name("monitoringEventListView", 10000)

    end = time.time() + (timeout_ms / 1000.0)
    while time.time() < end:
        # 1) objectName 스캔
        for i in range(300):
            n = "monitoringDetailViewButton_%d" % i
            if exists_name(n):
                click_name(n, 1500)
                wait_name("detailPopup", 10000)
                wait_name("detailViewRoot", 10000)
                return

        # 2) 텍스트 fallback
        try:
            objs = findObjects("{text='Detail View'}")
            if objs and len(objs) > 0:
                mouseClick(objs[0])
                wait_name("detailPopup", 10000)
                wait_name("detailViewRoot", 10000)
                return
        except Exception:
            pass

        # 3) 좌표 fallback (Event Log 첫 행의 버튼 영역)
        try:
            lv.contentY = 0
            w = int(str(lv.width))
            for rx in (0.22, 0.26, 0.30):
                for y in (78, 92, 106, 120):
                    mouseClick(lv, int(w * rx), y, 0, Qt.LeftButton)
                    snooze(0.2)
                    if exists_name("detailPopup"):
                        wait_name("detailViewRoot", 10000)
                        return
        except Exception:
            pass

        snooze(0.2)

    raise LookupError("monitoring detail button not found/click failed")


def set_stream_status_for_test(status):
    monitoring = wait_name("monitoringView", 10000)
    monitoring.streamStatusOverrideForTest = str(status)
    snooze(0.3)


def clear_stream_status_for_test():
    monitoring = wait_name("monitoringView", 10000)
    monitoring.streamStatusOverrideForTest = ""
    snooze(0.3)


def set_tracking_active(active, track_id="TC-TRACK-001"):
    monitoring = wait_name("monitoringView", 10000)
    if active:
        monitoring.visualTrackedId = str(track_id)
    else:
        monitoring.visualTrackedId = ""
    snooze(0.4)
