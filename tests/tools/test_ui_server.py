#!/usr/bin/env python3
from __future__ import annotations

import base64
import json
import os
import ssl
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


NF_PARAM_DEFAULTS: dict[str, str] = {
    # Jenkinsfile.nf parameters default values
    "PERF_EVENT_TARGET_COUNT": "50",
    "PERF_EVENT_WINDOW_SECONDS": "60",
    "PERF_EVENT_INTERVAL_SECONDS": "0",
    "PERF_EVENT_MAX_MISSING": "0",
    "PERF_EVENT_MAX_DUPLICATES": "0",
    "RELI_STREAM_DURATION_SECONDS": "3600",
    "RELI_STREAM_POLL_INTERVAL_SECONDS": "1",
    "RELI_STREAM_RECOVERY_TIMEOUT_SECONDS": "10",
}

DEFAULT_CONFIG = {
    "SFEPS_TEST_UI_HOST": "0.0.0.0",
    "SFEPS_TEST_UI_PORT": "8787",
    "SFEPS_JENKINS_URL": "http://192.168.0.88:8080",
    # Jenkinsfile (CI) / Jenkinsfile.nf (Non-Functional) job names
    "SFEPS_JENKINS_JOB_CI": "SFEPS",
    "SFEPS_JENKINS_JOB_NF": "SFEPS-NF",
    # Backward compatibility: if only this is set, use it for both pipelines.
    "SFEPS_JENKINS_JOB": "",
    "SFEPS_JENKINS_USER": "admin",
    "SFEPS_JENKINS_TOKEN": "11efd4ee8170b52577dd84d151cb52ef1c",
    "SFEPS_JENKINS_VERIFY_SSL": "1",
}


