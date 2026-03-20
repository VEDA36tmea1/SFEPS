# -*- coding: utf-8 -*-

import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    set_tracking_active(True, "TC-TRACK-001")
    test.compare(text_name("trackingStateLabel", 10000), "TRACKING ON")

    set_tracking_active(False)
    test.compare(text_name("trackingStateLabel", 10000), "TRACKING OFF")

    test.passes("TC-FUNC-TRACK-02 PASS")
