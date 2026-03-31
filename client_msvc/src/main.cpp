#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>
#include <QQmlContext>
#include <QWindow>
#include <QTimer>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QProcessEnvironment>
#include <QString>
#include <QVariant>
#include "authmanager.h"
#include "mainwindow.h"
#include "voicemanager.h"
#include "fraudmanager.h"
#include "positionmanager.h"
#include "pwmtransmitter.h"
#include "videoarchivemanager.h"
#include "recordinglistmodel.h"
#ifdef SFEPS_HAVE_OPENCV
#include "live_frame_provider.h"
#endif

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

    // PwmTransmitter: camera_RBF에서 수신한 PWM 값을 하드웨어로 송신
    // 환경변수: SFEPS_PWM_MODE (raspi|stm|both), SFEPS_PWM_HOST, SFEPS_PWM_PORT
    PwmTransmitter pwmTransmitter;
    engine.rootContext()->setContextProperty("pwmTransmitter", &pwmTransmitter);

    // VideoArchiveManager: 녹화 파일 카탈로그 및 재생 관리
    VideoArchiveManager videoArchiveManager;
    engine.rootContext()->setContextProperty("videoArchiveManager", &videoArchiveManager);

    // RecordingListModel: QML ListView에서 사용하는 녹화 목록 모델
    RecordingListModel recordingListModel;
    engine.rootContext()->setContextProperty("recordingListModel", &recordingListModel);

  // Video backend: ONVIF metadata + optional OpenCV RTSP preview + native track + RBF/PWM
  MainWindow videoBackend;
  engine.rootContext()->setContextProperty("videoBackend", &videoBackend);
#ifdef SFEPS_HAVE_OPENCV
  auto *liveFrameProvider = new LiveFrameProvider();
  engine.addImageProvider(QStringLiteral("live"), liveFrameProvider);
  videoBackend.setLiveFrameProvider(liveFrameProvider);
  // RBF 계산된 PWM → PwmTransmitter (Raspberry Pi / ESP8266) 직접 전송
  QObject::connect(&videoBackend, &MainWindow::pwmSetRequested,
                   [&pwmTransmitter, &videoBackend](int pan, int tilt) {
#ifdef SFEPS_HAVE_OPENCV
                       if (!videoBackend.laserTrackingEnabled()) return;
#endif
                       pwmTransmitter.sendPwm(pan, tilt);
                   });
  // 동시에 PositionManager를 통해 서버(5558)에도 통보 (모니터링/로깅 용도)
  QObject::connect(&videoBackend, &MainWindow::pwmSetRequested, &positionManager,
                   [&positionManager, &videoBackend](int pan, int tilt) {
#ifdef SFEPS_HAVE_OPENCV
                       if (!videoBackend.laserTrackingEnabled()) return;
#endif
                       positionManager.sendPositionCommand(
                           QStringLiteral("SET_PWM,PAN=%1,TILT=%2").arg(pan).arg(tilt));
                   });