HTML_PAGE = """<!doctype html>
<html lang=\"ko\">
<head>
  <meta charset=\"utf-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
  <title>SFEPS Test UI</title>
  <style>
    :root {
      --bg-1: #07131f;
      --bg-2: #102235;
      --panel: rgba(14, 27, 44, 0.82);
      --panel-border: #315178;
      --text: #e9f1ff;
      --muted: #a3bad8;
      --ok: #4ade80;
      --warn: #facc15;
      --bad: #fb7185;
      --btn-1: #0891b2;
      --btn-2: #0e7490;
      --btn-sub: #334155;
      --focus: #38bdf8;
      --link: #7dd3fc;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--text);
      font-family: \"SUIT Variable\", \"Pretendard Variable\", \"Noto Sans KR\", sans-serif;
      background:
        radial-gradient(1200px 420px at -15% -20%, #1e3a56 0%, transparent 70%),
        radial-gradient(900px 360px at 115% -10%, #194767 0%, transparent 65%),
        linear-gradient(145deg, var(--bg-1), var(--bg-2));
      min-height: 100vh;
    }
    .wrap {
      max-width: 1120px;
      margin: 26px auto;
      padding: 0 14px 26px;
    }
    .hero {
      padding: 18px 20px;
      border: 1px solid #35597f;
      border-radius: 18px;
      background: linear-gradient(130deg, rgba(19, 41, 66, 0.88), rgba(13, 31, 50, 0.78));
      box-shadow: 0 18px 38px rgba(1, 10, 20, 0.34);
      animation: rise .36s ease-out;
    }
    .title {
      margin: 0 0 6px;
      font-size: clamp(24px, 4.6vw, 34px);
      line-height: 1.2;
      letter-spacing: .2px;
    }
    .sub {
      margin: 0;
      color: var(--muted);
      line-height: 1.45;
      font-size: 14px;
    }
    .grid {
      margin-top: 14px;
      display: grid;
      gap: 14px;
      grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
      animation: rise .5s ease-out;
    }
    .card {
      border: 1px solid var(--panel-border);
      border-radius: 16px;
      padding: 14px;
      background: var(--panel);
      backdrop-filter: blur(6px);
    }
    .label {
      display: block;
      font-size: 12px;
      font-weight: 600;
      letter-spacing: .15px;
      color: var(--muted);
      margin-bottom: 6px;
    }
    select, input[type=text] {
      width: 100%;
      height: 40px;
      color: var(--text);
      border: 1px solid #44658f;
      border-radius: 10px;
      background: rgba(10, 21, 37, 0.88);
      padding: 0 10px;
      outline: none;
      transition: border-color .18s ease, box-shadow .18s ease;
    }
    select:focus, input[type=text]:focus {
      border-color: var(--focus);
      box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.22);
    }
    .hint {
      margin-top: 8px;
      font-size: 12px;
      line-height: 1.45;
      color: var(--muted);
    }
    .hidden { display: none; }
    .actions {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      margin-top: 12px;
    }
    button {
      border: 0;
      border-radius: 10px;
      padding: 10px 15px;
      color: #fff;
      background: linear-gradient(180deg, var(--btn-1), var(--btn-2));
      cursor: pointer;
      font-weight: 700;
      letter-spacing: .1px;
      transition: transform .12s ease, filter .12s ease;
    }
    button:hover { transform: translateY(-1px); filter: brightness(1.07); }
    button:active { transform: translateY(0); }
    button.secondary { background: var(--btn-sub); }
    .status {
      margin-top: 12px;
      border: 1px solid #37597f;
      background: rgba(9, 18, 31, 0.84);
      border-radius: 12px;
      padding: 12px;
      line-height: 1.5;
      white-space: pre-wrap;
      min-height: 124px;
      color: #d7e4f9;
    }
    .status-ok { border-color: #1f8c57; }
    .status-warn { border-color: #978025; }
    .status-bad { border-color: #a53a4e; }
    .mono { font-family: \"JetBrains Mono\", \"Cascadia Mono\", Menlo, Consolas, monospace; }
    .links {
      margin-top: 12px;
      display: grid;
      gap: 8px;
      grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
    }
    .link-item {
      border: 1px solid #3c628b;
      border-radius: 10px;
      padding: 10px 11px;
      background: rgba(10, 24, 40, 0.88);
      animation: rise .24s ease-out;
    }
    .link-title {
      margin: 0 0 4px;
      font-size: 12px;
      color: var(--muted);
    }
    .link-item a {
      color: var(--link);
      text-decoration: none;
      word-break: break-all;
      font-size: 13px;
    }
    .link-item a:hover { text-decoration: underline; }
    .small-muted {
      font-size: 12px;
      color: var(--muted);
    }
    @keyframes rise {
      from { opacity: 0; transform: translateY(8px); }
      to { opacity: 1; transform: translateY(0); }
    }
  </style>
</head>
<body>
  <div class=\"wrap\">
    <section class=\"hero\">
      <h1 class=\"title\">SFEPS Test Tool</h1>
      <p class=\"sub\">Jenkins Functional/Non-Functional 테스트를 실행하고, 마지막 빌드 결과와 리포트 링크를 바로 확인할 수 있습니다.</p>
    </section>

    <section class=\"grid\">
      <div class=\"card\">
        <label class=\"label\" for=\"pipeline\">Pipeline</label>
        <select id=\"pipeline\">
          <option value=\"ci\" selected>Functional Test (Jenkinsfile)</option>
          <option value=\"nf\">Non-Functional Test (Jenkinsfile.nf)</option>
        </select>
        <div class=\"hint\" id=\"pipelineHint\">Functional Test는 Jenkinsfile 기준으로 파라미터 없이 실행됩니다.</div>
      </div>
    </section>

    <section class=\"card hidden\" style=\"margin-top:14px\" id=\"nfCard\">
      <label class=\"label\">NF Parameters (Jenkinsfile.nf defaults)</label>
      <div class=\"grid\" style=\"margin-top: 10px;\">
        <div>
          <label class=\"label\" for=\"PERF_EVENT_TARGET_COUNT\">PERF_EVENT_TARGET_COUNT</label>
          <input id=\"PERF_EVENT_TARGET_COUNT\" type=\"text\" value=\"50\" />
        </div>
        <div>
          <label class=\"label\" for=\"PERF_EVENT_WINDOW_SECONDS\">PERF_EVENT_WINDOW_SECONDS</label>
          <input id=\"PERF_EVENT_WINDOW_SECONDS\" type=\"text\" value=\"60\" />
        </div>
        <div>
          <label class=\"label\" for=\"PERF_EVENT_INTERVAL_SECONDS\">PERF_EVENT_INTERVAL_SECONDS</label>
          <input id=\"PERF_EVENT_INTERVAL_SECONDS\" type=\"text\" value=\"0\" />
        </div>
        <div>
          <label class=\"label\" for=\"PERF_EVENT_MAX_MISSING\">PERF_EVENT_MAX_MISSING</label>
          <input id=\"PERF_EVENT_MAX_MISSING\" type=\"text\" value=\"0\" />
        </div>
        <div>
          <label class=\"label\" for=\"PERF_EVENT_MAX_DUPLICATES\">PERF_EVENT_MAX_DUPLICATES</label>
          <input id=\"PERF_EVENT_MAX_DUPLICATES\" type=\"text\" value=\"0\" />
        </div>
        <div>
          <label class=\"label\" for=\"RELI_STREAM_DURATION_SECONDS\">RELI_STREAM_DURATION_SECONDS</label>
          <input id=\"RELI_STREAM_DURATION_SECONDS\" type=\"text\" value=\"3600\" />
        </div>
        <div>
          <label class=\"label\" for=\"RELI_STREAM_POLL_INTERVAL_SECONDS\">RELI_STREAM_POLL_INTERVAL_SECONDS</label>
          <input id=\"RELI_STREAM_POLL_INTERVAL_SECONDS\" type=\"text\" value=\"1\" />
        </div>
        <div>
          <label class=\"label\" for=\"RELI_STREAM_RECOVERY_TIMEOUT_SECONDS\">RELI_STREAM_RECOVERY_TIMEOUT_SECONDS</label>
          <input id=\"RELI_STREAM_RECOVERY_TIMEOUT_SECONDS\" type=\"text\" value=\"10\" />
        </div>
      </div>
    </section>

    <section class=\"card\" style=\"margin-top:14px\">
      <div class=\"actions\">
        <button id=\"runBtn\">Run Jenkins Job</button>
        <button id=\"lastBtn\" class=\"secondary\">Refresh Last Build</button>
      </div>
      <div id=\"status\" class=\"status mono\">Ready.</div>
      <div style=\"margin-top: 10px;\" class=\"small-muted\">Last Build Report Links</div>
      <div id=\"linkList\" class=\"links\">
        <div class=\"small-muted\">Refresh Last Build를 누르면 링크가 표시됩니다.</div>
      </div>
    </section>
  </div>

  <script>
    var statusEl = document.getElementById('status');
    var linkListEl = document.getElementById('linkList');
    var pipelineEl = document.getElementById('pipeline');
    var nfCard = document.getElementById('nfCard');
    var pipelineHint = document.getElementById('pipelineHint');

    var nfKeys = [
      'PERF_EVENT_TARGET_COUNT',
      'PERF_EVENT_WINDOW_SECONDS',
      'PERF_EVENT_INTERVAL_SECONDS',
      'PERF_EVENT_MAX_MISSING',
      'PERF_EVENT_MAX_DUPLICATES',
      'RELI_STREAM_DURATION_SECONDS',
      'RELI_STREAM_POLL_INTERVAL_SECONDS',
      'RELI_STREAM_RECOVERY_TIMEOUT_SECONDS'
    ];

    function setStatus(text, kind) {
      statusEl.textContent = text;
      statusEl.className = 'status mono';
      if (kind) {
        statusEl.classList.add('status-' + kind);
      }
    }

    function clearLinks(message) {
      while (linkListEl.firstChild) {
        linkListEl.removeChild(linkListEl.firstChild);
      }
      var info = document.createElement('div');
      info.className = 'small-muted';
      info.textContent = message || 'No links.';
      linkListEl.appendChild(info);
    }

    function renderLinks(links) {
      if (!Array.isArray(links) || links.length === 0) {
        clearLinks('리포트 링크를 찾지 못했습니다. Jenkins artifact/archive 설정을 확인하세요.');
        return;
      }
      while (linkListEl.firstChild) {
        linkListEl.removeChild(linkListEl.firstChild);
      }
      for (var i = 0; i < links.length; i += 1) {
        var link = links[i];
        if (!link || !link.url) continue;
        var card = document.createElement('div');
        card.className = 'link-item';
        var title = document.createElement('p');
        title.className = 'link-title';
        title.textContent = link.name || ('link-' + (i + 1));
        var anchor = document.createElement('a');
        anchor.href = link.url;
        anchor.target = '_blank';
        anchor.rel = 'noopener noreferrer';
        anchor.textContent = link.url;
        card.appendChild(title);
        card.appendChild(anchor);
        linkListEl.appendChild(card);
      }
      if (!linkListEl.firstChild) {
        clearLinks('표시할 링크가 없습니다.');
      }
    }

    function pipelineLabel(value) {
      return value === 'nf'
        ? 'Non-Functional Test (Jenkinsfile.nf)'
        : 'Functional Test (Jenkinsfile)';
    }

    function syncPipelineUi() {
      var pipeline = pipelineEl.value;
      if (pipeline === 'nf') {
        nfCard.classList.remove('hidden');
        pipelineHint.textContent = 'Non-Functional Test는 Jenkinsfile.nf 파라미터를 함께 전송합니다.';
      } else {
        nfCard.classList.add('hidden');
        pipelineHint.textContent = 'Functional Test는 Jenkinsfile 기준으로 파라미터 없이 실행됩니다.';
      }
    }

    async function callJson(url, opts) {
      var options = opts || {};
      options.headers = Object.assign({'Content-Type': 'application/json'}, options.headers || {});
      var res = await fetch(url, options);
      var txt = await res.text();
      var data;
      try { data = JSON.parse(txt); } catch (e) { data = {raw: txt}; }
      if (!res.ok) {
        throw new Error(data.error || data.raw || ('HTTP ' + res.status));
      }
      return data;
    }

    function collectPayload() {
      var payload = {
        pipeline: pipelineEl.value,
        nf_params: {}
      };

      if (payload.pipeline === 'nf') {
        for (var i = 0; i < nfKeys.length; i += 1) {
          var key = nfKeys[i];
          payload.nf_params[key] = document.getElementById(key).value.trim();
        }
      }
      return payload;
    }

    async function runBuild() {
      var payload = collectPayload();
      setStatus('Submitting build request...');
      clearLinks('빌드가 큐에 등록되었습니다. 완료 후 Refresh Last Build로 최신 리포트를 가져오세요.');
      try {
        var data = await callJson('/api/jenkins/build', {
          method: 'POST',
          body: JSON.stringify(payload)
        });

        var lines = [
          'Triggered.',
          'pipeline: ' + pipelineLabel(data.pipeline),
          'job: ' + data.job,
          'queue_url: ' + (data.queue_url || '-'),
          'job_url: ' + (data.job_url || '-')
        ];

        if (data.parameters && Object.keys(data.parameters).length > 0) {
          lines.push('parameters: ' + JSON.stringify(data.parameters));
        } else {
          lines.push('parameters: (none)');
        }

        setStatus(lines.join('\\n'), 'ok');
      } catch (err) {
        setStatus('Build trigger failed:\\n' + err.message, 'bad');
      }
    }

    async function refreshLastBuild() {
      setStatus('Loading last build...');
      try {
        var pipeline = pipelineEl.value;
        var data = await callJson('/api/jenkins/last-build?pipeline=' + encodeURIComponent(pipeline));
        var result = data.result || 'RUNNING';
        var kind = 'warn';
        if (result === 'SUCCESS') kind = 'ok';
        if (result === 'FAILURE' || result === 'ABORTED' || result === 'UNSTABLE') kind = 'bad';

        var lines = [
          'pipeline: ' + pipelineLabel(pipeline),
          'job: ' + data.job,
          'build_number: ' + data.number,
          'result: ' + result,
          'building: ' + data.building,
          'url: ' + data.url
        ];
        setStatus(lines.join('\\n'), kind);
        renderLinks(data.report_links || []);
      } catch (err) {
        setStatus('Last build load failed:\\n' + err.message, 'bad');
        clearLinks('빌드 정보를 불러오지 못했습니다.');
      }
    }

    pipelineEl.addEventListener('change', syncPipelineUi);
    document.getElementById('runBtn').addEventListener('click', runBuild);
    document.getElementById('lastBtn').addEventListener('click', refreshLastBuild);
    syncPipelineUi();
  </script>
</body>
</html>
"""


