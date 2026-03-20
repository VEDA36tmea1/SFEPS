# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    ensure_monitoring_tab()
    before = monitoring_event_count()

    object_id = "TCUI01-001"
    inject_monitoring_event(object_id, "adult", "30s", True)
    snooze(1)  # Wait for event to be registered in UI
    after = wait_monitoring_event_added(before, 15000, object_id)

    test.verify(str(after) != str(before), "event list updated")
    test.passes("TC-FUNC-UI-01 PASS")