#endif

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

  // ── PwmTransmitter 초기화 ───────────────────────────────────────────────
  // SFEPS_PWM_MODE : "raspi" (TCP) | "stm" (ESP8266 UDP) | "both" (동시 전송)
  // SFEPS_PWM_HOST/SFEPS_PWM_PORT: 1차 타겟 (raspi/both에서는 Raspberry Pi TCP)
  // SFEPS_PWM_STM_HOST/SFEPS_PWM_STM_PORT: both 모드의 2차 타겟 (ESP8266 UDP)
  // SFEPS_PWM_STM_TRANSPORT: "udp"(기본) | "tcp" (ESP8266 AP + TCP 서버 사용 시)
  {
      const QString pwmMode = env.value("SFEPS_PWM_MODE", "raspi").trimmed();
      const QString pwmHost = env.value("SFEPS_PWM_HOST", "192.168.0.100").trimmed();
      const int     pwmPort = parseEnvPort(env, "SFEPS_PWM_PORT", 5566);
      const QString pwmStmHost = env.value("SFEPS_PWM_STM_HOST", "192.168.4.1").trimmed();
      const int     pwmStmPort = parseEnvPort(env, "SFEPS_PWM_STM_PORT", 4210);
      const QString pwmStmTransport = env.value("SFEPS_PWM_STM_TRANSPORT", "udp").trimmed();
      qDebug() << "[Main] PwmTransmitter mode=" << pwmMode
               << " host=" << pwmHost << " port=" << pwmPort;
      pwmTransmitter.setMode(pwmMode);
      pwmTransmitter.setStmTransport(pwmStmTransport);
      if (pwmMode.compare("both", Qt::CaseInsensitive) == 0) {
          qDebug() << "[Main] PwmTransmitter secondary(ESP8266)=" << pwmStmHost << ":" << pwmStmPort;
          pwmTransmitter.connectSecondaryTarget(pwmStmHost, pwmStmPort);
      }
      pwmTransmitter.connectTarget(pwmHost, pwmPort);
  }

  // camera_RBF.cpp --qt-mode 에서 역방향으로 수신한 PWM → PwmTransmitter로 송신
  QObject::connect(&positionManager, &PositionManager::pwmReceived,
                   [&pwmTransmitter, &videoBackend](int pan, int tilt) {
#ifdef SFEPS_HAVE_OPENCV
                       if (!videoBackend.laserTrackingEnabled()) return;
#endif
                       pwmTransmitter.sendPwm(pan, tilt);
                   });

  // 부정승차(isFraud)마다 XML ID를 세트에 넣어 빨간 bbox (추적 중이어도 processFraud가 항상 emit)
  QObject::connect(&fraudManager, &FraudManager::fraudDetected,
                   [&videoBackend](const QString &objectId,
                                   const QString & /*cardAgeText*/,
                                   const QString & /*age*/,
                                   bool isFraud,
                                   const QString & /*tag*/,
                                   const QString & /*imagePath*/) {
                       if (isFraud && !objectId.isEmpty())
                           videoBackend.addFraudXmlId(objectId);
                   });

  // 자동 추적 요청: 큐에 넣지 않은 경우(또는 drain 시)에만 emit → trackByXmlId
  // addFraudXmlId는 위 fraudDetected에서 처리 (추적 중 추가 FRAUD도 빨간색 반영)
  QObject::connect(&fraudManager, &FraudManager::fraudAutoTrackRequest,
                   [&videoBackend](const QString &xmlId,
                                   float bboxL, float bboxT, float bboxR, float bboxB) {
                       qDebug() << "[Main] fraud auto-track request xmlId=" << xmlId
                                << "fallback=(" << bboxL << bboxT << bboxR << bboxB << ")";
                       videoBackend.trackByXmlId(xmlId, bboxL, bboxT, bboxR, bboxB);
                   });

  // videoBackend tracking 상태 변화 → FraudManager 대기큐 동기화
  // trackByXmlId 호출 시 xmlId emit, clearRbfTarget 호출 시 "" emit
  QObject::connect(&videoBackend, &MainWindow::trackingXmlIdChanged,
                   [&fraudManager](const QString &xmlId) {
                       fraudManager.setActiveTrackingId(xmlId);
                   });

#ifdef CAMERA_RBF_QT_MODE
  // 자동 레이저 대상이 화면 밖으로 나가 추적을 중지했을 때,
  // Position server에 TRACK_END를 보내서 해당 객체 구독/추적을 종료한다.
  QObject::connect(&videoBackend, &MainWindow::laserTrackStopped,
                   [&positionManager, &videoBackend](const QString &xmlId) {
                       if (xmlId.isEmpty()) return;
                       positionManager.sendPositionCommand(QStringLiteral("TRACK_END|%1").arg(xmlId));
                   // 빨간 bbox(fraud 표시)는 TRACK_END만으로는 안 꺼질 수 있어,
                   // 추적 종료된 xmlId를 fraud 목록에서도 제거한다.
#ifdef SFEPS_HAVE_OPENCV
                   if (videoBackend.laserTrackingEnabled()) {
                       videoBackend.removeFraudXmlId(xmlId);
                   }
#endif
                   });

  // 자동/수동 구분 없이 laserTrackStopped → 라즈베리파이에 TRACK_END 전달
  QObject::connect(&videoBackend, &MainWindow::laserTrackStopped,
                   &pwmTransmitter, &PwmTransmitter::sendTrackEnd);
