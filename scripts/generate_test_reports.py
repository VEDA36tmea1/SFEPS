#!/usr/bin/env python3
from __future__ import annotations

import argparse
import html
import shutil
import subprocess
import textwrap
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable
from xml.sax.saxutils import escape as xml_escape


@dataclass
class TestCaseRow:
    suite: str
    classname: str
    name: str
    status: str
    elapsed_sec: float
    detail: str


def parse_int(value: str | None) -> int:
    try:
        return int(value or "0")
    except ValueError:
        return 0


def parse_float(value: str | None) -> float:
    try:
        return float(value or "0")
    except ValueError:
        return 0.0


def squash_spaces(value: str) -> str:
    return " ".join((value or "").split())


def collect_suite_nodes(root: ET.Element) -> list[ET.Element]:
    if root.tag == "testsuite":
        return [root]
    if root.tag == "testsuites":
        return root.findall("testsuite")
    suites = root.findall(".//testsuite")
    return suites if suites else [root]


def parse_junit_reports(xml_paths: Iterable[Path]) -> tuple[dict[str, float], list[TestCaseRow]]:
    summary = {
        "tests": 0,
        "failures": 0,
        "errors": 0,
        "skipped": 0,
        "time": 0.0,
    }
    rows: list[TestCaseRow] = []

    for xml_path in sorted(xml_paths):
        try:
            root = ET.parse(xml_path).getroot()
        except ET.ParseError as exc:
            rows.append(
                TestCaseRow(
                    suite=xml_path.name,
                    classname="parse",
                    name="xml-parse-error",
                    status="error",
                    elapsed_sec=0.0,
                    detail=f"Failed to parse XML: {exc}",
                )
            )
            summary["errors"] += 1
            continue

        for suite in collect_suite_nodes(root):
            summary["tests"] += parse_int(suite.attrib.get("tests"))
            summary["failures"] += parse_int(suite.attrib.get("failures"))
            summary["errors"] += parse_int(suite.attrib.get("errors"))
            summary["skipped"] += parse_int(suite.attrib.get("skipped"))
            summary["time"] += parse_float(suite.attrib.get("time"))

            suite_name = suite.attrib.get("name") or xml_path.name
            for case in suite.findall("testcase"):
                status = "passed"
                detail = ""
                for child in list(case):
                    if child.tag in ("failure", "error", "skipped"):
                        status = child.tag
                        detail = squash_spaces(child.attrib.get("message") or child.text or "")
                        break

                rows.append(
                    TestCaseRow(
                        suite=suite_name,
                        classname=case.attrib.get("classname", ""),
                        name=case.attrib.get("name", ""),
                        status=status,
                        elapsed_sec=parse_float(case.attrib.get("time")),
                        detail=detail,
                    )
                )

    if summary["tests"] == 0 and rows:
        summary["tests"] = len(rows)
        summary["failures"] = sum(1 for row in rows if row.status == "failure")
        summary["errors"] = sum(1 for row in rows if row.status == "error")
        summary["skipped"] = sum(1 for row in rows if row.status == "skipped")
        summary["time"] = sum(row.elapsed_sec for row in rows)

    return summary, rows


