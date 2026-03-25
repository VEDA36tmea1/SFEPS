#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>
#include <QQmlContext>
#include <QWindow>
#include <QTimer>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QString>
#include "authmanager.h"
#include "mainwindow.h"
#include "voicemanager.h"
#include "fraudmanager.h"
#include "positionmanager.h"
#include "videoarchivemanager.h"
#include "recordinglistmodel.h"

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(type);
    Q_UNUSED(context);
    fprintf(stderr, "%s\n", qPrintable(msg));
    fflush(stderr);
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

  QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &warnings) {
      for (const QQmlError &warning : warnings) {
          qWarning().noquote() << "[QML Warning]" << warning.toString();
      }
  });

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

    // Video archive manager + recording list model
    VideoArchiveManager videoArchiveManager;
    RecordingListModel recordingListModel;
    engine.rootContext()->setContextProperty("videoArchiveManager", &videoArchiveManager);
    engine.rootContext()->setContextProperty("recordingListModel", &recordingListModel);

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

  const QString alertHost = env.value("FRAUD_SERVER_HOST", "192.168.0.82");
  const bool clientTlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
  const bool alertTlsEnabled = parseEnvBool(env, "SFEPS_ALERT_TLS_ENABLE", clientTlsEnabled);
  const int alertPort = alertTlsEnabled
                            ? parseEnvPort(env, "SFEPS_ALERT_TLS_PORT", 6557)
                            : parseEnvPort(env, "FRAUD_SERVER_PORT", 5557);
  qDebug() << "[Main] Fraud alert server:" << alertHost << ":" << alertPort
           << (alertTlsEnabled ? "(TLS)" : "(Plain)");
  fraudManager.connectToServer(alertHost, alertPort);

  // Server-down detection and forced-logout handling
  QTimer *serverDownTimer = new QTimer(&app);
  serverDownTimer->setSingleShot(true);
  serverDownTimer->setInterval(5000); // 5 seconds
  bool isForceLogoutInProgress = false;

  auto performForcedLogout = [&](const QString &reason) {
      if (isForceLogoutInProgress) {
          qDebug() << "[Main] forced logout already in progress, ignoring trigger:" << reason;
          return;
      }
      isForceLogoutInProgress = true;
      qDebug() << "[Main] performing forced logout (reason):" << reason;
      // Ensure Position socket is closed and clear current user, then ask UI to show forced-logout notice.
      positionManager.disconnectPositionServer();
      authManager.clearCurrentUser();
      QString notice = QStringLiteral("서버와의 네트워크 연결이 끊어져 강제 로그아웃됩니다.");
      if (reason == "force_logout_event") {
          notice = QStringLiteral("서버 정책에 의해 강제 로그아웃됩니다.");
      }
      authManager.requestForcedLogout(notice);
  };

  QObject::connect(serverDownTimer, &QTimer::timeout, [&]() {
      performForcedLogout("server_down_timeout");
  });

  // Start server-down timer when either alert or position disconnects
  QObject::connect(&fraudManager, &FraudManager::serverDisconnected, [&]() {
      qDebug() << "[Main] fraudManager disconnected -> starting serverDownTimer";
      if (!serverDownTimer->isActive()) serverDownTimer->start();
  });
  QObject::connect(&positionManager, &PositionManager::positionDisconnected, [&]() {
      qDebug() << "[Main] positionManager disconnected -> starting serverDownTimer";
      if (!serverDownTimer->isActive()) serverDownTimer->start();
  });

  // Cancel server-down timer when connections are restored
  QObject::connect(&fraudManager, &FraudManager::serverConnected, [&]() {
      if (serverDownTimer->isActive()) {
          qDebug() << "[Main] fraudManager reconnected -> cancelling serverDownTimer";
          serverDownTimer->stop();
      }
  });
  QObject::connect(&positionManager, &PositionManager::positionConnected, [&]() {
      if (serverDownTimer->isActive()) {
          qDebug() << "[Main] positionManager reconnected -> cancelling serverDownTimer";
          serverDownTimer->stop();
      }
  });

  // Immediate forced logout when server sends AUTH|FORCE_LOGOUT
  QObject::connect(&fraudManager, &FraudManager::forceLogoutEvent, [&](const QString &raw) {
      qDebug() << "[Main] forceLogoutEvent received:" << raw;
      performForcedLogout("force_logout_event");
  });

    // Position channel (separate socket) for SUB_POS/UNSUB_POS and POS events
    const QString posHost = env.value("POS_SERVER_HOST", alertHost);
    const bool posTlsEnabled = parseEnvBool(env, "SFEPS_POS_TLS_ENABLE", false);
    const int posPort = posTlsEnabled
                                                        ? parseEnvPort(env, "SFEPS_POS_TLS_PORT", 6558)
                                                        : parseEnvPort(env, "POS_SERVER_PORT", 5558);
    qDebug() << "[Main] Position server:" << posHost << ":" << posPort
                     << (posTlsEnabled ? "(TLS)" : "(Plain)");
    // Position connection is started after successful login to avoid unauthenticated connects.

    bool authLoginSucceeded = false;
    bool alertLoginAckReceived = false;
    bool positionConnectIssued = false;
    QTimer *positionConnectFallbackTimer = new QTimer(&app);
    positionConnectFallbackTimer->setSingleShot(true);
    positionConnectFallbackTimer->setInterval(2000);

    auto tryStartPositionConnection = [&](bool allowWithoutAck = false) {
            if (positionConnectIssued) return;
            if (!authLoginSucceeded) return;
            if (!alertLoginAckReceived && !allowWithoutAck) return;
            positionConnectIssued = true;
            if (allowWithoutAck && !alertLoginAckReceived) {
                qWarning() << "[Main] login ACK not received in time; starting Position connection with fallback";
            } else {
                qDebug() << "[Main] login+ack ready: initiating Position connection to" << posHost << posPort;
            }
            positionManager.connectPositionServer(posHost, posPort);
    };

    QObject::connect(positionConnectFallbackTimer, &QTimer::timeout, [&]() {
        tryStartPositionConnection(true);
    });

    // Expose RTSP stream URL to QML so QML MediaPlayer can use it
    const QString rtspStreamUrl = QProcessEnvironment::systemEnvironment().value("RTSP_STREAM_URL", "rtsp://192.168.0.82:8554/cam1");
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
                if (!obj && (objUrl == loginUrl || objUrl == mainUrl)) {
                    qCritical() << "[Main] Failed to create root object:" << objUrl;
          QCoreApplication::exit(-1);
                }
      },
      Qt::QueuedConnection);

  // 로그인 성공 시 메인 창으로 전환
  QObject::connect(&authManager, &AuthManager::loginSuccess, [&engine, mainUrl](){
      qInfo() << "[Main] loginSuccess received, loading main window:" << mainUrl;
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

    // Position connection gate: prefer server TEST|LOGIN_OK ack, with timed fallback for recovery.
  QObject::connect(&authManager, &AuthManager::loginSuccess, [&]() {
      authLoginSucceeded = true;
      alertLoginAckReceived = false;
      positionConnectIssued = false;
      tryStartPositionConnection(false);
      positionConnectFallbackTimer->start();
  });

  QObject::connect(&fraudManager, &FraudManager::loginAckReceived, [&](const QString &userId) {
      Q_UNUSED(userId);
      alertLoginAckReceived = true;
      if (positionConnectFallbackTimer->isActive()) {
          positionConnectFallbackTimer->stop();
      }
      tryStartPositionConnection(false);
  });

  // When logout is requested, close main windows and show Login view again
    QObject::connect(&authManager, &AuthManager::logoutRequested, [&engine, loginUrl, &positionManager, &authLoginSucceeded, &alertLoginAckReceived, &positionConnectIssued, positionConnectFallbackTimer](){
      qDebug() << "[Main] logoutRequested: closing main windows, disconnecting Position and loading login view";
      authLoginSucceeded = false;
      alertLoginAckReceived = false;
      positionConnectIssued = false;
            if (positionConnectFallbackTimer->isActive()) {
                    positionConnectFallbackTimer->stop();
            }
      // Ensure Position socket is closed so server stops sending POS events
      positionManager.disconnectPositionServer();
      const auto rootObjects = engine.rootObjects();
      for (auto obj : rootObjects) {
          QWindow *win = qobject_cast<QWindow*>(obj);
          if (win && win->title() == "Hanwha Vision SFEPS") {
              win->close();
              win->deleteLater();
          }
      }
      engine.load(loginUrl);
  });

  // On app exit, try to notify auth server to logout so server can immediately cleanup sessions
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&authManager]() {
      qDebug() << "[Main] aboutToQuit: sending logout";
      authManager.sendLogout();
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