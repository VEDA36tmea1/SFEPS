pipeline {
    agent any

    options {
        timestamps()
        timeout(time: 20, unit: 'MINUTES')
    }

    parameters {
        string(name: 'SFEPS_PERF_ALERT_HOST', defaultValue: '192.168.0.92', description: 'Alert TCP host (server alert listener host)')
        string(name: 'SFEPS_PERF_ALERT_PORT', defaultValue: '5557', description: 'Alert TCP port')

        string(name: 'SFEPS_PERF_TARGET_COUNT', defaultValue: '20', description: 'Required suspicious event count')
        string(name: 'SFEPS_PERF_WINDOW_SEC', defaultValue: '60', description: 'Performance time window in seconds')
        string(name: 'SFEPS_PERF_ALLOWED_MISSING', defaultValue: '0', description: 'Allowed missing events')
        string(name: 'SFEPS_PERF_ALLOWED_DUPLICATE', defaultValue: '0', description: 'Allowed duplicated events')
        string(name: 'SFEPS_PERF_MESSAGE_PREFIX', defaultValue: 'FRAUD|', description: 'Alert message prefix to count')

        string(name: 'SFEPS_PERF_RFID_SOCKET_PATH', defaultValue: '/tmp/rc522_events.sock', description: 'UDS path for RFID NDJSON injection')
        string(name: 'SFEPS_PERF_RFID_TEXT', defaultValue: 'Invalid', description: 'RFID text injected by test (mismatch text for suspicious event)')
        string(name: 'SFEPS_PERF_RFID_UID_SEED', defaultValue: '2684354560', description: 'UID seed (decimal, default 0xA0000000)')
        string(name: 'SFEPS_PERF_RFID_DEVICE_ID', defaultValue: '1', description: 'device_id in injected NDJSON')
        string(name: 'SFEPS_PERF_RFID_SEND_INTERVAL_SEC', defaultValue: '0.05', description: 'Interval between injected RFID lines')
        string(name: 'SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC', defaultValue: '10', description: 'UDS accept timeout')
        string(name: 'AGENT_DOCKER_IMAGE', defaultValue: 'my-registry.example.com/myorg/sfeps-jenkins-agent:latest', description: 'Optional: Docker image to run build steps inside')
        string(name: 'SFEPS_PI_WORKDIR', defaultValue: '/home/iam/finalProject/SFEPS', description: 'SFEPS repo path on Raspberry Pi')
    }

    stages {
        stage('Checkout') {
            steps {
                checkout scm
            }
        }

        stage('Validate Agent') {
            steps {
                script {
                    if (!isUnix()) {
                        error('TC-NF-PERF-02 UDS test requires Linux/Unix Jenkins agent.')
                    }
                }
                sh 'python3 --version'
            }
        }

        stage('Setup Python') {
            steps {
                sh '''
                    python3 -m venv .venv-jenkins
                    . .venv-jenkins/bin/activate
                    python -m pip install --upgrade pip
                    python -m pip install pytest
                '''
            }
        }

        stage('Build SFEPS') {
            steps {
                sh '''
                    # If running inside Docker (Jenkins container), build natively in this container.
                    if [ -f /.dockerenv ]; then
                        echo 'Detected running inside Docker container; building inside container'
                        if [ -x scripts/install_agent_deps.sh ]; then
                            scripts/install_agent_deps.sh || true
                        fi
                        cmake -S server -B server/build || (cat server/CMakeLists.txt && false)
                        cmake --build server/build --parallel || true
                    elif [ -n "${AGENT_DOCKER_IMAGE:-}" ] && command -v docker >/dev/null 2>&1; then
                        echo "Running build inside Docker image: ${AGENT_DOCKER_IMAGE}"
                        docker run --rm -v "$PWD":/workspace -w /workspace "${AGENT_DOCKER_IMAGE}" bash -lc "\
                            if [ -x scripts/install_agent_deps.sh ]; then scripts/install_agent_deps.sh || true; else echo 'no installer'; fi && \
                            cmake -S server -B server/build || (cat server/CMakeLists.txt && false) && \
                            cmake --build server/build --parallel || true"
                    else
                        echo 'Running build on agent host'
                        if [ -x scripts/install_agent_deps.sh ]; then
                            echo 'Running agent dependency installer (may require sudo)'
                            scripts/install_agent_deps.sh || true
                        else
                            echo 'scripts/install_agent_deps.sh not found or not executable; ensure dependencies are installed on agent'
                        fi
                        cmake -S server -B server/build || (cat server/CMakeLists.txt && false)
                        cmake --build server/build --parallel || true
                    fi
                '''
            }
        }

        stage('Build & Restart on Pi') {
            steps {
                withCredentials([sshUserPrivateKey(credentialsId: 'sfeps-ssh', keyFileVariable: 'SSH_KEY')]) {
                    sh """
                        REMOTE="iam@192.168.0.92"
                        REMOTE_WORKDIR="${params.SFEPS_PI_WORKDIR}"

                        ssh -i "\$SSH_KEY" -o StrictHostKeyChecking=no \$REMOTE "if [ ! -d \"\$REMOTE_WORKDIR/server\" ]; then echo 'Missing server dir:' \"\$REMOTE_WORKDIR/server\"; ls -la \"\$REMOTE_WORKDIR\" || true; exit 2; fi; cd \"\$REMOTE_WORKDIR\" && cmake -S server -B server/build && cmake --build server/build -j\$(nproc) && sudo -n systemctl restart sfeps-server"
                    """
                }
            }
        }

        stage('Start SFEPS') {
            when {
                expression { return params.SFEPS_PERF_ALERT_HOST == '127.0.0.1' }
            }
            steps {
                sh '''
                    # Prepare minimal .env for local-only mode so run_server.sh won't fail fast.
                    mkdir -p server
                    mkdir -p server
                    # Create dummy CA file so run_server.sh's readability check passes
                    echo '-----BEGIN CERTIFICATE-----\nMIID...dummy...\n-----END CERTIFICATE-----' > server/ca.crt || true
                    chmod 644 server/ca.crt || true
                    cat > server/.env <<'EOF'
SFEPS_DB_HOST=localhost
SFEPS_DB_USER=test
SFEPS_DB_PASS=test
SFEPS_DB_NAME_AUTH=test_auth
SFEPS_DB_NAME_ANALYTICS=test_analytics
SFEPS_APP_PLAINTEXT_ENABLE=1
SFEPS_APP_TLS_ENABLE=0
# Point RTSPS_TLS_CA to workspace-local CA file (relative to server dir)
RTSPS_TLS_CA=./ca.crt
EOF

                    # Start the real server in background and record its PID + logs.
                    (cd server && ./run_server.sh > ../reports/sfeps_server.log 2>&1) &
                    echo $! > reports/sfeps_server.pid || true

                    # Wait for alert TCP port and UDS socket to appear before running tests.
                    ALERT_PORT=${SFEPS_PERF_ALERT_PORT}
                    UDS_PATH=${SFEPS_PERF_RFID_SOCKET_PATH}
                    # wait up to 30s for TCP port
                    for i in $(seq 1 60); do
                        python3 - <<PY
import socket,sys
try:
    s=socket.create_connection(('127.0.0.1', int($ALERT_PORT)), timeout=1)
    s.close()
    print('ok')
    sys.exit(0)
except Exception:
    sys.exit(1)
PY
                        if [ $? -eq 0 ]; then break; fi
                        sleep 0.5
                    done
                    # wait up to SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC+2 for UDS
                    WAIT_SEC=$(python3 - <<PY
import os
print(int(float(os.getenv('SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC','10'))+2))
PY
)
                    for i in $(seq 1 $WAIT_SEC); do
                        if [ -e "$UDS_PATH" ]; then break; fi
                        sleep 1
                    done
                    ls -l "$UDS_PATH" || true
                    sleep 0.5
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
    }

    post {
        always {
            junit testResults: 'reports/pytest_tc_nf_perf_02.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'reports/**', allowEmptyArchive: true

            // Attempt to stop the started SFEPS server if PID file exists
            sh '''
                if [ -f reports/sfeps_server.pid ]; then
                    pid=$(cat reports/sfeps_server.pid) || true
                    if [ -n "${pid}" ]; then
                        echo "Stopping SFEPS server pid=${pid}"
                        kill ${pid} || true
                        # wait up to 10s
                        for i in $(seq 1 10); do
                            if kill -0 ${pid} >/dev/null 2>&1; then
                                sleep 1
                            else
                                break
                            fi
                        done
                        if kill -0 ${pid} >/dev/null 2>&1; then
                            echo "PID ${pid} still alive; forcing kill"
                            kill -9 ${pid} || true
                        fi
                    fi
                fi
                # Show last lines of server log for diagnostics
                if [ -f reports/sfeps_server.log ]; then
                    echo '===== SFEPS server log (tail) ====='
                    tail -n 200 reports/sfeps_server.log || true
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
            junit testResults: 'reports/*.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'tests/real_server.log,.ci-mediamtx.log,.ci-ffmpeg-publisher.log', allowEmptyArchive: true
        }
    }
}
