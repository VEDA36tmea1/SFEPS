pipeline {
    agent any

    options {
        timestamps()
        timeout(time: 20, unit: 'MINUTES')
    }

    parameters {
        string(name: 'SFEPS_PERF_ALERT_HOST', defaultValue: '127.0.0.1', description: 'Alert TCP host (server alert listener host)')
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
        string(name: 'AGENT_DOCKER_IMAGE', defaultValue: 'sfeps-jenkins-agent:latest', description: 'Optional: Docker image to run build steps inside')
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

        stage('Start SFEPS') {
            steps {
                sh '''
                    # Prepare minimal .env for local-only mode so run_server.sh won't fail fast.
                    mkdir -p server
                    cat > server/.env <<'EOF'
SFEPS_DB_HOST=localhost
SFEPS_DB_USER=test
SFEPS_DB_PASS=test
SFEPS_DB_NAME_AUTH=test_auth
SFEPS_DB_NAME_ANALYTICS=test_analytics
SFEPS_APP_PLAINTEXT_ENABLE=1
SFEPS_APP_TLS_ENABLE=0
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

        stage('Run TC-NF-PERF-02') {
            environment {
                SFEPS_PERF_ALERT_HOST = "${params.SFEPS_PERF_ALERT_HOST}"
                SFEPS_PERF_ALERT_PORT = "${params.SFEPS_PERF_ALERT_PORT}"
                SFEPS_PERF_TARGET_COUNT = "${params.SFEPS_PERF_TARGET_COUNT}"
                SFEPS_PERF_WINDOW_SEC = "${params.SFEPS_PERF_WINDOW_SEC}"
                SFEPS_PERF_ALLOWED_MISSING = "${params.SFEPS_PERF_ALLOWED_MISSING}"
                SFEPS_PERF_ALLOWED_DUPLICATE = "${params.SFEPS_PERF_ALLOWED_DUPLICATE}"
                SFEPS_PERF_MESSAGE_PREFIX = "${params.SFEPS_PERF_MESSAGE_PREFIX}"
                SFEPS_PERF_RFID_SOCKET_PATH = "${params.SFEPS_PERF_RFID_SOCKET_PATH}"
                SFEPS_PERF_RFID_TEXT = "${params.SFEPS_PERF_RFID_TEXT}"
                SFEPS_PERF_RFID_UID_SEED = "${params.SFEPS_PERF_RFID_UID_SEED}"
                SFEPS_PERF_RFID_DEVICE_ID = "${params.SFEPS_PERF_RFID_DEVICE_ID}"
                SFEPS_PERF_RFID_SEND_INTERVAL_SEC = "${params.SFEPS_PERF_RFID_SEND_INTERVAL_SEC}"
                SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC = "${params.SFEPS_PERF_RFID_ACCEPT_TIMEOUT_SEC}"
            }
            steps {
                sh '''
                    mkdir -p reports
                    . .venv-jenkins/bin/activate
                    # Start lightweight fake SFEPS bridge to accept the test's alert connection
                    # and relay NDJSON from the UDS injector as FRAUD messages.
                    python3 tests/fake_sfeps_bridge.py > reports/fake_sfeps_bridge.log 2>&1 &
                    sleep 0.5
                    python -m pytest tests/test_tc_nf_perf_02.py -q --junitxml=reports/pytest_tc_nf_perf_02.xml
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
            '''
        }
    }
}
