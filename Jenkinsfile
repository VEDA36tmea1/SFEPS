pipeline {
    agent any

    options {
        timestamps()
        disableConcurrentBuilds()
        skipDefaultCheckout(true)
    }

    environment {
        // Override these in Jenkins job/global env if needed.
        SFEPS_DB_USER = "${env.SFEPS_DB_USER ?: 'pi'}"
        SFEPS_DB_PASS = "${env.SFEPS_DB_PASS ?: 'raspberry'}"
        SFEPS_DB_NAME_ANALYTICS = "${env.SFEPS_DB_NAME_ANALYTICS ?: 'CCgbd'}"
        SFEPS_ESP_TCP_ENABLE = "${env.SFEPS_ESP_TCP_ENABLE ?: '0'}"
        SFEPS_ESP_TCP_BIND_IP = "${env.SFEPS_ESP_TCP_BIND_IP ?: '127.0.0.1'}"
    }

    stages {
        stage('Checkout') {
            steps {
                checkout scm
            }
        }

        stage('Build Server') {
            steps {
                sh '''
                    set -eu
                    cmake -S server -B server/build
                    cmake --build server/build -j"$(nproc)"
                '''
            }
        }

        stage('Check Test Tooling') {
            steps {
                sh '''
                    set -eu
                    python3 -m pytest --version
                '''
            }
        }

        stage('Start Local MariaDB') {
            steps {
                sh '''
                    set -eu

                    DB_DIR="$WORKSPACE/.ci-mariadb"
                    DB_SOCKET="$DB_DIR/mysqld.sock"
                    DB_PID="$DB_DIR/mysqld.pid"
                    DB_LOG="$DB_DIR/mysqld.log"

                    rm -rf "$DB_DIR"
                    mkdir -p "$DB_DIR"

                    mariadb-install-db --datadir="$DB_DIR" --auth-root-authentication-method=normal --skip-test-db >/dev/null

                    mariadbd \
                      --datadir="$DB_DIR" \
                      --socket="$DB_SOCKET" \
                      --pid-file="$DB_PID" \
                      --log-error="$DB_LOG" \
                      --skip-networking \
                      --user=jenkins &

                    for _ in $(seq 1 60); do
                      if [ -S "$DB_SOCKET" ]; then
                        break
                      fi
                      sleep 1
                    done

                    test -S "$DB_SOCKET"

                    mariadb --protocol=SOCKET --socket="$DB_SOCKET" -uroot <<'SQL'
CREATE DATABASE IF NOT EXISTS CCgbd;
CREATE USER IF NOT EXISTS 'pi'@'localhost' IDENTIFIED BY 'raspberry';
GRANT ALL PRIVILEGES ON CCgbd.* TO 'pi'@'localhost';
FLUSH PRIVILEGES;

USE CCgbd;
CREATE TABLE IF NOT EXISTS users (
  id varchar(50) NOT NULL PRIMARY KEY,
  password varchar(50) NOT NULL,
  name varchar(20) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS login_logs (
  id int(11) NOT NULL AUTO_INCREMENT PRIMARY KEY,
  username varchar(50) DEFAULT NULL,
  ip_address varchar(50) DEFAULT NULL,
  status varchar(20) DEFAULT NULL,
  created_at timestamp NOT NULL DEFAULT current_timestamp()
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS recordings (
  id int(11) NOT NULL AUTO_INCREMENT PRIMARY KEY,
  filename varchar(255) NOT NULL,
  created_at timestamp NOT NULL DEFAULT current_timestamp()
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS analytics_logs (
  id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
  object_id varchar(128) NOT NULL,
  card_age_text varchar(32) NOT NULL DEFAULT '',
  age varchar(32) NOT NULL DEFAULT '',
  is_fraud tinyint(1) NOT NULL DEFAULT 0,
  frame_time varchar(32) DEFAULT NULL,
  object_type varchar(64) DEFAULT NULL,
  estimated_age int(11) DEFAULT 0,
  photo_path varchar(255) DEFAULT '',
  x double DEFAULT 0,
  y double DEFAULT 0,
  event varchar(128) DEFAULT '',
  created_at timestamp NOT NULL DEFAULT current_timestamp()
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

INSERT INTO users (id, password, name)
VALUES ('admin', '1111', 'Admin')
ON DUPLICATE KEY UPDATE
  password = VALUES(password),
  name = VALUES(name);
SQL
                '''
            }
        }

        stage('Run Login Tests') {
            steps {
                sh '''
                    set -eu
                    export MYSQL_UNIX_PORT="$WORKSPACE/.ci-mariadb/mysqld.sock"
                    mkdir -p reports
                    python3 -m pytest -q tests/test_tc_func_login.py -r a --junitxml=reports/login-tests.xml

                    python3 - <<'PY'
import sys
import xml.etree.ElementTree as ET

path = "reports/login-tests.xml"
root = ET.parse(path).getroot()

if root.tag == "testsuite":
    tests = int(root.attrib.get("tests", "0"))
    skipped = int(root.attrib.get("skipped", "0"))
else:
    tests = 0
    skipped = 0
    for suite in root.findall("testsuite"):
        tests += int(suite.attrib.get("tests", "0"))
        skipped += int(suite.attrib.get("skipped", "0"))

if tests == 0 or skipped == tests:
    print(f"All tests skipped ({skipped}/{tests}). Marking build as failed.")
    sys.exit(2)
PY
                '''
            }
        }

        stage('Start Local MediaMTX') {
            steps {
                sh '''
                    set -eu

                    export SFEPS_MTX_BIN="${SFEPS_MTX_BIN:-/usr/local/bin/mediamtx}"
                    export SFEPS_MTX_CONFIG="${SFEPS_MTX_CONFIG:-/etc/mediamtx/mediamtx.yml}"
                    export SFEPS_STREAM_RTSP_URL="${SFEPS_STREAM_RTSP_URL:-rtsp://127.0.0.1:8554/cam1}"
                    MTX_SCRIPT="${SFEPS_CI_MTX_SCRIPT:-/usr/local/bin/run_mediamtx_ci.sh}"
                    MTX_PID_FILE="$WORKSPACE/.ci-mediamtx.pid"
                    MTX_LOG="$WORKSPACE/.ci-mediamtx.log"

                    if [ ! -x "$MTX_SCRIPT" ]; then
                      echo "mediamtx start script is missing or not executable: $MTX_SCRIPT" >&2
                      exit 1
                    fi

                    # Keep a single local mediamtx process per build.
                    pkill -f '/usr/local/bin/mediamtx' 2>/dev/null || true

                    nohup env SFEPS_MTX_BIN="$SFEPS_MTX_BIN" SFEPS_MTX_CONFIG="$SFEPS_MTX_CONFIG" bash "$MTX_SCRIPT" >"$MTX_LOG" 2>&1 &
                    echo "$!" > "$MTX_PID_FILE"

                    python3 - <<'PY'
import socket
import time

def rtsp_port_ready(timeout=2.0):
    try:
        with socket.create_connection(("127.0.0.1", 8554), timeout=timeout) as s:
            return True
    except OSError:
        return False

deadline = time.time() + 60
while time.time() < deadline:
    if rtsp_port_ready():
        print("mediamtx TCP is ready on 127.0.0.1:8554")
        break
    time.sleep(1)
else:
    raise SystemExit("mediamtx did not open 127.0.0.1:8554 in time")
PY
                '''
            }
        }

        stage('Start RTSP Publisher') {
            steps {
                sh '''
                    set -eu

                    export SFEPS_STREAM_RTSP_URL="${SFEPS_STREAM_RTSP_URL:-rtsp://127.0.0.1:8554/cam1}"
                    FFMPEG_PID_FILE="$WORKSPACE/.ci-ffmpeg-publisher.pid"
                    FFMPEG_LOG="$WORKSPACE/.ci-ffmpeg-publisher.log"

                    if ! command -v ffmpeg >/dev/null 2>&1; then
                      echo "ffmpeg is required for stream publishing but not found in PATH" >&2
                      exit 1
                    fi

                    # Keep a single local publisher process per build.
                    pkill -f "ffmpeg.*${SFEPS_STREAM_RTSP_URL}" 2>/dev/null || true

                    nohup env SFEPS_STREAM_RTSP_URL="$SFEPS_STREAM_RTSP_URL" bash -c '
                      set +e
                      while true; do
                        ffmpeg -hide_banner -loglevel warning -re \
                          -f lavfi -i testsrc=size=640x360:rate=15 \
                          -an \
                          -c:v mpeg4 -pix_fmt yuv420p -g 30 \
                          -f rtsp -rtsp_transport tcp "$SFEPS_STREAM_RTSP_URL"
                        sleep 1
                      done
                    ' >"$FFMPEG_LOG" 2>&1 &
                    echo "$!" > "$FFMPEG_PID_FILE"

                    python3 - <<'PY'
import socket
import time

def rtsp_describe_ok(timeout=2.0):
    try:
        with socket.create_connection(("127.0.0.1", 8554), timeout=timeout) as s:
            s.settimeout(timeout)
            req = (
                "DESCRIBE rtsp://127.0.0.1:8554/cam1 RTSP/1.0\\r\\n"
                "CSeq: 1\\r\\n"
                "Accept: application/sdp\\r\\n"
                "User-Agent: jenkins-ci\\r\\n\\r\\n"
            )
            s.sendall(req.encode("utf-8"))
            data = s.recv(4096).decode("latin1", "replace")
            return ("RTSP/1.0 200" in data) and ("m=video" in data)
    except OSError:
        return False

deadline = time.time() + 60
last_error = None
while time.time() < deadline:
    if rtsp_describe_ok():
        print("RTSP publisher ready: DESCRIBE 200 + m=video on rtsp://127.0.0.1:8554/cam1")
        break
    last_error = "DESCRIBE not ready"
    time.sleep(1)
else:
    raise SystemExit(f"RTSP publisher did not become ready in time: {last_error}")
PY
                '''
            }
        }

        stage('Run Stream Tests') {
            steps {
                sh '''
                    set -eu
                    export MYSQL_UNIX_PORT="$WORKSPACE/.ci-mariadb/mysqld.sock"
                    export SFEPS_MTX_BIN="${SFEPS_MTX_BIN:-/usr/local/bin/mediamtx}"
                    export SFEPS_MTX_CONFIG="${SFEPS_MTX_CONFIG:-/etc/mediamtx/mediamtx.yml}"
                    export SFEPS_CI_MTX_SCRIPT="${SFEPS_CI_MTX_SCRIPT:-/usr/local/bin/run_mediamtx_ci.sh}"
                    export SFEPS_STREAM_RTSP_URL="${SFEPS_STREAM_RTSP_URL:-rtsp://127.0.0.1:8554/cam1}"
                    export SFEPS_STREAM_FAULT_DOWN_CMD="pkill -f '^/usr/local/bin/mediamtx( |$)' || true"
                    export SFEPS_STREAM_FAULT_UP_CMD="nohup env SFEPS_MTX_BIN=${SFEPS_MTX_BIN} SFEPS_MTX_CONFIG=${SFEPS_MTX_CONFIG} bash ${SFEPS_CI_MTX_SCRIPT} >${WORKSPACE}/.ci-mediamtx.log 2>&1 &"
                    mkdir -p reports
                    python3 -m pytest -q tests/test_tc_func_stream.py -r a --junitxml=reports/stream-tests.xml
                '''
            }
        }

        stage('Run Event Tests') {
            steps {
                sh '''
                    set -eu
                    export MYSQL_UNIX_PORT="$WORKSPACE/.ci-mariadb/mysqld.sock"
                    mkdir -p reports
                    python3 -m pytest -q tests/test_tc_func_event.py -r a --junitxml=reports/event-tests.xml

                    python3 - <<'PY'
import sys
import xml.etree.ElementTree as ET

path = "reports/event-tests.xml"
root = ET.parse(path).getroot()

if root.tag == "testsuite":
    tests = int(root.attrib.get("tests", "0"))
    skipped = int(root.attrib.get("skipped", "0"))
else:
    tests = 0
    skipped = 0
    for suite in root.findall("testsuite"):
        tests += int(suite.attrib.get("tests", "0"))
        skipped += int(suite.attrib.get("skipped", "0"))

if tests == 0 or skipped == tests:
    print(f"All tests skipped ({skipped}/{tests}). Marking build as failed.")
    sys.exit(2)
PY
                '''
            }
        }
    }

    post {
        always {
            sh '''
                DB_PID="$WORKSPACE/.ci-mariadb/mysqld.pid"
                if [ -f "$DB_PID" ]; then
                  kill "$(cat "$DB_PID")" 2>/dev/null || true
                fi

                MTX_PID_FILE="$WORKSPACE/.ci-mediamtx.pid"
                if [ -f "$MTX_PID_FILE" ]; then
                  kill "$(cat "$MTX_PID_FILE")" 2>/dev/null || true
                fi
                pkill -f '/usr/local/bin/mediamtx' 2>/dev/null || true

                FFMPEG_PID_FILE="$WORKSPACE/.ci-ffmpeg-publisher.pid"
                if [ -f "$FFMPEG_PID_FILE" ]; then
                  kill "$(cat "$FFMPEG_PID_FILE")" 2>/dev/null || true
                fi
                pkill -f 'ffmpeg.*rtsp://127.0.0.1:8554/cam1' 2>/dev/null || true
            '''
            sh '''
                set +e
                mkdir -p reports
                python3 scripts/generate_test_reports.py --input reports --output reports
                rc=$?
                if [ "$rc" -ne 0 ]; then
                  echo "test report generation failed (non-fatal), exit=$rc"
                fi
                exit 0
            '''
            junit testResults: 'reports/*.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'reports/*.xml,reports/test-report.html,reports/test-report.pdf,reports/test-report.xls,reports/test-report.xlsx,tests/real_server.log,.ci-mediamtx.log,.ci-ffmpeg-publisher.log', allowEmptyArchive: true
        }
    }
}