@dataclass
class AppConfig:
    host: str
    port: int
    jenkins_url: str
    jenkins_job_ci: str
    jenkins_job_nf: str
    jenkins_user: str
    jenkins_token: str
    verify_ssl: bool


def load_config() -> AppConfig:
    host = os.environ.get("SFEPS_TEST_UI_HOST", DEFAULT_CONFIG["SFEPS_TEST_UI_HOST"])
    port = int(os.environ.get("SFEPS_TEST_UI_PORT", DEFAULT_CONFIG["SFEPS_TEST_UI_PORT"]))
    jenkins_url = os.environ.get("SFEPS_JENKINS_URL", DEFAULT_CONFIG["SFEPS_JENKINS_URL"]).rstrip("/")

    legacy_job = os.environ.get("SFEPS_JENKINS_JOB", DEFAULT_CONFIG["SFEPS_JENKINS_JOB"]).strip("/")
    jenkins_job_ci = os.environ.get("SFEPS_JENKINS_JOB_CI", legacy_job or DEFAULT_CONFIG["SFEPS_JENKINS_JOB_CI"]).strip("/")
    jenkins_job_nf = os.environ.get("SFEPS_JENKINS_JOB_NF", legacy_job or DEFAULT_CONFIG["SFEPS_JENKINS_JOB_NF"]).strip("/")

    jenkins_user = os.environ.get("SFEPS_JENKINS_USER", DEFAULT_CONFIG["SFEPS_JENKINS_USER"])
    jenkins_token = os.environ.get("SFEPS_JENKINS_TOKEN", DEFAULT_CONFIG["SFEPS_JENKINS_TOKEN"])
    verify_ssl = os.environ.get("SFEPS_JENKINS_VERIFY_SSL", DEFAULT_CONFIG["SFEPS_JENKINS_VERIFY_SSL"]) != "0"

    return AppConfig(host, port, jenkins_url, jenkins_job_ci, jenkins_job_nf, jenkins_user, jenkins_token, verify_ssl)


