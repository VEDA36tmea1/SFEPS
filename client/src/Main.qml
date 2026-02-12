import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0
import src.views 1.0

Window {
    width: 1250
    height: 750
    visible: true
    title: "Hanwha Vision SFEPS"
    color: AppTheme.background

    property int currentViewIndex: 1  // 1: Live View, 2: Analytics, 3: Settings
    property int unreadCount: 0

    ListModel {
        id: notificationModel
    }

    Window {
        id: detailWindow
        width: 1000
        height: 600
        title: "Hardware Incident Detail"
        visible: false
        color: AppTheme.background

        property alias cardId: detailView.cardId
        property alias ageGroup: detailView.ageGroup
        property alias gateId: detailView.gateId
        property alias estAge: detailView.estAge

        DetailView {
            id: detailView
            anchors.fill: parent
            onCloseClicked: detailWindow.close()
            // We might need to add properties to DetailView.qml or just pass them if it's dynamic
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
                                    text: "John Doe"
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
                            onClicked: notificationPopup.open()
                        }

                        Popup {
                            id: notificationPopup
                            y: bellButton.height + 8
                            x: bellButton.width - width // Align right edges
                            width: 300
                            height: Math.min(400, navList.contentHeight + 40)
                            padding: 0
                            background: Rectangle {
                                color: AppTheme.surfaceCardAlt
                                radius: 8
                                border.color: AppTheme.borderCard
                            }
                            contentItem: ColumnLayout {
                                spacing: 0
                                Rectangle {
                                    Layout.fillWidth: true; Layout.preferredHeight: 40; color: "transparent"
                                    Text { anchors.centerIn: parent; text: "Notifications (" + unreadCount + ")"; color: "white"; font.bold: true }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: AppTheme.borderCard }
                                ListView {
                                    id: navList
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                    model: notificationModel
                                    delegate: ItemDelegate {
                                        width: parent.width; height: 70
                                        contentItem: RowLayout {
                                            spacing: 12
                                            Rectangle { width: 32; height: 32; radius: 16; color: "#450a0a"; Text { anchors.centerIn: parent; text: "⚠️"; font.pixelSize: 16 } }
                                            ColumnLayout {
                                                spacing: 2
                                                Text { text: cardId; color: "white"; font.bold: true; font.pixelSize: 12 }
                                                Text { text: "Gate " + gateId; color: "#9ca3af"; font.pixelSize: 10 }
                                            }
                                            Item { Layout.fillWidth: true }
                                            Button {
                                                text: "Detail"
                                                font.pixelSize: 10
                                                onClicked: {
                                                    detailWindow.cardId = cardId
                                                    detailWindow.ageGroup = ageGroup
                                                    detailWindow.gateId = gateId
                                                    detailWindow.estAge = estAge
                                                    detailWindow.show()
                                                    notificationPopup.close()
                                                }
                                            }
                                        }
                                    }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: AppTheme.borderCard }
                                Button {
                                    Layout.fillWidth: true; text: "Clear All"; flat: true
                                    onClicked: { notificationModel.clear(); unreadCount = 0; notificationPopup.close() }
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
                        onViewDetailRequest: {} 
                    }
                    AnalyticsView {}
                    SettingsView {
                        onCloseClicked: {}
                    }
                }
            }
        }
    // --- Fraud Detection Notification ---
    Connections {
        target: fraudManager
        function onFraudDetected(cardId, ageGroup, gateId, estAge) {
            notificationModel.insert(0, {
                cardId: cardId,
                ageGroup: ageGroup,
                gateId: gateId,
                estAge: estAge,
                timestamp: Qt.formatDateTime(new Date(), "HH:mm:ss")
            })
            unreadCount++
        }
    }

    Timer {
        id: fraudTimer
        interval: 10000
        onTriggered: {} // Removed auto-close logic
    }
}
