# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    ensure_monitoring_tab()
    snooze(2)  # Wait for monitoringEventModel to finish initialization (drain timer)

    before = monitoring_event_count()

    object_id = "TCUI01-001"
    inject_monitoring_event(object_id, "adult", "30s", True)
    after = wait_monitoring_event_added(before, 20000, object_id)

    test.verify(str(after) != str(before), "event list updated")
    test.passes("TC-FUNC-UI-01 PASS")

    test.verify(str(after) != str(before), "event list updated")
    test.passes("TC-FUNC-UI-01 PASS")