def normalize_pipeline(value: Any) -> str:
    pipeline = str(value or "ci").strip().lower()
    return "nf" if pipeline == "nf" else "ci"


def make_ssl_context(verify_ssl: bool) -> ssl.SSLContext | None:
    if verify_ssl:
        return None
    return ssl._create_unverified_context()


def jenkins_job_path(job_name: str) -> str:
    parts = [p for p in job_name.strip("/").split("/") if p]
    return "/".join("job/" + urllib.parse.quote(p) for p in parts)


def build_auth_header(user: str, token: str) -> dict[str, str]:
    if not user or not token:
        return {}
    raw = (user + ":" + token).encode("utf-8")
    return {"Authorization": "Basic " + base64.b64encode(raw).decode("ascii")}


def http_json(
    method: str,
    url: str,
    *,
    headers: dict[str, str] | None = None,
    body: bytes | None = None,
    verify_ssl: bool = True,
) -> tuple[int, dict[str, Any], dict[str, str]]:
    req = urllib.request.Request(url=url, method=method, data=body, headers=headers or {})
    ctx = make_ssl_context(verify_ssl)
    with urllib.request.urlopen(req, context=ctx, timeout=20) as resp:
        status = resp.getcode()
        raw = resp.read().decode("utf-8", errors="replace")
        resp_headers = {k: v for k, v in resp.headers.items()}

    try:
        data = json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        data = {"raw": raw}
    return status, data, resp_headers


