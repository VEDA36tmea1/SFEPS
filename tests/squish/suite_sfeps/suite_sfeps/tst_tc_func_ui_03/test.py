# -*- coding: utf-8 -*-
import names
source(findFile("scripts", "common.py"))

def main():
    start_sfeps()
    login_default("admin", "1111")

    logout_default(30000)
    test.verify(exists_name("loginWindow"), "returned to login screen")
    test.passes("TC-FUNC-UI-03 PASS")
