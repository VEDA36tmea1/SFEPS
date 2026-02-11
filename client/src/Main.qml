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

    property int currentViewIndex: 1  // 0: Login(Removed), 1: Live View, 2: Analytics, 3: Hardware(Detail), 4: Settings

    // Main App (CCTV Management Dashboard 스타일: Sidebar + Top bar)
    RowLayout {
        anchors.fill: parent
        spacing: 0

            // Sidebar
            Rectangle {
                visible: currentViewIndex !== 1 // Hide in Dashboard (MonitoringView)
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

                // Top Navigation Bar (Switchable: Default vs Search)
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

                    StackLayout {
                        anchors.fill: parent
                        currentIndex: currentViewIndex === 2 ? 1 : 0 // 0: Default, 1: Search (Analytics)

                        // [0] Default Header (Dashboard/Normal)
                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

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

                                // Tabs (List View / Analytics / Settings)
                                RowLayout {
                                    spacing: 8
                                    Repeater {
                                        model: [
                                            {
                                                label: "List View",
                                                idx: 1
                                            },
                                            {
                                                label: "Analytics",
                                                idx: 2
                                            },
                                            {
                                                label: "System Settings",
                                                idx: 4
                                            }
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

                                Item {
                                    Layout.fillWidth: true
                                }

                                Item {
                                    Layout.fillWidth: true
                                }

                                // Right actions (status + bell)
                                Rectangle {
                                    Layout.preferredHeight: 28
                                    implicitWidth: 150
                                    color: "#22c55e1a" // green-500/10
                                    radius: 999
                                    RowLayout {
                                        anchors.centerIn: parent
                                        spacing: 8
                                        Rectangle {
                                            width: 8
                                            height: 8
                                            radius: 4
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
                                    flat: true
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    background: Rectangle {
                                        color: "transparent"
                                        radius: 8
                                    }
                                    contentItem: Image {
                                        anchors.centerIn: parent
                                        source: "qrc:/assets/bell-gray.svg"
                                        sourceSize: Qt.size(18, 18)
                                    }
                                }
                            }
                        }

                        // [1] Search Header (Analytics View)
                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 24
                                anchors.rightMargin: 24
                                spacing: 16

                                // Search Input
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 40
                                    color: AppTheme.surface
                                    radius: 8
                                    border.color: AppTheme.inputBorder
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 12
                                        Image {
                                            source: "qrc:/assets/search.svg"
                                            sourceSize: Qt.size(16, 16)
                                            opacity: 0.7
                                        }
                                        TextField {
                                            Layout.fillWidth: true
                                            placeholderText: "Search analytics data..."
                                            color: "white"
                                            background: Item {}
                                            font.pixelSize: 14
                                        }
                                    }
                                }

                                // User Profile / Actions
                                Rectangle {
                                    width: 32
                                    height: 32
                                    radius: 16
                                    color: "#2563eb"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "JD"
                                        color: "white"
                                        font.bold: true
                                        font.pixelSize: 12
                                    }
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
                        onViewDetailRequest: currentViewIndex = 3
                    }
                    AnalyticsView {}
                    DetailView {
                        onCloseClicked: currentViewIndex = 1
                    }
                    SettingsView {
                        onCloseClicked: {}
                    }
                }
            }
    }
}