def fetch_crumb(config: AppConfig) -> dict[str, str]:
    if not config.jenkins_url:
        return {}

    crumb_url = config.jenkins_url + "/crumbIssuer/api/json"
    headers = {"Accept": "application/json"}
    headers.update(build_auth_header(config.jenkins_user, config.jenkins_token))

    try:
        _, data, _ = http_json("GET", crumb_url, headers=headers, verify_ssl=config.verify_ssl)
        crumb_field = str(data.get("crumbRequestField", "")).strip()
        crumb_value = str(data.get("crumb", "")).strip()
        if crumb_field and crumb_value:
            return {crumb_field: crumb_value}
    except Exception:
        return {}
    return {}


def resolve_job_name(config: AppConfig, pipeline: str) -> str:
    return config.jenkins_job_nf if pipeline == "nf" else config.jenkins_job_ci


def build_nf_params(payload: dict[str, Any]) -> dict[str, str]:
    raw_params = payload.get("nf_params") if isinstance(payload, dict) else None
    source: dict[str, Any] = raw_params if isinstance(raw_params, dict) else {}

    params: dict[str, str] = {}
    for key, default_value in NF_PARAM_DEFAULTS.items():
        value = source.get(key, default_value)
        text = str(value).strip()
        params[key] = text if text else default_value
    return params


