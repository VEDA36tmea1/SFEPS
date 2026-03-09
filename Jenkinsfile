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

        stage('Run Login Tests') {
            steps {
                sh '''
                    set -eu
                    mkdir -p reports
                    python3 -m pytest -q tests/test_tc_func_login.py -r a --junitxml=reports/login-tests.xml
                '''
            }
        }
    }

    post {
        always {
            junit testResults: 'reports/login-tests.xml', allowEmptyResults: true
            archiveArtifacts artifacts: 'tests/real_server.log', allowEmptyArchive: true
        }
    }
}
