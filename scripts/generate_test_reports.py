#!/usr/bin/env python3
from __future__ import annotations

import argparse
import html
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
        "passed": "status-pass",
        "failure": "status-fail",
        "error": "status-error",
        "skipped": "status-skip",
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
        ".table-wrap { padding: 12px 20px 24px 20px; overflow-x: auto; }",
        "table { width: 100%; border-collapse: collapse; table-layout: fixed; min-width: 1200px; }",
        "col.suite { width: 16%; }",
        "col.classname { width: 16%; }",
        "col.case { width: 26%; }",
        "col.status { width: 8%; }",
        "col.time { width: 8%; }",
        "col.detail { width: 26%; }",
        "thead th { position: sticky; top: 0; z-index: 1; background: #eff6ff; color: #1e3a8a; border-bottom: 2px solid #bfdbfe; font-size: 12px; text-transform: uppercase; letter-spacing: 0.03em; }",
        "th, td { border: 1px solid #e5e7eb; padding: 10px; text-align: left; vertical-align: top; font-size: 13px; }",
        "tbody tr:nth-child(even) { background: #f9fafb; }",
        ".cell { white-space: normal; word-break: break-word; overflow-wrap: anywhere; line-height: 1.35; }",
        ".status-pass { color: #166534; font-weight: 700; }",
        ".status-fail { color: #991b1b; font-weight: 700; }",
        ".status-error { color: #b91c1c; font-weight: 700; }",
        ".status-skip { color: #92400e; font-weight: 700; }",
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
                "<tbody>",
            ]
        )
        for row in rows:
            css = status_class_map.get(row.status, "")
            html_parts.append(
                "<tr>"
                f"<td><div class='cell'>{html.escape(row.suite)}</div></td>"
                f"<td><div class='cell'>{html.escape(row.classname)}</div></td>"
                f"<td><div class='cell'>{html.escape(row.name)}</div></td>"
                f"<td><div class='cell {css}'>{html.escape(row.status)}</div></td>"
                f"<td><div class='cell'>{row.elapsed_sec:.3f}</div></td>"
                f"<td><div class='cell'>{html.escape(row.detail)}</div></td>"
                "</tr>"
            )
        html_parts.extend(["</tbody>", "</table>"])

    html_parts.extend(["</div>", "</div>", "</body>", "</html>"])
    output_path.write_text("\n".join(html_parts), encoding="utf-8")


def excel_cell(value: str, value_type: str = "String", style: str | None = None) -> str:
    style_attr = f' ss:StyleID="{style}"' if style else ""
    return f'<Cell{style_attr}><Data ss:Type="{value_type}">{xml_escape(value)}</Data></Cell>'


def render_excel_xls(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
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
    lines.append("</Table></Worksheet>")

    lines.append('<Worksheet ss:Name="TestCases"><Table>')
    for width in ("120", "140", "220", "75", "70", "320"):
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

    lines.append("</Table></Worksheet>")
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
    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
        "<cols>"
        "<col min=\"1\" max=\"1\" width=\"20\" customWidth=\"1\"/>"
        "<col min=\"2\" max=\"2\" width=\"24\" customWidth=\"1\"/>"
        "<col min=\"3\" max=\"3\" width=\"34\" customWidth=\"1\"/>"
        "<col min=\"4\" max=\"4\" width=\"12\" customWidth=\"1\"/>"
        "<col min=\"5\" max=\"5\" width=\"12\" customWidth=\"1\"/>"
        "<col min=\"6\" max=\"6\" width=\"50\" customWidth=\"1\"/>"
        "</cols>"
        f"<sheetData>{sheet_data}</sheetData>"
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


def fit_text(text: str, width: int) -> str:
    text = squash_spaces(text)
    if len(text) <= width:
        return text.ljust(width)
    if width <= 1:
        return text[:width]
    return (text[: width - 1] + "~")


def build_pdf_lines(summary: dict[str, float], rows: list[TestCaseRow]) -> list[str]:
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
    header = "SUITE      CLASS      TEST CASE          STATUS   TIME   DETAIL"
    lines.append(header)
    lines.append("-" * len(header))

    if not rows:
        lines.append("No testcases found in reports/*.xml")
        return lines

    for row in rows:
        detail_chunks = textwrap.wrap(squash_spaces(row.detail), width=24) or [""]
        first_line = (
            f"{fit_text(row.suite, 10)} "
            f"{fit_text(row.classname, 10)} "
            f"{fit_text(row.name, 18)} "
            f"{fit_text(row.status, 8)} "
            f"{fit_text(f'{row.elapsed_sec:.3f}', 6)} "
            f"{fit_text(detail_chunks[0], 24)}"
        )
        lines.append(first_line)
        for chunk in detail_chunks[1:3]:
            lines.append(
                f"{' ' * 10} {' ' * 10} {' ' * 18} {' ' * 8} {' ' * 6} {fit_text(chunk, 24)}"
            )
    return lines


def render_pdf(summary: dict[str, float], rows: list[TestCaseRow], output_path: Path) -> None:
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
    render_pdf(summary, rows, pdf_path)

    print(f"Generated: {html_path}")
    print(f"Generated: {xls_path}")
    print(f"Generated: {xlsx_path}")
    print(f"Generated: {pdf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
