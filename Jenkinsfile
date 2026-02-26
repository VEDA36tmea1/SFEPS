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
                    python -m pytest tests/test_tc_nf_perf_02.py -q --junitxml=reports/pytest_tc_nf_perf_02.xml
                '''
            }
        }
    }

    post {
        always {
            junit testResults: 'reports/pytest_tc_nf_perf_02.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'reports/**', allowEmptyArchive: true
        }
    }
}