def build_url(base_url: str, suffix: str) -> str:
    return base_url.rstrip("/") + "/" + suffix.lstrip("/")


def extract_report_links(build_data: dict[str, Any]) -> list[dict[str, str]]:
    build_url_text = str(build_data.get("url") or "").strip()
    links: list[dict[str, str]] = []
    seen: set[str] = set()

    def add_link(name: str, url: str) -> None:
        clean_name = str(name or "").strip()
        clean_url = str(url or "").strip()
        if not clean_name or not clean_url or clean_url in seen:
            return
        seen.add(clean_url)
        links.append({"name": clean_name, "url": clean_url})

    if build_url_text:
        add_link("Build", build_url_text)
        add_link("Console", build_url(build_url_text, "console"))

    has_junit = False
    actions = build_data.get("actions")
    if isinstance(actions, list):
        for action in actions:
            if not isinstance(action, dict):
                continue
            class_name = str(action.get("_class") or "")
            if "TestResultAction" in class_name:
                has_junit = True
                break

    if has_junit and build_url_text:
        add_link("JUnit Test Report", build_url(build_url_text, "testReport/"))

    artifacts = build_data.get("artifacts")
    if isinstance(artifacts, list) and build_url_text:
        for artifact in artifacts:
            if not isinstance(artifact, dict):
                continue
            relative_path = str(artifact.get("relativePath") or "").strip()
            if not relative_path:
                continue
            encoded_path = urllib.parse.quote(relative_path, safe="/")
            file_name = str(artifact.get("fileName") or artifact.get("displayPath") or relative_path)
            add_link("Artifact: " + file_name, build_url(build_url_text, "artifact/" + encoded_path))

    return links


def trigger_jenkins_build(config: AppConfig, payload: dict[str, Any]) -> dict[str, Any]:
    if not config.jenkins_url:
        raise ValueError("SFEPS_JENKINS_URL must be configured")

    pipeline = normalize_pipeline(payload.get("pipeline"))
    job_name = resolve_job_name(config, pipeline)
    if not job_name:
        raise ValueError("Jenkins job name is empty. Check SFEPS_JENKINS_JOB_CI / SFEPS_JENKINS_JOB_NF")

    job_path = jenkins_job_path(job_name)
    params: dict[str, str] = build_nf_params(payload) if pipeline == "nf" else {}

    if params:
        query = urllib.parse.urlencode(params)
        build_endpoint = "/buildWithParameters?" + query
    else:
        build_endpoint = "/build"
    build_trigger_url = config.jenkins_url + "/" + job_path + build_endpoint

    headers: dict[str, str] = {"Accept": "application/json"}
    headers.update(build_auth_header(config.jenkins_user, config.jenkins_token))
    headers.update(fetch_crumb(config))

    req = urllib.request.Request(url=build_trigger_url, method="POST", headers=headers)
    ctx = make_ssl_context(config.verify_ssl)
    with urllib.request.urlopen(req, context=ctx, timeout=20) as resp:
        code = resp.getcode()
        response_headers = {k: v for k, v in resp.headers.items()}

    queue_url = response_headers.get("Location", "")
    if code not in (HTTPStatus.CREATED, HTTPStatus.FOUND, HTTPStatus.OK):
        raise RuntimeError("Unexpected Jenkins response code: " + str(code))

    return {
        "ok": True,
        "status": int(code),
        "pipeline": pipeline,
        "job": job_name,
        "queue_url": queue_url,
        "job_url": config.jenkins_url + "/" + job_path,
        "parameters": params,
    }


