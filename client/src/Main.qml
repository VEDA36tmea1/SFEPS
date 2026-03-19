import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0
import src.views 1.0

Window {
    id: rootWindow
    width: 1250
    height: 750
    visible: true
    title: "Hanwha Vision SFEPS"
    color: AppTheme.background

    property int currentViewIndex: 1  // 1: Live View, 2: Analytics, 3: Settings
    property int unreadCount: 0
    property bool laserTrackingEnabled: true
    property string forcedLogoutMessage: "서버와의 네트워크 연결이 끊어져 강제 로그아웃됩니다."
    property int forcedLogoutSecondsRemaining: 3

    ListModel {
        id: notificationModel
    }

    property int currentNotificationIndex: -1

    Popup {
        id: detailPopup
        anchors.centerIn: parent
        width: 1000
        height: 600
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0
        
        background: Rectangle {
            color: AppTheme.background
            radius: 12
            border.color: AppTheme.borderCard
            border.width: 1
            
            // Add a drop shadow effect or just a darker border for depth
            layer.enabled: true
        }

        property alias objectId: detailView.objectId
        property alias cardAgeText: detailView.cardAgeText
        property alias ageGroup: detailView.ageGroup
        property alias isFraud: detailView.isFraud

        DetailView {
            id: detailView
            anchors.fill: parent
            onCloseClicked: detailPopup.close()
            onConfirmClicked: {
                if (currentNotificationIndex >= 0 && currentNotificationIndex < notificationModel.count) {
                    notificationModel.remove(currentNotificationIndex)
                }
                currentNotificationIndex = -1
            }
        }
    }

    Popup {
        id: forcedLogoutPopup
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 460)
        height: 220
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        padding: 0

        background: Rectangle {
            color: AppTheme.surfaceCardAlt
            radius: 12
            border.color: AppTheme.borderCard
            border.width: 1
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            Text {
                text: "네트워크 연결 오류"
                color: "white"
                font.pixelSize: 20
                font.bold: true
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                text: forcedLogoutMessage
                color: AppTheme.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: 14
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                text: "자동 로그아웃까지 " + forcedLogoutSecondsRemaining + "초"
                color: AppTheme.accent
                font.pixelSize: 13
                font.bold: true
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.fillHeight: true }

            Button {
                id: forcedLogoutConfirmButton
                text: "확인"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 120
                Layout.preferredHeight: 40
                background: Rectangle {
                    radius: 8
                    color: forcedLogoutConfirmButton.hovered ? AppTheme.accentHover : AppTheme.accent
                }
                contentItem: Text {
                    text: parent.text
                    color: "white"
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    forcedLogoutTimer.stop()
                    forcedLogoutPopup.close()
                    authManager.notifyLocalLogout()
                }
            }
        }
    }

    // Main App (CCTV Management Dashboard 스타일: Sidebar + Top bar)
    RowLayout {
        anchors.fill: parent
        spacing: 0

            // Sidebar (Hidden as requested)
            Rectangle {
                visible: false
                Layout.preferredWidth: visible ? 208 : 0
                Layout.fillHeight: true
                color: AppTheme.sidebarBg

                // Right border
                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: AppTheme.borderCard
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Logo header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 72
                        color: "transparent"
                        // bottom border
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: AppTheme.borderCard
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            // Logo with Orange Background
                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                color: AppTheme.primaryOrange
                                radius: 8

                                Image {
                                    anchors.centerIn: parent
                                    width: 28
                                    height: 28
                                    source: "../assets/SFEPS_Logo_no_background.PNG"
                                    fillMode: Image.PreserveAspectFit
                                }
                            }

                            ColumnLayout {
                                spacing: 0
                                Text {
                                    text: "Hanwha Vision"
                                    color: "white"
                                    font.pixelSize: 14
                                    font.bold: true
                                }
                                Text {
                                    text: "SFEPS CONTROL"
                                    color: "#9ca3af"
                                    font.pixelSize: 10
                                    font.letterSpacing: 1
                                }
                            }
                        }
                    }

                    // Navigation
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.margins: 16
                        spacing: 8

                        Repeater {
                            model: [
                                {
                                    label: "Dashboard",
                                    idx: 1
                                },
                                {
                                    label: "Hardware Status",
                                    idx: 3
                                },
                                {
                                    label: "Evasion Analytics",
                                    idx: 2
                                },
                                {
                                    label: "System Settings",
                                    idx: 4
                                }
                            ]
                            delegate: Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                flat: true
                                checkable: true
                                checked: currentViewIndex === modelData.idx

                                contentItem: RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 10
                                    Text {
                                        text: modelData.label
                                        color: parent.checked ? "white" : "#9ca3af"
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                                background: Rectangle {
                                    color: parent.checked ? AppTheme.accent : (parent.hovered ? "#262626" : "transparent")
                                    radius: 8
                                }
                                onClicked: currentViewIndex = modelData.idx
                            }
                        }
                    }

                    // Profile footer (간단 버전)
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 72
                        color: "transparent"
                        Rectangle {
                            anchors.top: parent.top
                            width: parent.width
                            height: 1
                            color: AppTheme.borderCard
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12
                            Rectangle {
                                width: 40
                                height: 40
                                radius: 20
                                color: "#2563eb"
                                Text {
                                    anchors.centerIn: parent
                                    text: "JD"
                                    color: "white"
                                    font.bold: true
                                }
                            }
                            ColumnLayout {
                                spacing: 0
                                Text {
                                    text: authManager.currentUserId || "Admin"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.bold: true
                                }
                                Text {
                                    text: "Senior Admin"
                                    color: "#9ca3af"
                                    font.pixelSize: 10
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            // Bottom Glyph Button (To Monitoring)
                            Button {
                                Layout.preferredWidth: 32
                                Layout.preferredHeight: 32
                                flat: true
                                background: Rectangle {
                                    color: parent.hovered ? "#333" : "transparent"
                                    radius: 4
                                }
                                contentItem: Image {
                                    anchors.centerIn: parent
                                    source: "../assets/glyph.svg"
                                    sourceSize: Qt.size(20, 20)
                                }
                                onClicked: currentViewIndex = 1 // Go to Dashboard/Monitoring
                            }
                        }
                    }
                }
            }

            // Main content area
            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Top Navigation Bar
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    color: AppTheme.navBarBg
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: AppTheme.borderCard
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 24
                        anchors.rightMargin: 24
                        spacing: 16

                        ColumnLayout {
                            spacing: 0
                            Text {
                                text: "Hanwha Vision SFEPS"
                                color: "white"
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: "Central Monitoring Hub"
                                color: "#6b7280"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                            }
                        }

                        // Tabs (Dashboard / Analytics / Settings)
                        RowLayout {
                            spacing: 8
                            Repeater {
                                model: [
                                    { label: "Dashboard", idx: 1 },
                                    { label: "Analytics", idx: 2 },
                                    { label: "System Settings", idx: 3 }
                                ]
                                delegate: Button {
                                    flat: true
                                    checkable: true
                                    checked: currentViewIndex === modelData.idx
                                    contentItem: Text {
                                        text: modelData.label
                                        color: parent.checked ? AppTheme.accent : "#9ca3af"
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    background: Rectangle {
                                        color: "transparent"
                                        Rectangle {
                                            anchors.bottom: parent.bottom
                                            width: parent.width
                                            height: parent.checked ? 2 : 0
                                            color: AppTheme.accent
                                        }
                                    }
                                    onClicked: currentViewIndex = modelData.idx
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        // Right actions (status + bell)
                        Rectangle {
                            Layout.preferredHeight: 28
                            implicitWidth: 150
                            color: "#22c55e1a"
                            radius: 999
                            RowLayout {
                                anchors.centerIn: parent
                                spacing: 8
                                Rectangle {
                                    width: 8; height: 8; radius: 4
                                    color: AppTheme.statusOnline
                                }
                                Text {
                                    text: "SYSTEM ONLINE"
                                    color: AppTheme.statusOnline
                                    font.pixelSize: 10
                                    font.weight: Font.Medium
                                }
                            }
                        }
                        
                        Button {
                            id: bellButton
                            flat: true
                            implicitWidth: 32
                            implicitHeight: 32
                            background: Rectangle {
                                color: parent.hovered ? "#333" : "transparent"
                                radius: 8
                            }
                            contentItem: Item {
                                Image {
                                    anchors.centerIn: parent
                                    source: "qrc:/assets/bell-gray.svg"
                                    sourceSize: Qt.size(18, 18)
                                }
                                Rectangle {
                                    visible: unreadCount > 0
                                    anchors.top: parent.top; anchors.right: parent.right
                                    anchors.topMargin: -2; anchors.rightMargin: -2
                                    width: 16; height: 16; radius: 8
                                    color: AppTheme.statusOffline
                                    Text {
                                        anchors.centerIn: parent
                                        text: unreadCount
                                        color: "white"
                                        font.pixelSize: 10; font.bold: true
                                    }
                                }
                            }
                            onClicked: {
                                notificationDrawer.open()
                                unreadCount = 0
                            }
                        }

                        // Logout button: notify server and exit
                        Button {
                            id: logoutButton
                            flat: true
                            implicitWidth: 32
                            implicitHeight: 32
                            background: Rectangle {
                                color: parent.hovered ? "#333" : "transparent"
                                radius: 8
                            }
                            contentItem: Image {
                                anchors.centerIn: parent
                                source: "qrc:/assets/logout.png"
                                sourceSize: Qt.size(18, 18)
                            }
                            onClicked: {
                                console.log("[UI] Logout clicked: sending logout and returning to Login View")
                                authManager.sendLogout()
                            }
                        }

                        Drawer {
                            id: notificationDrawer
                            edge: Qt.RightEdge
                            width: 380
                            height: parent.height
                            
                            background: Rectangle {
                                color: AppTheme.surfaceCardAlt
                                border.color: AppTheme.borderCard
                                Rectangle {
                                    anchors.left: parent.left
                                    width: 1; height: parent.height
                                    color: AppTheme.borderCard
                                }
                            }

                            contentItem: ColumnLayout {
                                spacing: 0
                                
                                // Integrated Header
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 70
                                    color: AppTheme.surface
                                    
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 24; anchors.rightMargin: 16
                                        ColumnLayout {
                                            spacing: 2
                                            Text { 
                                                text: "Notifications"
                                                color: "white"
                                                font.pixelSize: 18
                                                font.bold: true 
                                            }
                                            Text {
                                                text: unreadCount + " new notifications"
                                                color: AppTheme.accent
                                                font.pixelSize: 11
                                                font.bold: true
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                        Button {
                                            flat: true
                                            implicitWidth: 32; implicitHeight: 32
                                            onClicked: notificationDrawer.close()
                                            background: null
                                            contentItem: Text {
                                                text: "✕"
                                                color: "#666"
                                                font.pixelSize: 20
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: AppTheme.borderCard }
                                }

                                ListView {
                                    id: navList
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.margins: 12
                                    clip: true
                                    model: notificationModel
                                    spacing: 10
                                    delegate: Rectangle {
                                        width: navList.width - 24; height: 95
                                        color: AppTheme.surfaceCard
                                        radius: 10
                                        border.color: AppTheme.borderCard
                                        
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 16
                                            spacing: 16
                                            
                                            Rectangle { 
                                                width: 44; height: 44; radius: 22; color: "#2d160a"
                                                border.color: AppTheme.accent
                                                border.width: 1
                                                Text { anchors.centerIn: parent; text: "⚡"; font.pixelSize: 20 } 
                                            }
                                            
                                            ColumnLayout {
                                                spacing: 4
                                                Text { text: "Object " + objectId; color: "white"; font.bold: true; font.pixelSize: 14 }
                                                Text { text: cardAgeText.toUpperCase() + " • " + timestamp; color: AppTheme.textSecondary; font.pixelSize: 11 }
                                            }
                                            
                                            Item { Layout.fillWidth: true }
                                            
                                            Button {
                                                id: viewBtn
                                                text: "View"
                                                font.pixelSize: 11
                                                palette.buttonText: viewBtn.hovered ? "white" : AppTheme.accent
                                                background: Rectangle {
                                                    color: viewBtn.hovered ? AppTheme.accent : "transparent"
                                                    border.color: AppTheme.accent
                                                    radius: 6
                                                }
                                                onClicked: {
                                                    currentNotificationIndex = index
                                                    detailPopup.objectId = objectId
                                                    detailPopup.cardAgeText = cardAgeText
                                                    detailPopup.ageGroup = ageGroup
                                                    detailPopup.isFraud = isFraud
                                                    detailPopup.open()
                                                    notificationDrawer.close()
                                                }
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: AppTheme.borderCard }
                                
                                Button {
                                    id: clearBtn
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 60
                                    text: "Clear All Notifications"
                                    flat: true
                                    font.bold: true
                                    palette.buttonText: clearBtn.hovered ? AppTheme.accentHover : AppTheme.accent
                                    onClicked: { notificationModel.clear(); notificationDrawer.close() }
                                }
                            }
                        }
                    }
                }

                // Content
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: currentViewIndex - 1

                    MonitoringView {
                        laserTrackingEnabled: rootWindow.laserTrackingEnabled
                        onViewDetailRequest: (objectId, cardAgeText, ageGroup, isFraud) => {
                            detailPopup.objectId = objectId
                            detailPopup.cardAgeText = cardAgeText
                            detailPopup.ageGroup = ageGroup
                            detailPopup.isFraud = isFraud
                            detailPopup.open()
                        }
                    }
                    AnalyticsView {}
                    SettingsView {
                        laserTrackingEnabled: rootWindow.laserTrackingEnabled
                        onLaserTrackingToggled: function(enabled) {
                            rootWindow.laserTrackingEnabled = enabled
                        }
                        onCloseClicked: {}
                    }
                }
            }
        }
    // --- Fraud Detection Notification ---
    Connections {
        target: fraudManager
        function onFraudDetected(objectId, cardAgeText, ageGroup, isFraud) {
            notificationModel.insert(0, {
                objectId: objectId,
                cardAgeText: cardAgeText,
                ageGroup: ageGroup,
                isFraud: isFraud,
                timestamp: Qt.formatDateTime(new Date(), "HH:mm:ss")
            })
            unreadCount++
        }
    }

    Connections {
        target: authManager
        function onForcedLogoutNotice(message) {
            forcedLogoutMessage = message
            forcedLogoutSecondsRemaining = 3
            if (!forcedLogoutPopup.opened) {
                forcedLogoutPopup.open()
            }
            if (!forcedLogoutTimer.running) {
                forcedLogoutTimer.start()
            }
        }
    }

    Timer {
        id: forcedLogoutTimer
        interval: 1000
        repeat: true
        onTriggered: {
            forcedLogoutSecondsRemaining = Math.max(0, forcedLogoutSecondsRemaining - 1)
            if (forcedLogoutSecondsRemaining === 0) {
                stop()
                if (forcedLogoutPopup.opened) {
                    forcedLogoutPopup.close()
                }
                authManager.notifyLocalLogout()
            }
        }
    }

    Timer {
        id: fraudTimer
        interval: 10000
        onTriggered: {} // Removed auto-close logic
    }
}
