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

        // CD settings (override in Jenkins job/global env)
        SFEPS_DOCKER_REGISTRY = "${env.SFEPS_DOCKER_REGISTRY ?: 'ghcr.io'}"
        SFEPS_DOCKER_IMAGE_REPO = "${env.SFEPS_DOCKER_IMAGE_REPO ?: 'veda36tmea1/sfeps-server'}"
        SFEPS_DOCKER_PLATFORM = "${env.SFEPS_DOCKER_PLATFORM ?: 'linux/arm64'}"
        SFEPS_DOCKERFILE_PATH = "${env.SFEPS_DOCKERFILE_PATH ?: 'docker/server/Dockerfile'}"
        SFEPS_REGISTRY_CREDENTIALS_ID = "${env.SFEPS_REGISTRY_CREDENTIALS_ID ?: 'sfeps-registry-creds'}"
        // Single-job fallback branch (set to main when running main in a single Pipeline job)
        SFEPS_SINGLE_JOB_BRANCH = "${env.SFEPS_SINGLE_JOB_BRANCH ?: 'develop'}"

        SFEPS_TEST_HOST = "${env.SFEPS_TEST_HOST ?: '192.168.0.82'}"
        SFEPS_TEST_SSH_CREDENTIALS_ID = "${env.SFEPS_TEST_SSH_CREDENTIALS_ID ?: 'sfeps-test-ssh'}"
        SFEPS_TEST_CONTAINER_NAME = "${env.SFEPS_TEST_CONTAINER_NAME ?: 'sfeps-server-test'}"

        SFEPS_PROD_HOST = "${env.SFEPS_PROD_HOST ?: ''}"
        SFEPS_PROD_SSH_CREDENTIALS_ID = "${env.SFEPS_PROD_SSH_CREDENTIALS_ID ?: 'sfeps-prod-ssh'}"
        SFEPS_PROD_CONTAINER_NAME = "${env.SFEPS_PROD_CONTAINER_NAME ?: 'sfeps-server-prod'}"

        SFEPS_REMOTE_ENV_FILE = "${env.SFEPS_REMOTE_ENV_FILE ?: '/home/iam/SFEPS/server/.env.local'}"
        SFEPS_CONTAINER_ENV_FILE = "${env.SFEPS_CONTAINER_ENV_FILE ?: '/opt/sfeps/server/.env.local'}"
        SFEPS_REMOTE_PKI_DIR = "${env.SFEPS_REMOTE_PKI_DIR ?: '/etc/sfeps/pki'}"
        SFEPS_REMOTE_MYSQL_SOCK_DIR = "${env.SFEPS_REMOTE_MYSQL_SOCK_DIR ?: '/run/mysqld'}"
        SFEPS_VIDEO_DIR = "${env.SFEPS_VIDEO_DIR ?: '/home/iam/SFEPS/videos'}"
        SFEPS_HEALTH_PORT = "${env.SFEPS_HEALTH_PORT ?: '5555'}"
        SFEPS_SLACK_NOTIFY = "${env.SFEPS_SLACK_NOTIFY ?: '1'}"
        SFEPS_SLACK_WEBHOOK_CREDENTIALS_ID = "${env.SFEPS_SLACK_WEBHOOK_CREDENTIALS_ID ?: 'sfeps-slack-webhook'}"
        SFEPS_SLACK_CHANNEL = "${env.SFEPS_SLACK_CHANNEL ?: ''}"
        SFEPS_SQUISH_RUNNER = "${env.SFEPS_SQUISH_RUNNER ?: ''}"
        SFEPS_SQUISH_SERVER = "${env.SFEPS_SQUISH_SERVER ?: ''}"
        SFEPS_SQUISH_SUITE_PATH = "${env.SFEPS_SQUISH_SUITE_PATH ?: 'tests/squish/suite_sfeps/suite_sfeps'}"
        SFEPS_SQUISH_AUT_PATH = "${env.SFEPS_SQUISH_AUT_PATH ?: ''}"
        SFEPS_SQUISH_REQUIRED = "${env.SFEPS_SQUISH_REQUIRED ?: '1'}"
        SFEPS_WINDOWS_GUI_AGENT_LABEL = "${env.SFEPS_WINDOWS_GUI_AGENT_LABEL ?: 'windows-gui'}"

    }

    stages {
        stage('Checkout') {
            steps {
                checkout scm
            }
        }

        stage('Resolve CI Metadata') {
            steps {
                script {
                    def branch = env.BRANCH_NAME?.trim()
                    if (branch == 'null') {
                        branch = ''
                    }
                    if (!branch) {
                        branch = env.GIT_BRANCH?.trim()
                        if (branch == 'null') {
                            branch = ''
                        }
                        if (branch?.startsWith('origin/')) {
                            branch = branch.substring('origin/'.length())
                        }
                    }
                    if (!branch || branch == 'HEAD') {
                        branch = sh(
                            returnStdout: true,
                            script: '''git branch -r --contains HEAD | sed 's#^ *origin/##' | grep -v '^HEAD ->' | grep -v '^HEAD$' | head -n1 || true'''
                        ).trim()
                    }
                    if (!branch || branch == 'null' || branch == 'HEAD') {
                        branch = env.SFEPS_SINGLE_JOB_BRANCH?.trim()
                    }
                    if (!branch || branch == 'null' || branch == 'HEAD') {
                        error "Unable to resolve branch name for CD. Set SFEPS_SINGLE_JOB_BRANCH (e.g. develop/main) in Jenkins job env."
                    }

                    def gitShaShort = sh(returnStdout: true, script: "git rev-parse --short=8 HEAD").trim()
                    if (!gitShaShort || gitShaShort == 'null') {
                        error "Unable to resolve git SHA for CD."
                    }

                    env.SFEPS_CI_BRANCH = branch
                    env.SFEPS_GIT_SHA_SHORT = gitShaShort
                    def branchTag = branch.replaceAll("[^A-Za-z0-9_.-]+", "-")

                    if (env.SFEPS_DOCKER_REGISTRY?.trim()) {
                        env.SFEPS_IMAGE_REF = "${env.SFEPS_DOCKER_REGISTRY}/${env.SFEPS_DOCKER_IMAGE_REPO}:${branchTag}-${env.BUILD_NUMBER}-${env.SFEPS_GIT_SHA_SHORT}"
                        env.SFEPS_IMAGE_LATEST_REF = "${env.SFEPS_DOCKER_REGISTRY}/${env.SFEPS_DOCKER_IMAGE_REPO}:${branchTag}-latest"
                    } else {
                        env.SFEPS_IMAGE_REF = ""
                        env.SFEPS_IMAGE_LATEST_REF = ""
                    }

                    echo "Resolved branch=${branch}, sha=${gitShaShort}"
                    if (env.SFEPS_IMAGE_REF) {
                        echo "CD image tag=${env.SFEPS_IMAGE_REF}"
                    }
                }
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
                    rm -rf reports
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

        stage('Ensure Test Server Before Squish') {
            when {
                expression { env.SFEPS_CI_BRANCH == 'develop' }
            }
            steps {
                withCredentials([
                    sshUserPrivateKey(
                        credentialsId: "${env.SFEPS_TEST_SSH_CREDENTIALS_ID}",
                        keyFileVariable: 'SSH_KEY',
                        usernameVariable: 'SSH_USER'
                    )
                ]) {
                    sh '''
                        set -eu
                        if [ -z "${SFEPS_TEST_HOST}" ]; then
                          echo "SFEPS_TEST_HOST is required before Squish tests." >&2
                          exit 1
                        fi

                        REMOTE="${SSH_USER}@${SFEPS_TEST_HOST}"
                        SSH_OPTS="-i ${SSH_KEY} -o BatchMode=yes -o StrictHostKeyChecking=accept-new"

                        ssh ${SSH_OPTS} "${REMOTE}" "set -eu
                          if ! docker ps --format '{{.Names}}' | grep -Fx '${SFEPS_TEST_CONTAINER_NAME}' >/dev/null; then
                            if docker ps -a --format '{{.Names}}' | grep -Fx '${SFEPS_TEST_CONTAINER_NAME}' >/dev/null; then
                              docker start '${SFEPS_TEST_CONTAINER_NAME}' >/dev/null
                              echo 'Started test container: ${SFEPS_TEST_CONTAINER_NAME}'
                            else
                              echo 'Missing test container: ${SFEPS_TEST_CONTAINER_NAME}' >&2
                              exit 1
                            fi
                          else
                            echo 'Test container already running: ${SFEPS_TEST_CONTAINER_NAME}'
                          fi

                          if timeout 90 bash -lc 'while ! cat </dev/null >/dev/tcp/127.0.0.1/${SFEPS_HEALTH_PORT} 2>/dev/null; do sleep 2; done'; then
                            echo 'Pre-Squish health check OK on port ${SFEPS_HEALTH_PORT}'
                            exit 0
                          fi

                          echo 'Pre-Squish health check FAILED' >&2
                          docker logs --tail 120 '${SFEPS_TEST_CONTAINER_NAME}' || true
                          exit 1"
                    '''
                }
            }
        }

                stage('Run Squish UI Tests') {
                        steps {
                                script {
                            def squishStepFailed = false
                                        node(env.SFEPS_WINDOWS_GUI_AGENT_LABEL) {
                                                deleteDir()
                                                checkout scm

                                try {
                                    bat '''
                                                        @echo off
                                                        setlocal EnableExtensions EnableDelayedExpansion
                                                        set "REPORT_DIR=%CD%\\reports"
                                                        if not exist "%REPORT_DIR%" mkdir "%REPORT_DIR%"

                                                        set "SQUISH_REQUIRED=1"

                                                        set "SQUISH_RUNNER=%SFEPS_SQUISH_RUNNER%"
                                                        if "%SQUISH_RUNNER%"=="" (
                                                            for /f "delims=" %%R in ('where squishrunner.exe 2^>nul') do (
                                                                set "SQUISH_RUNNER=%%R"
                                                                goto :runner_found
                                                            )
                                                            if exist "C:\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\froglogic\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\froglogic\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\Program Files\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Program Files\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\Program Files\\froglogic\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Program Files\\froglogic\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\Program Files ^(x86^)\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Program Files ^(x86^)\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\Program Files ^(x86^)\\froglogic\\Squish\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Program Files ^(x86^)\\froglogic\\Squish\\bin\\squishrunner.exe"
                                                            if "%SQUISH_RUNNER%"=="" if exist "C:\\Users\\2-08\\Squish for Qt 9.2.0\\bin\\squishrunner.exe" set "SQUISH_RUNNER=C:\\Users\\2-08\\Squish for Qt 9.2.0\\bin\\squishrunner.exe"
                                                        )
                                                        :runner_found

                                                        set "SQUISH_SERVER=%SFEPS_SQUISH_SERVER%"
                                                        if "%SQUISH_SERVER%"=="" (
                                                            for %%I in ("%SQUISH_RUNNER%") do set "SQUISH_SERVER=%%~dpIsquishserver.exe"
                                                        )

                                                        set "SUITE_PATH=%SFEPS_SQUISH_SUITE_PATH%"
                                                        if "%SUITE_PATH%"=="" set "SUITE_PATH=tests\\squish\\suite_sfeps\\suite_sfeps"

                                                        set "AUT_PATH=%SFEPS_SQUISH_AUT_PATH%"
                                                        if "%AUT_PATH%"=="" (
                                                            if exist "client\\build-mingw\\appHanwhaVisionSFEPS.exe" (
                                                                set "AUT_PATH=client\\build-mingw\\appHanwhaVisionSFEPS.exe"
                                                            ) else if exist "client\\build\\appHanwhaVisionSFEPS.exe" (
                                                                set "AUT_PATH=client\\build\\appHanwhaVisionSFEPS.exe"
                                                            ) else if exist "C:\\Jenkins\\workspace\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe" (
                                                                set "AUT_PATH=C:\\Jenkins\\workspace\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe"
                                                            ) else if exist "C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe" (
                                                                set "AUT_PATH=C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe"
                                                            ) else if exist "C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build\\appHanwhaVisionSFEPS.exe" (
                                                                set "AUT_PATH=C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build\\appHanwhaVisionSFEPS.exe"
                                                            )
                                                        )

                                                        if "%SQUISH_RUNNER%"=="" (
                                                            echo squishrunner not found on Windows GUI agent.
                                                            echo Hint: set SFEPS_SQUISH_RUNNER to full path, e.g. C:\\Squish\\bin\\squishrunner.exe
                                                            exit /b 1
                                                        )

                                                        where "%SQUISH_RUNNER%" >nul 2>nul
                                                        if errorlevel 1 (
                                                            if not exist "%SQUISH_RUNNER%" (
                                                                echo squishrunner path does not exist: %SQUISH_RUNNER%
                                                                exit /b 1
                                                            )
                                                        )

                                                        echo Using squishrunner: %SQUISH_RUNNER%

                                                        set "STARTED_SQUISH_SERVER=0"
                                                        echo Killing any pre-existing squishserver instances...
                                                        taskkill /F /IM squishserver.exe >nul 2>nul
                                                        timeout /t 1 >nul
                                                        
                                                        if not exist "%SQUISH_SERVER%" (
                                                            echo squishserver not found on Windows GUI agent: %SQUISH_SERVER%
                                                            exit /b 1
                                                        )
                                                        
                                                        echo Starting squishserver: %SQUISH_SERVER%
                                                        start "squishserver" /B "%SQUISH_SERVER%" > "%TEMP%\\squishserver.log" 2>&1
                                                        echo Waiting for squishserver to initialize...
                                                        
                                                        timeout /t 1 >nul
                                                        tasklist | find /I "squishserver.exe"
                                                        if errorlevel 1 (
                                                            echo ERROR: squishserver process did not start!
                                                            if exist "%TEMP%\\squishserver.log" type "%TEMP%\\squishserver.log"
                                                            exit /b 1
                                                        )
                                                        echo squishserver process is running
                                                        
                                                        timeout /t 3 >nul
                                                        
                                                        echo Checking if squishserver port 4322 is open...
                                                        netstat -ano | find ":4322" > "%TEMP%\\netstat_result.txt"
                                                        if errorlevel 1 (
                                                            echo ERROR: squishserver port 4322 is NOT listening!
                                                            netstat -ano
                                                            exit /b 1
                                                        )
                                                        echo Squish server port 4322 is LISTENING (confirmed by netstat)
                                                        type "%TEMP%\\netstat_result.txt"
                                                        set "STARTED_SQUISH_SERVER=1"

                                                        if "%AUT_PATH%"=="" (
                                                            echo AUT binary not found on Windows GUI agent.
                                                            echo Set SFEPS_SQUISH_AUT_PATH or build the client on that node.
                                                            echo Checked:
                                                            echo  - client\\build-mingw\\appHanwhaVisionSFEPS.exe
                                                            echo  - client\\build\\appHanwhaVisionSFEPS.exe
                                                            echo  - C:\\Jenkins\\workspace\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe
                                                            echo  - C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build-mingw\\appHanwhaVisionSFEPS.exe
                                                            echo  - C:\\Users\\2-08\\Desktop\\SFEPS\\client\\build\\appHanwhaVisionSFEPS.exe
                                                            exit /b 1
                                                        )

                                                        if not exist "%AUT_PATH%" (
                                                            echo AUT binary path does not exist on Windows GUI agent: %AUT_PATH%
                                                            exit /b 1
                                                        )

                                                        set SQUISH_FAILED=0
                                                        for %%T in (
                                                            tst_tc_func_ui_01
                                                            tst_tc_func_stream_02
                                                            tst_tc_func_ui_02
                                                            tst_tc_func_ui_03
                                                            tst_tc_func_track_01
                                                            tst_tc_func_track_02
                                                        ) do (
                                                            set "REPORT_FILE=!REPORT_DIR!\\squish-%%T.xml"
                                                            if exist "!REPORT_FILE!" del /f /q "!REPORT_FILE!"
                                                            for /f %%I in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "TC_START_MS=%%I"
                                                            call "%SQUISH_RUNNER%" --testsuite "%SUITE_PATH%" --testcase %%T --aut "%AUT_PATH%" --reportgen "junit,!REPORT_FILE!" --exitCodeOnFail 1
                                                            set "TC_RC=!ERRORLEVEL!"
                                                            for /f %%I in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "TC_END_MS=%%I"
                                                            set /a TC_ELAPSED_MS=!TC_END_MS!-!TC_START_MS!
                                                            if !TC_ELAPSED_MS! lss 0 set "TC_ELAPSED_MS=0"
                                                            for /f %%I in ('powershell -NoProfile -Command "$ms=[double]$env:TC_ELAPSED_MS; [string]::Format([System.Globalization.CultureInfo]::InvariantCulture,'{0:0.000}',$ms/1000.0)"') do set "TC_ELAPSED_SEC=%%I"
                                                            if not exist "!REPORT_FILE!" (
                                                                echo Squish did not generate JUnit XML for %%T, writing fallback report.
                                                                > "!REPORT_FILE!" echo ^<testsuite name="%%T" tests="1" failures="0" errors="0" skipped="0" time="!TC_ELAPSED_SEC!"^>
                                                                if "!TC_RC!"=="0" (
                                                                    >> "!REPORT_FILE!" echo   ^<testcase classname="squish.%%T" name="%%T" time="!TC_ELAPSED_SEC!" /^>
                                                                ) else (
                                                                    >> "!REPORT_FILE!" echo   ^<testcase classname="squish.%%T" name="%%T" time="!TC_ELAPSED_SEC!"^>^<failure message="squishrunner exited with code !TC_RC!" /^>^</testcase^>
                                                                    > "!REPORT_FILE!.tmp" (
                                                                        echo ^<testsuite name="%%T" tests="1" failures="1" errors="0" skipped="0" time="!TC_ELAPSED_SEC!"^>
                                                                        echo   ^<testcase classname="squish.%%T" name="%%T" time="!TC_ELAPSED_SEC!"^>^<failure message="squishrunner exited with code !TC_RC!" /^>^</testcase^>
                                                                        echo ^</testsuite^>
                                                                    )
                                                                    move /Y "!REPORT_FILE!.tmp" "!REPORT_FILE!" >nul
                                                                )
                                                                if "!TC_RC!"=="0" (
                                                                    >> "!REPORT_FILE!" echo ^</testsuite^>
                                                                )
                                                            )
                                                            if exist "!REPORT_FILE!" (
                                                                powershell -NoProfile -Command "$p=$env:REPORT_FILE; $t=$env:TC_ELAPSED_SEC; [xml]$xml=Get-Content -LiteralPath $p; if ($xml.testsuite) { if (-not $xml.testsuite.time -or [double]$xml.testsuite.time -eq 0) { $xml.testsuite.time = $t }; foreach ($tc in @($xml.testsuite.testcase)) { if (-not $tc.time -or [double]$tc.time -eq 0) { $tc.time = $t } }; $xml.Save($p) }"
                                                                echo Generated Squish report: !REPORT_FILE! ^(time=!TC_ELAPSED_SEC!s^)
                                                            )
                                                            if not "!TC_RC!"=="0" set SQUISH_FAILED=1
                                                        )

                                                        if "%STARTED_SQUISH_SERVER%"=="1" (
                                                            taskkill /F /IM squishserver.exe >nul 2>nul
                                                        )

                                                        if "%SQUISH_FAILED%"=="1" exit /b 1
                                                '''
                                                    } catch (err) {
                                                        squishStepFailed = true
                                                        echo "Squish execution failed on Windows node, but stashing reports before failing stage."
                                                    }
                                                stash name: 'squish-reports', includes: 'reports/**', allowEmpty: true
                                        }

                                        try {
                                                unstash 'squish-reports'
                                        } catch (err) {
                                                echo "No Squish reports were stashed: ${err}"
                                        }

                                        if (squishStepFailed) {
                                            error('Run Squish UI Tests failed.')
                                        }
                                }
                        }
                }

        stage('Build & Push ARM Image') {
            when {
                expression { env.SFEPS_CI_BRANCH == 'develop' || env.SFEPS_CI_BRANCH == 'main' }
            }
            steps {
                withCredentials([
                    usernamePassword(
                        credentialsId: "${env.SFEPS_REGISTRY_CREDENTIALS_ID}",
                        usernameVariable: 'REGISTRY_USER',
                        passwordVariable: 'REGISTRY_PASS'
                    )
                ]) {
                    sh '''
                        set -eu
                        if [ -z "${SFEPS_DOCKER_REGISTRY}" ]; then
                          echo "SFEPS_DOCKER_REGISTRY is required for CD image push." >&2
                          exit 1
                        fi
                        if [ -z "${SFEPS_IMAGE_REF}" ] || [ -z "${SFEPS_IMAGE_LATEST_REF}" ]; then
                          echo "CD image tags are empty. Resolve CI Metadata stage failed." >&2
                          exit 1
                        fi

                        docker buildx inspect sfeps-builder >/dev/null 2>&1 || docker buildx create --name sfeps-builder --use
                        docker buildx use sfeps-builder

                        printf '%s' "${REGISTRY_PASS}" | docker login "${SFEPS_DOCKER_REGISTRY}" -u "${REGISTRY_USER}" --password-stdin
                        docker buildx build \
                          --platform "${SFEPS_DOCKER_PLATFORM}" \
                          -f "${SFEPS_DOCKERFILE_PATH}" \
                          -t "${SFEPS_IMAGE_REF}" \
                          -t "${SFEPS_IMAGE_LATEST_REF}" \
                          --push \
                          .
                    '''
                }
            }
        }

        stage('Deploy To Test Raspberry (develop)') {
            when {
                expression { env.SFEPS_CI_BRANCH == 'develop' }
            }
            steps {
                withCredentials([
                    usernamePassword(
                        credentialsId: "${env.SFEPS_REGISTRY_CREDENTIALS_ID}",
                        usernameVariable: 'REGISTRY_USER',
                        passwordVariable: 'REGISTRY_PASS'
                    ),
                    sshUserPrivateKey(
                        credentialsId: "${env.SFEPS_TEST_SSH_CREDENTIALS_ID}",
                        keyFileVariable: 'SSH_KEY',
                        usernameVariable: 'SSH_USER'
                    )
                ]) {
                    sh '''
                        set -eu
                        if [ -z "${SFEPS_TEST_HOST}" ]; then
                          echo "SFEPS_TEST_HOST is required for develop deployment." >&2
                          exit 1
                        fi
                        REMOTE="${SSH_USER}@${SFEPS_TEST_HOST}"
                        SSH_OPTS="-i ${SSH_KEY} -o BatchMode=yes -o StrictHostKeyChecking=accept-new"

                        printf '%s' "${REGISTRY_PASS}" | ssh ${SSH_OPTS} "${REMOTE}" \
                          "docker login '${SFEPS_DOCKER_REGISTRY}' -u '${REGISTRY_USER}' --password-stdin"

                        ssh ${SSH_OPTS} "${REMOTE}" "set -eu
                          docker pull '${SFEPS_IMAGE_REF}'
                          docker rm -f '${SFEPS_TEST_CONTAINER_NAME}' >/dev/null 2>&1 || true
                          mkdir -p '${SFEPS_VIDEO_DIR}'
                          if [ ! -r '${SFEPS_REMOTE_ENV_FILE}' ]; then
                            echo 'missing env file: ${SFEPS_REMOTE_ENV_FILE}' >&2
                            exit 1
                          fi
                          if grep -nE '^[[:space:]]*(<<<<<<<|=======|>>>>>>>)' '${SFEPS_REMOTE_ENV_FILE}' >/dev/null; then
                            echo 'env file has unresolved merge conflict markers: ${SFEPS_REMOTE_ENV_FILE}' >&2
                            grep -nE '^[[:space:]]*(<<<<<<<|=======|>>>>>>>)' '${SFEPS_REMOTE_ENV_FILE}' || true
                            exit 1
                          fi
                          if [ ! -S '${SFEPS_REMOTE_MYSQL_SOCK_DIR}/mysqld.sock' ]; then
                            echo 'missing mysql socket: ${SFEPS_REMOTE_MYSQL_SOCK_DIR}/mysqld.sock' >&2
                            exit 1
                          fi
                          grep -Ev '^(SFEPS_APP_BIND_IP|SFEPS_APP_TLS_ENABLE|SFEPS_APP_PLAINTEXT_ENABLE|SFEPS_APP_TLS_CERT_FILE|SFEPS_APP_TLS_KEY_FILE|SFEPS_ESP_TCP_ENABLE|SFEPS_ESP_TCP_BIND_IP|SFEPS_ESP_TCP_PORT|SFEPS_ESP_TCP_MAX_CLIENTS|SFEPS_ESP_TCP_ALLOW_IPS)=' \
                            '${SFEPS_REMOTE_ENV_FILE}' > '/tmp/sfeps-server-test.env'
                          {
                            echo 'SFEPS_APP_BIND_IP=0.0.0.0'
                            echo 'SFEPS_APP_TLS_ENABLE=0'
                            echo 'SFEPS_APP_PLAINTEXT_ENABLE=1'
                            echo 'SFEPS_ESP_TCP_ENABLE=0'
                          } >> '/tmp/sfeps-server-test.env'
                          if ! docker run -d --name '${SFEPS_TEST_CONTAINER_NAME}' --restart unless-stopped --network host \
                            -v '/tmp/sfeps-server-test.env:${SFEPS_CONTAINER_ENV_FILE}:ro' \
                            -v '${SFEPS_REMOTE_PKI_DIR}:${SFEPS_REMOTE_PKI_DIR}:ro' \
                            -v '${SFEPS_REMOTE_MYSQL_SOCK_DIR}:${SFEPS_REMOTE_MYSQL_SOCK_DIR}' \
                            -v '${SFEPS_VIDEO_DIR}:${SFEPS_VIDEO_DIR}' \
                            -e SFEPS_ENV_FILE='${SFEPS_CONTAINER_ENV_FILE}' \
                            '${SFEPS_IMAGE_REF}'; then
                            echo 'docker run failed for test deploy' >&2
                            docker ps -a --filter name='${SFEPS_TEST_CONTAINER_NAME}' || true
                            exit 1
                          fi"

                        ssh ${SSH_OPTS} "${REMOTE}" "set -eu
                          if timeout 90 bash -lc 'while ! cat </dev/null >/dev/tcp/127.0.0.1/${SFEPS_HEALTH_PORT} 2>/dev/null; do sleep 2; done'; then
                            echo 'test deploy health check OK on port ${SFEPS_HEALTH_PORT}'
                            exit 0
                          fi
                          echo 'test deploy health check FAILED' >&2
                          docker logs --tail 120 '${SFEPS_TEST_CONTAINER_NAME}' || true
                          exit 1"
                    '''
                }
            }
        }

        stage('Approve Production Deployment') {
            when {
                expression { env.SFEPS_CI_BRANCH == 'main' }
            }
            steps {
                timeout(time: 30, unit: 'MINUTES') {
                    input message: 'Deploy main image to production Raspberry Pi?', ok: 'Deploy'
                }
            }
        }

        stage('Deploy To Production Raspberry (main)') {
            when {
                expression { env.SFEPS_CI_BRANCH == 'main' }
            }
            steps {
                withCredentials([
                    usernamePassword(
                        credentialsId: "${env.SFEPS_REGISTRY_CREDENTIALS_ID}",
                        usernameVariable: 'REGISTRY_USER',
                        passwordVariable: 'REGISTRY_PASS'
                    ),
                    sshUserPrivateKey(
                        credentialsId: "${env.SFEPS_PROD_SSH_CREDENTIALS_ID}",
                        keyFileVariable: 'SSH_KEY',
                        usernameVariable: 'SSH_USER'
                    )
                ]) {
                    sh '''
                        set -eu
                        if [ -z "${SFEPS_PROD_HOST}" ]; then
                          echo "SFEPS_PROD_HOST is required for production deployment." >&2
                          exit 1
                        fi
                        REMOTE="${SSH_USER}@${SFEPS_PROD_HOST}"
                        SSH_OPTS="-i ${SSH_KEY} -o BatchMode=yes -o StrictHostKeyChecking=accept-new"

                        printf '%s' "${REGISTRY_PASS}" | ssh ${SSH_OPTS} "${REMOTE}" \
                          "docker login '${SFEPS_DOCKER_REGISTRY}' -u '${REGISTRY_USER}' --password-stdin"

                        ssh ${SSH_OPTS} "${REMOTE}" "set -eu
                          docker pull '${SFEPS_IMAGE_REF}'
                          docker rm -f '${SFEPS_PROD_CONTAINER_NAME}' >/dev/null 2>&1 || true
                          mkdir -p '${SFEPS_VIDEO_DIR}'
                          if [ ! -r '${SFEPS_REMOTE_ENV_FILE}' ]; then
                            echo 'missing env file: ${SFEPS_REMOTE_ENV_FILE}' >&2
                            exit 1
                          fi
                          if grep -nE '^[[:space:]]*(<<<<<<<|=======|>>>>>>>)' '${SFEPS_REMOTE_ENV_FILE}' >/dev/null; then
                            echo 'env file has unresolved merge conflict markers: ${SFEPS_REMOTE_ENV_FILE}' >&2
                            grep -nE '^[[:space:]]*(<<<<<<<|=======|>>>>>>>)' '${SFEPS_REMOTE_ENV_FILE}' || true
                            exit 1
                          fi
                          if [ ! -S '${SFEPS_REMOTE_MYSQL_SOCK_DIR}/mysqld.sock' ]; then
                            echo 'missing mysql socket: ${SFEPS_REMOTE_MYSQL_SOCK_DIR}/mysqld.sock' >&2
                            exit 1
                          fi
                          grep -Ev '^(SFEPS_APP_TLS_ENABLE|SFEPS_APP_PLAINTEXT_ENABLE|SFEPS_APP_TLS_CERT_FILE|SFEPS_APP_TLS_KEY_FILE)=' \
                            '${SFEPS_REMOTE_ENV_FILE}' > '/tmp/sfeps-server-prod.env'
                          {
                            echo 'SFEPS_APP_TLS_ENABLE=0'
                            echo 'SFEPS_APP_PLAINTEXT_ENABLE=1'
                          } >> '/tmp/sfeps-server-prod.env'
                          if ! docker run -d --name '${SFEPS_PROD_CONTAINER_NAME}' --restart unless-stopped --network host \
                            -v '/tmp/sfeps-server-prod.env:${SFEPS_CONTAINER_ENV_FILE}:ro' \
                            -v '${SFEPS_REMOTE_PKI_DIR}:${SFEPS_REMOTE_PKI_DIR}:ro' \
                            -v '${SFEPS_REMOTE_MYSQL_SOCK_DIR}:${SFEPS_REMOTE_MYSQL_SOCK_DIR}' \
                            -v '${SFEPS_VIDEO_DIR}:${SFEPS_VIDEO_DIR}' \
                            -e SFEPS_ENV_FILE='${SFEPS_CONTAINER_ENV_FILE}' \
                            '${SFEPS_IMAGE_REF}'; then
                            echo 'docker run failed for production deploy' >&2
                            docker ps -a --filter name='${SFEPS_PROD_CONTAINER_NAME}' || true
                            exit 1
                          fi"

                        ssh ${SSH_OPTS} "${REMOTE}" "set -eu
                          if timeout 90 bash -lc 'while ! cat </dev/null >/dev/tcp/127.0.0.1/${SFEPS_HEALTH_PORT} 2>/dev/null; do sleep 2; done'; then
                            echo 'production deploy health check OK on port ${SFEPS_HEALTH_PORT}'
                            exit 0
                          fi
                          echo 'production deploy health check FAILED' >&2
                          docker logs --tail 120 '${SFEPS_PROD_CONTAINER_NAME}' || true
                          exit 1"
                    '''
                }
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

            script {
                def notifyFlag = (env.SFEPS_SLACK_NOTIFY ?: '0').trim().toLowerCase()
                if (!(notifyFlag in ['1', 'true', 'yes', 'on'])) {
                    echo "Slack notification disabled (SFEPS_SLACK_NOTIFY=${env.SFEPS_SLACK_NOTIFY})."
                    return
                }

                def status = currentBuild.currentResult ?: 'UNKNOWN'
                def emoji = '[INFO]'
                if (status == 'SUCCESS') {
                    emoji = '[SUCCESS]'
                } else if (status == 'FAILURE') {
                    emoji = '[FAILURE]'
                } else if (status == 'UNSTABLE') {
                    emoji = '[UNSTABLE]'
                } else if (status == 'ABORTED') {
                    emoji = '[ABORTED]'
                }

                env.SFEPS_NOTIFY_STATUS = status
                env.SFEPS_NOTIFY_EMOJI = emoji

                try {
                    withCredentials([
                        string(
                            credentialsId: "${env.SFEPS_SLACK_WEBHOOK_CREDENTIALS_ID}",
                            variable: 'SLACK_WEBHOOK_URL'
                        )
                    ]) {
                        sh '''
                            set +e
                            python3 - <<'PY'
import json
import os
import urllib.request

webhook = os.environ.get("SLACK_WEBHOOK_URL", "").strip()
if not webhook:
    raise SystemExit(0)

status = os.environ.get("SFEPS_NOTIFY_STATUS", "UNKNOWN")
emoji = os.environ.get("SFEPS_NOTIFY_EMOJI", "[INFO]")
job = os.environ.get("JOB_NAME", "unknown-job")
build_no = os.environ.get("BUILD_NUMBER", "?")
branch = os.environ.get("SFEPS_CI_BRANCH") or os.environ.get("BRANCH_NAME", "unknown")
build_url = os.environ.get("BUILD_URL", "")
image_ref = os.environ.get("SFEPS_IMAGE_REF", "")
channel = os.environ.get("SFEPS_SLACK_CHANNEL", "").strip()

lines = [
    f"{emoji} *{status}* `{job} #{build_no}`",
    f"- branch: `{branch}`",
]
if image_ref:
    lines.append(f"- image: `{image_ref}`")
if build_url:
    lines.append(f"- build: <{build_url}|Open Jenkins Build>")

payload = {"text": "\\n".join(lines)}
if channel:
    payload["channel"] = channel

req = urllib.request.Request(
    webhook,
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"},
)
with urllib.request.urlopen(req, timeout=10) as resp:
    resp.read()
PY
                            rc=$?
                            if [ "$rc" -ne 0 ]; then
                              echo "Slack notification failed (non-fatal), exit=$rc"
                            fi
                            exit 0
                        '''
                    }
                } catch (err) {
                    echo "Slack notification skipped (non-fatal): ${err}"
                }
            }
        }
    }
}