def render_html(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
    status_class_map = {
        "passed": "badge-pass",
        "failure": "badge-fail",
        "error": "badge-error",
        "skipped": "badge-skip",
    }
    generated_at = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")

    html_parts = [
        "<!doctype html>",
        "<html lang='en'>",
        "<head>",
        "<meta charset='utf-8'>",
        "<meta name='viewport' content='width=device-width, initial-scale=1'>",
        "<title>SFEPS Test Report</title>",
        "<style>",
        "body { margin: 0; padding: 24px; background: linear-gradient(180deg, #f5f7fb 0%, #edf2ff 100%); font-family: 'Segoe UI', Tahoma, sans-serif; color: #1f2937; }",
        ".panel { max-width: 1400px; margin: 0 auto; background: #ffffff; border-radius: 16px; box-shadow: 0 12px 36px rgba(31, 41, 55, 0.12); overflow: hidden; }",
        ".hero { padding: 24px 28px; background: linear-gradient(135deg, #0f172a 0%, #1e3a8a 60%, #2563eb 100%); color: #f8fafc; }",
        ".hero h1 { margin: 0 0 8px 0; font-size: 24px; letter-spacing: 0.2px; }",
        ".hero p { margin: 0; opacity: 0.9; font-size: 13px; }",
        ".summary { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; padding: 16px 20px 8px 20px; }",
        ".kpi { background: #f8fafc; border: 1px solid #e5e7eb; border-radius: 10px; padding: 10px 12px; }",
        ".kpi .label { display: block; color: #6b7280; font-size: 12px; }",
        ".kpi .value { display: block; margin-top: 4px; font-size: 20px; font-weight: 700; }",
        ".toolbar { display: grid; grid-template-columns: minmax(240px, 1.4fr) minmax(130px, 0.6fr) minmax(170px, 0.7fr) auto; gap: 10px; align-items: center; padding: 10px 20px 4px 20px; }",
        ".control { height: 38px; border: 1px solid #cbd5e1; border-radius: 10px; padding: 0 12px; font-size: 14px; color: #0f172a; background: #ffffff; outline: none; }",
        ".control:focus { border-color: #2563eb; box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.15); }",
        ".result-count { justify-self: end; font-size: 13px; color: #475569; font-weight: 600; }",
        ".table-wrap { padding: 12px 20px 24px 20px; overflow-x: auto; }",
        "table { width: 100%; border-collapse: separate; border-spacing: 0; table-layout: auto; min-width: 1100px; }",
        "col.suite { width: 16%; }",
        "col.classname { width: 16%; }",
        "col.case { width: 26%; }",
        "col.status { width: 8%; }",
        "col.time { width: 8%; }",
        "col.detail { width: 26%; }",
        "thead th { position: sticky; top: 0; z-index: 1; background: #eff6ff; color: #1e3a8a; border-bottom: 2px solid #bfdbfe; font-size: 12px; text-transform: uppercase; letter-spacing: 0.03em; }",
        "th, td { border: 1px solid #e5e7eb; padding: 12px 10px; text-align: left; vertical-align: top; font-size: 13px; }",
        "tbody tr:nth-child(even) { background: #f9fafb; }",
        ".cell { white-space: pre-wrap; word-break: break-word; overflow-wrap: anywhere; line-height: 1.45; }",
        ".status-col { text-align: center; }",
        ".badge { display: inline-block; padding: 3px 10px; border-radius: 999px; font-size: 12px; font-weight: 700; letter-spacing: 0.02em; line-height: 1.4; border: 1px solid transparent; }",
        ".badge-pass { color: #14532d; background: #dcfce7; border-color: #86efac; }",
        ".badge-fail { color: #7f1d1d; background: #fee2e2; border-color: #fca5a5; }",
        ".badge-error { color: #7f1d1d; background: #fecaca; border-color: #f87171; }",
        ".badge-skip { color: #78350f; background: #fef3c7; border-color: #fcd34d; }",
        ".empty { padding: 20px; border: 1px dashed #cbd5e1; border-radius: 12px; background: #f8fafc; color: #475569; }",
        "</style>",
        "</head>",
        "<body>",
        "<div class='panel'>",
        "<div class='hero'>",
        "<h1>SFEPS Automated Test Report</h1>",
        f"<p>Generated at {html.escape(generated_at)}</p>",
        "</div>",
        "<div class='summary'>",
        f"<div class='kpi'><span class='label'>Tests</span><span class='value'>{int(summary['tests'])}</span></div>",
        f"<div class='kpi'><span class='label'>Failures</span><span class='value'>{int(summary['failures'])}</span></div>",
        f"<div class='kpi'><span class='label'>Errors</span><span class='value'>{int(summary['errors'])}</span></div>",
        f"<div class='kpi'><span class='label'>Skipped</span><span class='value'>{int(summary['skipped'])}</span></div>",
        f"<div class='kpi'><span class='label'>Duration</span><span class='value'>{summary['time']:.2f}s</span></div>",
        "</div>",
        "<div class='toolbar'>",
        "<input id='searchInput' class='control' type='search' placeholder='Search suite, class, case, detail...'>",
        "<select id='statusFilter' class='control'>"
        "<option value='all'>All Status</option>"
        "<option value='failure'>Failure</option>"
        "<option value='error'>Error</option>"
        "<option value='skipped'>Skipped</option>"
        "<option value='passed'>Passed</option>"
        "</select>",
        "<select id='sortSelect' class='control'>"
        "<option value='status'>Sort: Status</option>"
        "<option value='time-desc'>Sort: Time desc</option>"
        "<option value='time-asc'>Sort: Time asc</option>"
        "<option value='suite'>Sort: Suite</option>"
        "<option value='name'>Sort: Name</option>"
        "</select>",
        "<div id='resultCount' class='result-count'></div>",
        "</div>",
        "<div class='table-wrap'>",
    ]

    if not rows:
        html_parts.append("<div class='empty'>No testcases found in reports/*.xml</div>")
    else:
        html_parts.extend(
            [
                "<table>",
                "<colgroup><col class='suite'><col class='classname'><col class='case'><col class='status'><col class='time'><col class='detail'></colgroup>",
                "<thead><tr><th>Suite</th><th>Class</th><th>Test Case</th><th>Status</th><th>Time (s)</th><th>Detail</th></tr></thead>",
                "<tbody id='casesBody'>",
            ]
        )
        for row in rows:
            css = status_class_map.get(row.status, "")
            status_label = row.status.upper()
            badge = f"<span class='badge {css}'>{html.escape(status_label)}</span>"
            suite_safe = html.escape(row.suite, quote=True)
            class_safe = html.escape(row.classname, quote=True)
            name_safe = html.escape(row.name, quote=True)
            detail_safe = html.escape(row.detail, quote=True)
            status_safe = html.escape(row.status, quote=True)
            search_blob = html.escape(
                f"{row.suite} {row.classname} {row.name} {row.detail} {row.status}".lower(),
                quote=True,
            )
            html_parts.append(
                f"<tr data-status='{status_safe}' data-time='{row.elapsed_sec:.6f}' "
                f"data-suite='{suite_safe.lower()}' data-name='{name_safe.lower()}' "
                f"data-search='{search_blob}'>"
                f"<td><div class='cell'>{suite_safe}</div></td>"
                f"<td><div class='cell'>{class_safe}</div></td>"
                f"<td><div class='cell'>{name_safe}</div></td>"
                f"<td class='status-col'>{badge}</td>"
                f"<td><div class='cell'>{row.elapsed_sec:.3f}</div></td>"
                f"<td><div class='cell'>{detail_safe}</div></td>"
                "</tr>"
            )
        html_parts.extend(["</tbody>", "</table>"])

    html_parts.extend(
        [
            "</div>",
            "</div>",
            "<script>",
            "(function () {",
            "  const tbody = document.getElementById('casesBody');",
            "  const searchInput = document.getElementById('searchInput');",
            "  const statusFilter = document.getElementById('statusFilter');",
            "  const sortSelect = document.getElementById('sortSelect');",
            "  const resultCount = document.getElementById('resultCount');",
            "  if (!tbody || !searchInput || !statusFilter || !sortSelect || !resultCount) return;",
            "  const allRows = Array.from(tbody.querySelectorAll('tr'));",
            "  const statusRank = { failure: 0, error: 1, skipped: 2, passed: 3 };",
            "  const sortRows = (rows, sortValue) => {",
            "    if (sortValue === 'time-desc') return rows.sort((a, b) => parseFloat(b.dataset.time) - parseFloat(a.dataset.time));",
            "    if (sortValue === 'time-asc') return rows.sort((a, b) => parseFloat(a.dataset.time) - parseFloat(b.dataset.time));",
            "    if (sortValue === 'suite') return rows.sort((a, b) => a.dataset.suite.localeCompare(b.dataset.suite));",
            "    if (sortValue === 'name') return rows.sort((a, b) => a.dataset.name.localeCompare(b.dataset.name));",
            "    return rows.sort((a, b) => {",
            "      const ra = statusRank[a.dataset.status] ?? 99;",
            "      const rb = statusRank[b.dataset.status] ?? 99;",
            "      if (ra !== rb) return ra - rb;",
            "      return parseFloat(b.dataset.time) - parseFloat(a.dataset.time);",
            "    });",
            "  };",
            "  const applyFilters = () => {",
            "    const q = searchInput.value.trim().toLowerCase();",
            "    const status = statusFilter.value;",
            "    const sortValue = sortSelect.value;",
            "    const filtered = allRows.filter((row) => {",
            "      const byStatus = (status === 'all') || (row.dataset.status === status);",
            "      const byText = (!q) || row.dataset.search.includes(q);",
            "      return byStatus && byText;",
            "    });",
            "    const sorted = sortRows(filtered, sortValue);",
            "    tbody.innerHTML = '';",
            "    sorted.forEach((row) => tbody.appendChild(row));",
            "    resultCount.textContent = `${sorted.length} / ${allRows.length} cases`;",
            "  };",
            "  searchInput.addEventListener('input', applyFilters);",
            "  statusFilter.addEventListener('change', applyFilters);",
            "  sortSelect.addEventListener('change', applyFilters);",
            "  applyFilters();",
            "})();",
            "</script>",
            "</body>",
            "</html>",
        ]
    )
    output_path.write_text("\n".join(html_parts), encoding="utf-8")


def excel_cell(value: str, value_type: str = "String", style: str | None = None) -> str:
    style_attr = f' ss:StyleID="{style}"' if style else ""
    return f'<Cell{style_attr}><Data ss:Type="{value_type}">{xml_escape(value)}</Data></Cell>'


def render_excel_xls(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
    row_count = max(2, len(rows) + 1)
    lines: list[str] = []
    lines.append('<?xml version="1.0"?>')
    lines.append('<?mso-application progid="Excel.Sheet"?>')
    lines.append(
        '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" '
        'xmlns:o="urn:schemas-microsoft-com:office:office" '
        'xmlns:x="urn:schemas-microsoft-com:office:excel" '
        'xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">'
    )
    lines.append("<Styles>")
    lines.append('<Style ss:ID="Default" ss:Name="Normal"><Alignment ss:Vertical="Top"/><Font ss:FontName="Calibri" ss:Size="10"/></Style>')
    lines.append('<Style ss:ID="Header"><Font ss:Bold="1"/><Interior ss:Color="#DCEBFF" ss:Pattern="Solid"/></Style>')
    lines.append('<Style ss:ID="Wrap"><Alignment ss:Vertical="Top" ss:WrapText="1"/></Style>')
    lines.append('<Style ss:ID="Pass"><Font ss:Color="#166534" ss:Bold="1"/></Style>')
    lines.append('<Style ss:ID="Fail"><Font ss:Color="#991B1B" ss:Bold="1"/></Style>')
    lines.append('<Style ss:ID="Error"><Font ss:Color="#B91C1C" ss:Bold="1"/></Style>')
    lines.append('<Style ss:ID="Skip"><Font ss:Color="#92400E" ss:Bold="1"/></Style>')
    lines.append("</Styles>")

    lines.append('<Worksheet ss:Name="Summary"><Table>')
    lines.append("<Row>")
    lines.append(excel_cell("Metric", style="Header"))
    lines.append(excel_cell("Value", style="Header"))
    lines.append("</Row>")
    lines.append("<Row>" + excel_cell("Tests") + excel_cell(str(int(summary["tests"])), value_type="Number") + "</Row>")
    lines.append("<Row>" + excel_cell("Failures") + excel_cell(str(int(summary["failures"])), value_type="Number") + "</Row>")
    lines.append("<Row>" + excel_cell("Errors") + excel_cell(str(int(summary["errors"])), value_type="Number") + "</Row>")
    lines.append("<Row>" + excel_cell("Skipped") + excel_cell(str(int(summary["skipped"])), value_type="Number") + "</Row>")
    lines.append("<Row>" + excel_cell("Duration(sec)") + excel_cell(f"{summary['time']:.3f}", value_type="Number") + "</Row>")
    lines.append("</Table>")
    lines.append('<WorksheetOptions xmlns="urn:schemas-microsoft-com:office:excel"></WorksheetOptions>')
    lines.append("</Worksheet>")

    lines.append('<Worksheet ss:Name="TestCases"><Table>')
    for width in ("180", "220", "340", "95", "95", "540"):
        lines.append(f'<Column ss:Width="{width}"/>')
    lines.append("<Row>")
    lines.append(excel_cell("Suite", style="Header"))
    lines.append(excel_cell("Class", style="Header"))
    lines.append(excel_cell("Test Case", style="Header"))
    lines.append(excel_cell("Status", style="Header"))
    lines.append(excel_cell("Time (s)", style="Header"))
    lines.append(excel_cell("Detail", style="Header"))
    lines.append("</Row>")

    status_style = {
        "passed": "Pass",
        "failure": "Fail",
        "error": "Error",
        "skipped": "Skip",
    }
    for row in rows:
        lines.append("<Row>")
        lines.append(excel_cell(row.suite, style="Wrap"))
        lines.append(excel_cell(row.classname, style="Wrap"))
        lines.append(excel_cell(row.name, style="Wrap"))
        lines.append(excel_cell(row.status, style=status_style.get(row.status)))
        lines.append(excel_cell(f"{row.elapsed_sec:.3f}", value_type="Number"))
        lines.append(excel_cell(row.detail, style="Wrap"))
        lines.append("</Row>")

    lines.append("</Table>")
    lines.append(
        f'<AutoFilter x:Range="R1C1:R{row_count}C6" xmlns="urn:schemas-microsoft-com:office:excel"></AutoFilter>'
    )
    lines.append(
        '<WorksheetOptions xmlns="urn:schemas-microsoft-com:office:excel">'
        "<FreezePanes/>"
        "<FrozenNoSplit/>"
        "<SplitHorizontal>1</SplitHorizontal>"
        "<TopRowBottomPane>1</TopRowBottomPane>"
        "<ActivePane>2</ActivePane>"
        "<Panes><Pane><Number>2</Number><ActiveRow>1</ActiveRow></Pane></Panes>"
        "</WorksheetOptions>"
    )
    lines.append("</Worksheet>")
    lines.append("</Workbook>")
    output_path.write_text("\n".join(lines), encoding="utf-8")


def xlsx_col_name(index_1_based: int) -> str:
    value = index_1_based
    name = ""
    while value > 0:
        value, rem = divmod(value - 1, 26)
        name = chr(65 + rem) + name
    return name


def xlsx_string_cell(col: int, row: int, value: str, style: int | None = None) -> str:
    ref = f"{xlsx_col_name(col)}{row}"
    style_attr = f' s="{style}"' if style is not None else ""
    return f'<c r="{ref}" t="inlineStr"{style_attr}><is><t>{xml_escape(value)}</t></is></c>'


def xlsx_number_cell(col: int, row: int, value: float | int, style: int | None = None) -> str:
    ref = f"{xlsx_col_name(col)}{row}"
    style_attr = f' s="{style}"' if style is not None else ""
    return f'<c r="{ref}"{style_attr}><v>{value}</v></c>'


def build_xlsx_styles_xml() -> str:
    return """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <fonts count="6">
    <font><sz val="11"/><name val="Calibri"/><family val="2"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><family val="2"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><color rgb="FF166534"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><color rgb="FF991B1B"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><color rgb="FFB91C1C"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><color rgb="FF92400E"/></font>
  </fonts>
  <fills count="3">
    <fill><patternFill patternType="none"/></fill>
    <fill><patternFill patternType="gray125"/></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFDCEBFF"/><bgColor indexed="64"/></patternFill></fill>
  </fills>
  <borders count="1">
    <border><left/><right/><top/><bottom/><diagonal/></border>
  </borders>
  <cellStyleXfs count="1">
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0"/>
  </cellStyleXfs>
  <cellXfs count="7">
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
    <xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1">
      <alignment horizontal="center" vertical="center" wrapText="1"/>
    </xf>
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1">
      <alignment vertical="top" wrapText="1"/>
    </xf>
    <xf numFmtId="0" fontId="2" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1">
      <alignment vertical="top"/>
    </xf>
    <xf numFmtId="0" fontId="3" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1">
      <alignment vertical="top"/>
    </xf>
    <xf numFmtId="0" fontId="4" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1">
      <alignment vertical="top"/>
    </xf>
    <xf numFmtId="0" fontId="5" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1">
      <alignment vertical="top"/>
    </xf>
  </cellXfs>
  <cellStyles count="1">
    <cellStyle name="Normal" xfId="0" builtinId="0"/>
  </cellStyles>
  <dxfs count="4">
    <dxf>
      <font><color rgb="FF7F1D1D"/></font>
      <fill><patternFill patternType="solid"><fgColor rgb="FFFEE2E2"/><bgColor indexed="64"/></patternFill></fill>
    </dxf>
    <dxf>
      <font><color rgb="FF7F1D1D"/></font>
      <fill><patternFill patternType="solid"><fgColor rgb="FFFECACA"/><bgColor indexed="64"/></patternFill></fill>
    </dxf>
    <dxf>
      <font><color rgb="FF78350F"/></font>
      <fill><patternFill patternType="solid"><fgColor rgb="FFFEF3C7"/><bgColor indexed="64"/></patternFill></fill>
    </dxf>
    <dxf>
      <font><color rgb="FF14532D"/></font>
      <fill><patternFill patternType="solid"><fgColor rgb="FFDCFCE7"/><bgColor indexed="64"/></patternFill></fill>
    </dxf>
  </dxfs>
</styleSheet>
"""


def build_xlsx_summary_sheet_xml(summary: dict[str, float]) -> str:
    rows: list[str] = []
    rows.append(
        "<row r=\"1\">"
        + xlsx_string_cell(1, 1, "Metric", style=1)
        + xlsx_string_cell(2, 1, "Value", style=1)
        + "</row>"
    )

    metrics = [
        ("Tests", int(summary["tests"])),
        ("Failures", int(summary["failures"])),
        ("Errors", int(summary["errors"])),
        ("Skipped", int(summary["skipped"])),
        ("Duration(sec)", round(summary["time"], 3)),
    ]
    for idx, (name, value) in enumerate(metrics, start=2):
        rows.append(
            f"<row r=\"{idx}\">"
            + xlsx_string_cell(1, idx, name, style=2)
            + xlsx_number_cell(2, idx, value)
            + "</row>"
        )

    sheet_data = "".join(rows)
    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
        "<cols>"
        "<col min=\"1\" max=\"1\" width=\"26\" customWidth=\"1\"/>"
        "<col min=\"2\" max=\"2\" width=\"14\" customWidth=\"1\"/>"
        "</cols>"
        f"<sheetData>{sheet_data}</sheetData>"
        "</worksheet>"
    )


def build_xlsx_testcases_sheet_xml(rows: list[TestCaseRow]) -> str:
    row_count = max(2, len(rows) + 1)
    status_style_map = {
        "passed": 3,
        "failure": 4,
        "error": 5,
        "skipped": 6,
    }
    xml_rows: list[str] = []
    xml_rows.append(
        "<row r=\"1\">"
        + xlsx_string_cell(1, 1, "Suite", style=1)
        + xlsx_string_cell(2, 1, "Class", style=1)
        + xlsx_string_cell(3, 1, "Test Case", style=1)
        + xlsx_string_cell(4, 1, "Status", style=1)
        + xlsx_string_cell(5, 1, "Time (s)", style=1)
        + xlsx_string_cell(6, 1, "Detail", style=1)
        + "</row>"
    )

    for row_idx, row in enumerate(rows, start=2):
        status_style = status_style_map.get(row.status, 2)
        xml_rows.append(
            f"<row r=\"{row_idx}\">"
            + xlsx_string_cell(1, row_idx, row.suite, style=2)
            + xlsx_string_cell(2, row_idx, row.classname, style=2)
            + xlsx_string_cell(3, row_idx, row.name, style=2)
            + xlsx_string_cell(4, row_idx, row.status, style=status_style)
            + xlsx_number_cell(5, row_idx, round(row.elapsed_sec, 3))
            + xlsx_string_cell(6, row_idx, row.detail, style=2)
            + "</row>"
        )

    sheet_data = "".join(xml_rows)
    conditional_formatting = ""
    if len(rows) > 0:
        status_range = f"D2:D{len(rows) + 1}"
        conditional_formatting = (
            f'<conditionalFormatting sqref="{status_range}">'
            '<cfRule type="containsText" dxfId="0" priority="1" operator="containsText" text="failure">'
            '<formula>NOT(ISERROR(SEARCH("failure",LOWER(D2))))</formula>'
            "</cfRule>"
            '<cfRule type="containsText" dxfId="1" priority="2" operator="containsText" text="error">'
            '<formula>NOT(ISERROR(SEARCH("error",LOWER(D2))))</formula>'
            "</cfRule>"
            '<cfRule type="containsText" dxfId="2" priority="3" operator="containsText" text="skipped">'
            '<formula>NOT(ISERROR(SEARCH("skipped",LOWER(D2))))</formula>'
            "</cfRule>"
            '<cfRule type="containsText" dxfId="3" priority="4" operator="containsText" text="passed">'
            '<formula>NOT(ISERROR(SEARCH("passed",LOWER(D2))))</formula>'
            "</cfRule>"
            "</conditionalFormatting>"
        )

    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetViews><sheetView workbookViewId=\"0\">"
        "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
        "</sheetView></sheetViews>"
        "<cols>"
        "<col min=\"1\" max=\"1\" width=\"26\" customWidth=\"1\"/>"
        "<col min=\"2\" max=\"2\" width=\"32\" customWidth=\"1\"/>"
        "<col min=\"3\" max=\"3\" width=\"48\" customWidth=\"1\"/>"
        "<col min=\"4\" max=\"4\" width=\"14\" customWidth=\"1\"/>"
        "<col min=\"5\" max=\"5\" width=\"12\" customWidth=\"1\"/>"
        "<col min=\"6\" max=\"6\" width=\"80\" customWidth=\"1\"/>"
        "</cols>"
        f"<sheetData>{sheet_data}</sheetData>"
        f"<autoFilter ref=\"A1:F{row_count}\"/>"
        f"{conditional_formatting}"
        "</worksheet>"
    )


