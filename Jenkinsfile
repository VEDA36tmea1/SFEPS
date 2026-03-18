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

        SFEPS_TEST_HOST = "${env.SFEPS_TEST_HOST ?: '192.168.0.101'}"
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
