pragma Singleton
import QtQuick 2.15

QtObject {
    // CCTV Management Dashboard (src/styles/theme.css) 기준 토큰
    readonly property color background: "#0a0a0a"      // app 배경(React 코드에서 사용)
    readonly property color surface: "#1a1a1a"         // sidebar/surface
    readonly property color surfaceCard: "#262626"     // card
    readonly property color surfaceCardAlt: "#1f1f1f"  // login card 등

    readonly property color accent: "#ff6b2c"          // primary
    readonly property color accentHover: "#ff8555"
    // 기존 코드 호환(이전 Designer 토큰명 사용처)
    readonly property color primaryOrange: accent
    readonly property color primaryOrangeHover: accentHover

    readonly property color textPrimary: "#ffffff"
    readonly property color textSecondary: "#a3a3a3"
    readonly property color textMuted: "#a3a3a3"
    readonly property color textDisabled: "#4b5563"

    readonly property color borderSub: "#333333"
    readonly property color borderCard: "#333333"
    readonly property color inputBg: "#2a2a2a"
    readonly property color inputBorder: "#404040"

    readonly property color navBarBg: "#0a0a0a"
    readonly property color sidebarBg: "#1a1a1a"
    readonly property color statusOnline: "#22c55e"
    readonly property color statusOffline: "#ef4444"

    readonly property font fontMain: Qt.font({
        family: "Inter",
        pixelSize: 14,
        weight: Font.Normal
    })

    readonly property font fontHeader: Qt.font({
        family: "Inter",
        pixelSize: 18,
        weight: Font.Bold
    })

    readonly property font fontTitle: Qt.font({
        family: "Inter",
        pixelSize: 24,
        weight: Font.Bold
    })
}