def render_excel_xlsx(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
    content_types = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
</Types>
"""
    rels_root = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>
"""
    workbook_xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <bookViews><workbookView xWindow="0" yWindow="0" windowWidth="18000" windowHeight="9000"/></bookViews>
  <sheets>
    <sheet name="Summary" sheetId="1" r:id="rId1"/>
    <sheet name="TestCases" sheetId="2" r:id="rId2"/>
  </sheets>
</workbook>
"""
    workbook_rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>
"""

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", content_types)
        zf.writestr("_rels/.rels", rels_root)
        zf.writestr("xl/workbook.xml", workbook_xml)
        zf.writestr("xl/_rels/workbook.xml.rels", workbook_rels)
        zf.writestr("xl/styles.xml", build_xlsx_styles_xml())
        zf.writestr("xl/worksheets/sheet1.xml", build_xlsx_summary_sheet_xml(summary))
        zf.writestr("xl/worksheets/sheet2.xml", build_xlsx_testcases_sheet_xml(rows))


def pdf_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def wrap_pdf_text(text: str, width: int) -> list[str]:
    text = squash_spaces(text)
    if not text:
        return [""]
    return textwrap.wrap(text, width=width, break_long_words=True, break_on_hyphens=False)


def build_pdf_row_lines(values: list[str], widths: list[int]) -> list[str]:
    wrapped_columns = [wrap_pdf_text(value, width) for value, width in zip(values, widths)]
    height = max(len(column) for column in wrapped_columns)
    lines: list[str] = []
    for line_idx in range(height):
        cells = []
        for col_idx, width in enumerate(widths):
            chunk = wrapped_columns[col_idx][line_idx] if line_idx < len(wrapped_columns[col_idx]) else ""
            cells.append(chunk.ljust(width))
        lines.append(" | ".join(cells))
    return lines


def build_pdf_lines(summary: dict[str, float], rows: list[TestCaseRow]) -> list[str]:
    col_titles = ["SUITE", "CLASS", "TEST CASE", "STATUS", "TIME", "DETAIL"]
    col_widths = [12, 12, 20, 8, 8, 28]
    separator = "-+-".join("-" * width for width in col_widths)

    lines = []
    lines.append("SFEPS Automated Test Report")
    lines.append("")
    lines.append(
        "Tests={tests} Failures={failures} Errors={errors} Skipped={skipped} Duration={time:.2f}s".format(
            tests=int(summary["tests"]),
            failures=int(summary["failures"]),
            errors=int(summary["errors"]),
            skipped=int(summary["skipped"]),
            time=summary["time"],
        )
    )
    lines.append("")
    lines.append(" | ".join(title.ljust(width) for title, width in zip(col_titles, col_widths)))
    lines.append(separator)

    if not rows:
        lines.append("No testcases found in reports/*.xml")
        return lines

    for row in rows:
        values = [
            row.suite,
            row.classname,
            row.name,
            row.status,
            f"{row.elapsed_sec:.3f}",
            row.detail,
        ]
        lines.extend(build_pdf_row_lines(values, col_widths))
        lines.append(separator)
    return lines


def render_pdf_plaintext_fallback(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
    lines = build_pdf_lines(summary, rows)
    max_lines_per_page = 58
    pages = [lines[i : i + max_lines_per_page] for i in range(0, len(lines), max_lines_per_page)] or [[]]

    objects: list[bytes] = []

    def add_obj(obj: bytes) -> int:
        objects.append(obj)
        return len(objects)

    font_id = add_obj(b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>")
    pages_id = add_obj(b"<<>>")
    page_ids: list[int] = []

    for page_lines in pages:
        commands = ["BT", "/F1 9 Tf", "32 810 Td"]
        for idx, line in enumerate(page_lines):
            if idx > 0:
                commands.append("0 -13 Td")
            commands.append(f"({pdf_escape(line)}) Tj")
        commands.append("ET")
        stream = "\n".join(commands).encode("latin-1", errors="replace")
        content = (
            b"<< /Length "
            + str(len(stream)).encode("ascii")
            + b" >>\nstream\n"
            + stream
            + b"\nendstream"
        )
        content_id = add_obj(content)
        page = (
            f"<< /Type /Page /Parent {pages_id} 0 R "
            f"/MediaBox [0 0 595 842] "
            f"/Resources << /Font << /F1 {font_id} 0 R >> >> "
            f"/Contents {content_id} 0 R >>"
        ).encode("ascii")
        page_ids.append(add_obj(page))

    kids = " ".join(f"{pid} 0 R" for pid in page_ids)
    objects[pages_id - 1] = (
        f"<< /Type /Pages /Kids [{kids}] /Count {len(page_ids)} >>"
    ).encode("ascii")
    catalog_id = add_obj(f"<< /Type /Catalog /Pages {pages_id} 0 R >>".encode("ascii"))

    pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for idx, obj in enumerate(objects, start=1):
        offsets.append(len(pdf))
        pdf.extend(f"{idx} 0 obj\n".encode("ascii"))
        pdf.extend(obj)
        pdf.extend(b"\nendobj\n")

    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    pdf.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root {catalog_id} 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode(
            "ascii"
        )
    )

    output_path.write_bytes(bytes(pdf))


def render_pdf_from_html(html_path: Path, output_path: Path) -> str:
    html_uri = html_path.resolve().as_uri()
    out_pdf = str(output_path.resolve())
    html_file = str(html_path.resolve())
    renderer_cmds: list[tuple[str, list[str]]] = []

    wkhtml = shutil.which("wkhtmltopdf")
    if wkhtml:
        renderer_cmds.append(
            (
                "wkhtmltopdf",
                [
                    wkhtml,
                    "--quiet",
                    "--enable-local-file-access",
                    "--encoding",
                    "utf-8",
                    "--page-size",
                    "A4",
                    "--margin-top",
                    "10mm",
                    "--margin-right",
                    "8mm",
                    "--margin-bottom",
                    "10mm",
                    "--margin-left",
                    "8mm",
                    html_file,
                    out_pdf,
                ],
            )
        )

    for name in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable"):
        binary = shutil.which(name)
        if binary:
            renderer_cmds.append(
                (
                    name,
                    [
                        binary,
                        "--headless",
                        "--disable-gpu",
                        "--no-sandbox",
                        "--allow-file-access-from-files",
                        "--print-to-pdf-no-header",
                        f"--print-to-pdf={out_pdf}",
                        html_uri,
                    ],
                )
            )

    for renderer_name, cmd in renderer_cmds:
        try:
            subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=150)
            if output_path.exists() and output_path.stat().st_size > 0:
                return renderer_name
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, FileNotFoundError):
            continue

    return ""


