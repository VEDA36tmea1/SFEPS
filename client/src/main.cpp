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
#include "authmanager.h"
#include "mainwindow.h"
#include "voicemanager.h"
#include "fraudmanager.h"

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
  fraudManager.connectToServer("192.168.0.89", 5557);

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