#endif

  // 앱 종료 시: 현재 추적 중인 객체가 있으면 TRACK_END 한 번 보내고 정리
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
      const QString xmlId = fraudManager.activeTrackingId();
      if (xmlId.isEmpty())
          return;
      qDebug() << "[Main] aboutToQuit: sending TRACK_END for activeTrackingId=" << xmlId;
      positionManager.sendPositionCommand(QStringLiteral("TRACK_END|%1").arg(xmlId));
      pwmTransmitter.sendTrackEnd(xmlId);
  });

  const QString alertHost = env.value("FRAUD_SERVER_HOST", "192.168.0.101");
  const bool directStreamMode = parseEnvBool(env, "SFEPS_DIRECT_STREAM_MODE", false);
  const bool clientTlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
  const bool alertTlsEnabled = parseEnvBool(env, "SFEPS_ALERT_TLS_ENABLE", clientTlsEnabled);
  const int alertPort = alertTlsEnabled
                            ? parseEnvPort(env, "SFEPS_ALERT_TLS_PORT", 6557)
                            : parseEnvPort(env, "FRAUD_SERVER_PORT", 5557);
  qDebug() << "[Main] Fraud alert server:" << alertHost << ":" << alertPort
           << (alertTlsEnabled ? "(TLS)" : "(Plain)");
  if (!directStreamMode) {
      fraudManager.connectToServer(alertHost, alertPort);
  } else {
      qWarning() << "[Main] SFEPS_DIRECT_STREAM_MODE=1 -> skip auth/fraud/position server connections";
  }

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
  if (!directStreamMode) {
      QObject::connect(&fraudManager, &FraudManager::serverDisconnected, [&]() {
          qDebug() << "[Main] fraudManager disconnected -> starting serverDownTimer";
          if (!serverDownTimer->isActive()) serverDownTimer->start();
      });
      QObject::connect(&positionManager, &PositionManager::positionDisconnected, [&]() {
          qDebug() << "[Main] positionManager disconnected -> starting serverDownTimer";
          if (!serverDownTimer->isActive()) serverDownTimer->start();
      });
  }

  // Cancel server-down timer when connections are restored
  if (!directStreamMode) {
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
  }

  // Immediate forced logout when server sends AUTH|FORCE_LOGOUT
  if (!directStreamMode) {
      QObject::connect(&fraudManager, &FraudManager::forceLogoutEvent, [&](const QString &raw) {
          qDebug() << "[Main] forceLogoutEvent received:" << raw;
          performForcedLogout("force_logout_event");
      });
  }

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
    const QString rtspStreamUrl = QProcessEnvironment::systemEnvironment().value("RTSP_STREAM_URL", "rtsp://192.168.0.101:8554/cam1");
    engine.rootContext()->setContextProperty("rtspStreamUrl", rtspStreamUrl);

    // FFmpeg backend: MediaPlayer.playbackOptions.probeSize (bytes). Smaller = less pre-roll / latency;
    // too small may fail to open some streams. Default 65536. Set RTSP_MEDIA_PROBE_SIZE=-1 for Qt default.
    qint64 rtspMediaProbeSize = 65536;
    const QString probeEnv = env.value(QStringLiteral("RTSP_MEDIA_PROBE_SIZE")).trimmed();
    if (!probeEnv.isEmpty()) {
        bool ok = false;
        const qint64 v = probeEnv.toLongLong(&ok);
        if (ok) {
            if (v == -1)
                rtspMediaProbeSize = -1; // Qt/FFmpeg backend default probesize
            else if (v > 0)
                rtspMediaProbeSize = v;
        }
    }
    engine.rootContext()->setContextProperty(QStringLiteral("rtspMediaProbeSize"), QVariant::fromValue(rtspMediaProbeSize));
    qDebug() << "[Main] RTSP_MEDIA_PROBE_SIZE (bytes):" << rtspMediaProbeSize;

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
  if (!directStreamMode) {
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
  }

    // Position connection gate: prefer server TEST|LOGIN_OK ack, with timed fallback for recovery.
  if (!directStreamMode) {
      QObject::connect(&authManager, &AuthManager::loginSuccess, [&]() {
          authLoginSucceeded = true;
          alertLoginAckReceived = false;
          positionConnectIssued = false;
          tryStartPositionConnection(false);
          positionConnectFallbackTimer->start();
      });
  }

  if (!directStreamMode) {
      QObject::connect(&fraudManager, &FraudManager::loginAckReceived, [&](const QString &userId) {
          Q_UNUSED(userId);
          alertLoginAckReceived = true;
          if (positionConnectFallbackTimer->isActive()) {
              positionConnectFallbackTimer->stop();
          }
          tryStartPositionConnection(false);
      });
  }

  // When logout is requested, close main windows and show Login view again
  if (!directStreamMode) {
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
  }

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

  // 초기 화면 로드
  if (directStreamMode) {
      engine.load(mainUrl);
  } else {
      engine.load(loginUrl);
  }

  return app.exec();
}