def render_pdf(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path, html_path: Path) -> None:
    renderer = render_pdf_from_html(html_path, output_path)
    if renderer:
        print(f"PDF renderer: {renderer}")
        return

    render_pdf_plaintext_fallback(summary, rows, output_path)
    print("PDF renderer: internal-fallback")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate HTML/Excel/PDF reports from JUnit XML.")
    parser.add_argument("--input", default="reports", help="Directory containing junit xml files")
    parser.add_argument("--output", default="reports", help="Directory to write report artifacts")
    args = parser.parse_args()

    input_dir = Path(args.input)
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    xml_paths = sorted(input_dir.glob("*.xml"))
    summary, rows = parse_junit_reports(xml_paths)

    html_path = output_dir / "test-report.html"
    xls_path = output_dir / "test-report.xls"
    xlsx_path = output_dir / "test-report.xlsx"
    pdf_path = output_dir / "test-report.pdf"

    render_html(summary, rows, html_path)
    render_excel_xls(summary, rows, xls_path)
    render_excel_xlsx(summary, rows, xlsx_path)
    render_pdf(summary, rows, pdf_path, html_path)

    print(f"Generated: {html_path}")
    print(f"Generated: {xls_path}")
    print(f"Generated: {xlsx_path}")
    print(f"Generated: {pdf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
