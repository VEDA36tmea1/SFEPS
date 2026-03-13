#!/usr/bin/env python3
from __future__ import annotations

import argparse
import html
import textwrap
import xml.etree.ElementTree as ET
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
    pdf_path = output_dir / "test-report.pdf"

    render_html(summary, rows, html_path)
    render_excel_xls(summary, rows, xls_path)
    render_pdf(summary, rows, pdf_path)

    print(f"Generated: {html_path}")
    print(f"Generated: {xls_path}")
    print(f"Generated: {pdf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
