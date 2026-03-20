# -*- coding: utf-8 -*-

import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    set_stream_status_for_test("DISCONNECTED")

    status_text = text_name("streamStatusLabel", 10000)
    test.verify("OFFLINE" in status_text, "스트림 상태 텍스트 OFFLINE 확인")

    banner = wait_name("streamNoticeBanner", 10000)
    test.verify(banner.visible, "스트림 장애 배너 표시 확인")

    clear_stream_status_for_test()
    test.passes("TC-FUNC-STREAM-02 PASS")
