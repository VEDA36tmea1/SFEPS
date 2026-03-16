#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>
#include <QQmlContext>
#include <QWindow>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QProcessEnvironment>
#include <QString>
#include "authmanager.h"
#include "mainwindow.h"
#include "voicemanager.h"
#include "fraudmanager.h"
#include "positionmanager.h"

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    fprintf(stderr, "%s\n", qPrintable(msg));
    fflush(stderr);
    QFile file("C:/Users/2-08/Desktop/qt_client_ui/debug_output.txt");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ") << msg << "\n";
}

int main(int argc, char *argv[]) {
  qInstallMessageHandler(myMessageOutput);

  // 경고 방지와 커스텀 스타일 지원을 위해 "Basic" 스타일 강제 적용
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  QGuiApplication app(argc, argv);

  // 애플리케이션 메타데이터 설정
  app.setOrganizationName("HanwhaVision");
  app.setOrganizationDomain("hanwhavision.com");
  app.setApplicationName("SFEPS_Client");
  // resources.qrc에 포함된 실제 파일로 설정
  app.setWindowIcon(QIcon(":/assets/SFEPS_Logo.png"));

  // QML 타입 등록
  qmlRegisterType<MainWindow>("src.backend", 1, 0, "VideoDisplay");

  QQmlApplicationEngine engine;

  // AuthManager를 컨텍스트 속성으로 등록 (싱글톤처럼 사용)
  AuthManager authManager;
  engine.rootContext()->setContextProperty("authManager", &authManager);

  // VoiceManager를 컨텍스트 속성으로 등록
  VoiceManager voiceManager;
  engine.rootContext()->setContextProperty("voiceManager", &voiceManager);

  // FraudManager를 컨텍스트 속성으로 등록
  FraudManager fraudManager;
  engine.rootContext()->setContextProperty("fraudManager", &fraudManager);

    // PositionManager를 컨텍스트 속성으로 등록 (포지션/트래킹 전용)
    PositionManager positionManager;
    engine.rootContext()->setContextProperty("positionManager", &positionManager);

  // 알림 서버 호스트: 환경변수 FRAUD_SERVER_HOST가 설정되어 있으면 그 값을 사용하고,
  // 설정되어 있지 않으면 기존 하드코드된 주소를 기본값으로 사용합니다.
  const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  auto parseEnvBool = [](const QProcessEnvironment &e, const QString &key, bool defaultValue) {
      const QString raw = e.value(key).trimmed().toLower();
      if (raw.isEmpty()) return defaultValue;
      if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
      if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
      return defaultValue;
  };
  auto parseEnvPort = [](const QProcessEnvironment &e, const QString &key, int defaultValue) {
      const QString raw = e.value(key).trimmed();
      if (raw.isEmpty()) return defaultValue;
      bool ok = false;
      const int parsed = raw.toInt(&ok);
      if (!ok || parsed < 1 || parsed > 65535) return defaultValue;
      return parsed;
  };

  const QString alertHost = env.value("FRAUD_SERVER_HOST", "192.168.0.101");
  const bool clientTlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
  const bool alertTlsEnabled = parseEnvBool(env, "SFEPS_ALERT_TLS_ENABLE", clientTlsEnabled);
  const int alertPort = alertTlsEnabled
                            ? parseEnvPort(env, "SFEPS_ALERT_TLS_PORT", 6557)
                            : parseEnvPort(env, "FRAUD_SERVER_PORT", 5557);
  qDebug() << "[Main] Fraud alert server:" << alertHost << ":" << alertPort
           << (alertTlsEnabled ? "(TLS)" : "(Plain)");
  fraudManager.connectToServer(alertHost, alertPort);

    // Position channel (separate socket) for SUB_POS/UNSUB_POS and POS events
    const QString posHost = env.value("POS_SERVER_HOST", alertHost);
    const bool posTlsEnabled = parseEnvBool(env, "SFEPS_POS_TLS_ENABLE", false);
    const int posPort = posTlsEnabled
                                                        ? parseEnvPort(env, "SFEPS_POS_TLS_PORT", 6558)
                                                        : parseEnvPort(env, "POS_SERVER_PORT", 5558);
    qDebug() << "[Main] Position server:" << posHost << ":" << posPort
                     << (posTlsEnabled ? "(TLS)" : "(Plain)");
    positionManager.connectPositionServer(posHost, posPort);

    // Expose RTSP stream URL to QML so QML MediaPlayer can use it
    const QString rtspStreamUrl = QProcessEnvironment::systemEnvironment().value("RTSP_STREAM_URL", "rtsp://192.168.0.101:8554/cam1");
    engine.rootContext()->setContextProperty("rtspStreamUrl", rtspStreamUrl);

    // Auto-subscribe helper for testing: if SFEPS_AUTO_SUB_POS_ID env var is set,
    // send a SUB_POS|<id> once after connecting.
    const QString autoSubId = env.value("SFEPS_AUTO_SUB_POS_ID", "").trimmed();
    if (!autoSubId.isEmpty()) {
        qDebug() << "[Main] Auto SUB_POS enabled for id:" << autoSubId;
        QTimer::singleShot(1000, [&positionManager, autoSubId]() {
            positionManager.sendPositionCommand(QString("SUB_POS|%1").arg(autoSubId));
        });
    }

  // QML 파일 URL 정의
  const QUrl loginUrl(QStringLiteral("qrc:/src/views/LoginView.qml"));
  const QUrl mainUrl(QStringLiteral("qrc:/src/Main.qml"));

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [loginUrl, mainUrl](QObject *obj, const QUrl &objUrl) {
        // 로드 실패 시 종료 (LoginWindow 또는 MainWindow 중 하나라도 실패하면)
        if (!obj && (objUrl == loginUrl || objUrl == mainUrl))
          QCoreApplication::exit(-1);
      },
      Qt::QueuedConnection);

  // 로그인 성공 시 메인 창으로 전환
  QObject::connect(&authManager, &AuthManager::loginSuccess, [&engine, mainUrl](){
      // 1. 메인 윈도우 로드 (앱 종료 방지를 위해 먼저 로드)
      engine.load(mainUrl);
      
      // 2. 기존 로그인 윈도우 닫기
      // 루트 객체들 중 타이틀이 "SFEPS Login"인 윈도우를 찾아 닫습니다.
      const auto rootObjects = engine.rootObjects();
      for (auto obj : rootObjects) {
          QWindow *win = qobject_cast<QWindow*>(obj);
          if (win && win->title() == "SFEPS Login") {
              win->close();
              win->deleteLater();
          }
      }
  });

  // 필요한 경우 모듈 임포트 경로 추가
  engine.addImportPath(app.applicationDirPath() + "/plugins");
  // 리소스 기반 디렉토리(src/, src/views/)를 임포트 경로에 추가
  // 예: import src 1.0, import src.views 1.0
  engine.addImportPath(QStringLiteral("qrc:/"));

  // 초기 화면(로그인 창) 로드
  engine.load(loginUrl);

  return app.exec();
}