def get_last_build(config: AppConfig, pipeline: str) -> dict[str, Any]:
    if not config.jenkins_url:
        raise ValueError("SFEPS_JENKINS_URL must be configured")

    job_name = resolve_job_name(config, pipeline)
    if not job_name:
        raise ValueError("Jenkins job name is empty. Check SFEPS_JENKINS_JOB_CI / SFEPS_JENKINS_JOB_NF")

    job_path = jenkins_job_path(job_name)
    tree = "number,result,building,url,actions[_class],artifacts[fileName,relativePath,displayPath]"
    url = config.jenkins_url + "/" + job_path + "/lastBuild/api/json?tree=" + urllib.parse.quote(tree, safe=",[]_")

    headers = {"Accept": "application/json"}
    headers.update(build_auth_header(config.jenkins_user, config.jenkins_token))
    _, data, _ = http_json("GET", url, headers=headers, verify_ssl=config.verify_ssl)

    return {
        "pipeline": pipeline,
        "job": job_name,
        "number": data.get("number"),
        "result": data.get("result"),
        "building": data.get("building", False),
        "url": data.get("url"),
        "report_links": extract_report_links(data),
    }


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _send_html(self, html: str) -> None:
        raw = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _read_json_body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            data = json.loads(body.decode("utf-8"))
            if isinstance(data, dict):
                return data
        except json.JSONDecodeError:
            pass
        return {}

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            self._send_html(HTML_PAGE)
            return

        if path == "/api/health":
            cfg = self.server.app_config  # type: ignore[attr-defined]
            self._send_json(
                200,
                {
                    "ok": True,
                    "jenkins_configured": bool(cfg.jenkins_url and cfg.jenkins_job_ci and cfg.jenkins_job_nf),
                    "jenkins_url": cfg.jenkins_url,
                    "jenkins_job_ci": cfg.jenkins_job_ci,
                    "jenkins_job_nf": cfg.jenkins_job_nf,
                    "nf_defaults": NF_PARAM_DEFAULTS,
                },
            )
            return

        if path == "/api/jenkins/last-build":
            cfg = self.server.app_config  # type: ignore[attr-defined]
            pipeline = normalize_pipeline((query.get("pipeline") or ["ci"])[0])
            try:
                data = get_last_build(cfg, pipeline)
                self._send_json(200, data)
            except urllib.error.HTTPError as exc:
                self._send_json(502, {"error": "Jenkins HTTPError %s: %s" % (exc.code, exc.reason)})
            except Exception as exc:
                self._send_json(500, {"error": str(exc)})
            return

        self._send_json(404, {"error": "Not found"})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path != "/api/jenkins/build":
            self._send_json(404, {"error": "Not found"})
            return

        cfg = self.server.app_config  # type: ignore[attr-defined]
        payload = self._read_json_body()

        try:
            data = trigger_jenkins_build(cfg, payload)
            self._send_json(200, data)
        except urllib.error.HTTPError as exc:
            self._send_json(502, {"error": "Jenkins HTTPError %s: %s" % (exc.code, exc.reason)})
        except Exception as exc:
            self._send_json(500, {"error": str(exc)})

    def log_message(self, fmt: str, *args: Any) -> None:
        print("[test-ui] " + (fmt % args))


def main() -> int:
    cfg = load_config()
    server = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    server.app_config = cfg  # type: ignore[attr-defined]

    print("SFEPS Test UI server started")
    browser_host = "127.0.0.1" if cfg.host == "0.0.0.0" else cfg.host
    print("- open_in_browser: http://%s:%s" % (browser_host, cfg.port))
    print("- bind_host: %s" % cfg.host)
    print("- jenkins_url: %s" % (cfg.jenkins_url or "(not set)"))
    print("- jenkins_job_ci: %s" % (cfg.jenkins_job_ci or "(not set)"))
    print("- jenkins_job_nf: %s" % (cfg.jenkins_job_nf or "(not set)"))

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
