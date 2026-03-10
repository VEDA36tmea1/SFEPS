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
  frame_time varchar(32) DEFAULT NULL,
  object_type varchar(64) DEFAULT NULL,
  created_at timestamp NOT NULL DEFAULT current_timestamp(),
  estimated_age int(11) DEFAULT 0,
  photo_path varchar(255) DEFAULT '',
  x double DEFAULT 0,
  y double DEFAULT 0,
  event varchar(128) DEFAULT ''
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

                    MTX_SCRIPT="$WORKSPACE/mediamtx/run_mediamtx.sh"
                    MTX_PID_FILE="$WORKSPACE/.ci-mediamtx.pid"
                    MTX_LOG="$WORKSPACE/.ci-mediamtx.log"

                    if [ ! -f "$MTX_SCRIPT" ]; then
                      echo "mediamtx start script is missing: $MTX_SCRIPT" >&2
                      exit 1
                    fi

                    # Keep a single local mediamtx process per build.
                    pkill -f '/mediamtx/bin/mediamtx' 2>/dev/null || true

                    nohup bash "$MTX_SCRIPT" >"$MTX_LOG" 2>&1 &
                    echo "$!" > "$MTX_PID_FILE"

                    python3 - <<'PY'
import socket
import time

deadline = time.time() + 60
last_error = None
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", 8554), timeout=1.0):
            print("mediamtx is listening on 127.0.0.1:8554")
            break
    except OSError as exc:
        last_error = exc
        time.sleep(1)
else:
    raise SystemExit(f"mediamtx did not open 127.0.0.1:8554 in time: {last_error}")
PY
                '''
            }
        }

        stage('Run Stream Tests') {
            steps {
                sh '''
                    set -eu
                    export MYSQL_UNIX_PORT="$WORKSPACE/.ci-mariadb/mysqld.sock"
                    export SFEPS_STREAM_RTSP_URL="${SFEPS_STREAM_RTSP_URL:-rtsp://127.0.0.1:8554/cam1}"
                    export SFEPS_STREAM_FAULT_DOWN_CMD="pkill -f '/mediamtx/bin/mediamtx' || true"
                    export SFEPS_STREAM_FAULT_UP_CMD="nohup bash '$WORKSPACE/mediamtx/run_mediamtx.sh' >'$WORKSPACE/.ci-mediamtx.log' 2>&1 &"
                    mkdir -p reports
                    python3 -m pytest -q tests/test_tc_func_stream.py -r a --junitxml=reports/stream-tests.xml
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
                pkill -f '/mediamtx/bin/mediamtx' 2>/dev/null || true
            '''
            junit testResults: 'reports/*.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'tests/real_server.log,.ci-mediamtx.log', allowEmptyArchive: true
        }
    }
}
