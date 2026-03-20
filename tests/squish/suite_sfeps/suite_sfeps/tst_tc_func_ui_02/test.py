# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    ensure_monitoring_tab()
    before = monitoring_event_count()

    inject_monitoring_event("TCUI02-001", "adult", "30s", True)
    wait_monitoring_event_added(before, 10000)

    open_monitoring_detail_first(15000)

    test.verify(exists_name("detailPopup"), "detailPopup visible")
    test.verify(exists_name("detailViewRoot"), "detailViewRoot visible")
    test.verify(exists_name("detailObjectIdValueText"), "object id field visible")
    test.verify(exists_name("detailCardTagValueText"), "card info field visible")
    test.verify(exists_name("detailEstimatedAgeValueText"), "estimated age field visible")
    test.passes("TC-FUNC-UI-02 PASS")
