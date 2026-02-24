import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Shapes 1.15
import src 1.0

Page {
    id: root
    background: Rectangle {
        color: AppTheme.background
    }
    signal backClicked

    property int totalEntries: 0
    property int sessionEvasions: 0
    property int totalEvasions: 0 + sessionEvasions
    property real evasionRate: totalEntries > 0 ? Math.round((totalEvasions / totalEntries) * 1000) / 10 : 0

    // Demographic Counters
    property int ageCount18: 0
    property int ageCount25: 0
    property int ageCount35: 0
    property int ageCount45: 0
    property int ageCount60: 0
    property int ageCountPlus: 0

    // Dynamic scaling helper
    property int maxAgeCount: Math.max(1, ageCount18, ageCount25, ageCount35, ageCount45, ageCount60, ageCountPlus)

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scrollView.availableWidth
            anchors.margins: 24
            spacing: 24

            // Header (Designer: icon + title)
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 24
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 8
                // Icon with tint (Using non-interactive Button to leverage icon.color)
                Button {
                    flat: true
                    enabled: false // Static icon
                    icon.source: "../../assets/bar-chart-square-03.svg"
                    icon.color: AppTheme.primaryOrange
                    icon.width: 28
                    icon.height: 28
                    background: null
                }
                Text {
                    text: "Evasion Analytics"
                    color: AppTheme.textPrimary
                    font: AppTheme.fontTitle
                }
            }

            // Search Bar Area
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 16
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
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
            }

            // Charts Area (Entry Status & Demographic)
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 320
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 16

                // Entry Status Overview (Donut Chart)
                Rectangle {
                    Layout.preferredWidth: Math.round(root.width * 0.4)
                    Layout.fillHeight: true
                    color: AppTheme.surfaceCard
                    radius: 8
                    border.color: AppTheme.borderCard
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 16

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Entry Status Overview"
                                color: "white"
                                font.bold: true
                                font.pixelSize: 16
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Text {
                                text: "..."
                                color: AppTheme.textSecondary
                                font.bold: true
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            RowLayout {
                                anchors.centerIn: parent
                                spacing: 32

                                // Chart
                                Rectangle {
                                    width: 160
                                    height: 160
                                    radius: 80
                                    color: "transparent"
                                    border.width: 20
                                    border.color: AppTheme.primaryOrange // Evasions

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 160
                                        height: 160
                                        radius: 80
                                        color: "transparent"
                                        border.width: 20
                                        border.color: "#3b82f6" // Valid Entries (Blue)
                                        opacity: 0.8
                                        z: -1
                                        // In real app, use ShapePath for partial arcs
                                    }

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4
                                        Text {
                                            text: root.evasionRate + "%"
                                            color: "white"
                                            font.bold: true
                                            font.pixelSize: 24
                                            Layout.alignment: Qt.AlignHCenter
                                        }
                                        Text {
                                            text: "EVASION"
                                            color: AppTheme.textSecondary
                                            font.pixelSize: 10
                                            Layout.alignment: Qt.AlignHCenter
                                        }
                                    }
                                }

                                // Legend
                                ColumnLayout {
                                    spacing: 12
                                    RowLayout {
                                        spacing: 8
                                        Rectangle {
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: AppTheme.primaryOrange
                                        }
                                        ColumnLayout {
                                            spacing: 2
                                            Text {
                                                text: "Evasions"
                                                color: AppTheme.textSecondary
                                                font.pixelSize: 12
                                            }
                                            Text {
                                                text: root.totalEvasions.toLocaleString()
                                                color: "white"
                                                font.bold: true
                                                font.pixelSize: 16
                                            }
                                        }
                                    }
                                    RowLayout {
                                        spacing: 8
                                        Rectangle {
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: "#3b82f6"
                                        }
                                        ColumnLayout {
                                            spacing: 2
                                            Text {
                                                text: "Total Entries"
                                                color: AppTheme.textSecondary
                                                font.pixelSize: 12
                                            }
                                            Text {
                                                text: totalEntries.toLocaleString()
                                                color: "white"
                                                font.bold: true
                                                font.pixelSize: 16
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Demographic Distribution (Bar Chart)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: AppTheme.surfaceCard
                    radius: 8
                    border.color: AppTheme.borderCard
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        Text {
                            text: "Passenger Demographics"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 16
                        }

                        Item {
                            Layout.fillHeight: true
                        } // Spacer

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 180
                            spacing: 0
                            Layout.alignment: Qt.AlignBottom

                            Repeater {
                                model: [
                                    { label: "<18", val: root.ageCount18 },
                                    { label: "18-25", val: root.ageCount25 },
                                    { label: "26-35", val: root.ageCount35 },
                                    { label: "36-45", val: root.ageCount45 },
                                    { label: "46-60", val: root.ageCount60 },
                                    { label: "60+", val: root.ageCountPlus }
                                ]
                                    Item {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true

                                        ColumnLayout {
                                            anchors.fill: parent
                                            spacing: 8

                                            // Bar Container
                                            Item {
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true

                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    anchors.horizontalCenter: parent.horizontalCenter
                                                    width: 30
                                                    height: parent.height * (modelData.val / root.maxAgeCount) * 0.9 // 90% space max
                                                    color: AppTheme.accent
                                                    radius: 4
                                                    opacity: 0.9
                                                }
                                            }

                                            Text {
                                                text: modelData.val
                                                color: "white"
                                                font.pixelSize: 10
                                                Layout.alignment: Qt.AlignHCenter
                                            }

                                            Text {
                                                text: modelData.label
                                                color: AppTheme.textSecondary
                                                font.pixelSize: 11
                                                Layout.alignment: Qt.AlignHCenter
                                            }
                                        }
                                    }
                            }
                        }
                    }
                }
            }

            // Recent Alerts Table
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                color: AppTheme.surfaceCard
                radius: 8
                border.color: AppTheme.borderCard
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    Connections {
                        target: fraudManager
                        function onFraudDetected(cardId, ageGroup, gateId, estAge) {
                            alertsModel.insert(0, {
                                ts: Qt.formatDateTime(new Date(), "HH:mm:ss"),
                                location: "Gate " + gateId,
                                type: ageGroup.toUpperCase() + " CARD",
                                confidence: cardId,
                                action: "Footage"
                            })
                            sessionEvasions++
                            totalEntries++
                            
                            // Increment age demographics based on estAge
                            if (estAge < 18) ageCount18++
                            else if (estAge <= 25) ageCount25++
                            else if (estAge <= 35) ageCount35++
                            else if (estAge <= 45) ageCount45++
                            else if (estAge <= 60) ageCount60++
                            else ageCountPlus++
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Recent Fraud Alerts"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 16
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Text {
                            text: "View All Logs"
                            color: AppTheme.primaryOrange
                            font.bold: true
                            font.pixelSize: 12
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }

                    // Table Header
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 20
                        Text {
                            text: "TIMESTAMP"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 80
                        }
                        Text {
                            text: "LOCATION"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 150
                        }
                        Text {
                            text: "CARD TYPE"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 120
                        }
                        Text {
                            text: "CARD ID"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 100
                        }
                        Text {
                            text: "ACTION"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.fillWidth: true
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: AppTheme.borderCard
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: ListModel {
                            id: alertsModel
                        }
                        delegate: ColumnLayout {
                            width: ListView.view ? ListView.view.width : 0
                            spacing: 0

                            RowLayout {
                                Layout.topMargin: 12
                                Layout.bottomMargin: 12
                                spacing: 20

                                Text {
                                    text: ts
                                    color: "white"
                                    Layout.preferredWidth: 80
                                    font.pixelSize: 13
                                }
                                Text {
                                    text: location
                                    color: "white"
                                    Layout.preferredWidth: 150
                                    font.pixelSize: 13
                                }
                                Rectangle {
                                    radius: 4
                                    color: "#9a3412"
                                    Layout.preferredWidth: 120
                                    Layout.preferredHeight: 24
                                    RowLayout {
                                        anchors.centerIn: parent
                                        Text {
                                            text: type
                                            color: "#fbbf24"
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }
                                }
                                Text {
                                    text: confidence
                                    color: "white"
                                    Layout.preferredWidth: 100
                                    font.pixelSize: 13
                                }
                                Button {
                                    Layout.preferredHeight: 28
                                    flat: true
                                    background: Rectangle {
                                        color: "transparent"
                                        border.color: "#4b5563"
                                        border.width: 1
                                        radius: 4
                                    }
                                    contentItem: RowLayout {
                                        anchors.centerIn: parent
                                        spacing: 4
                                        Text {
                                            text: "▶"
                                            color: "white"
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            text: action
                                            color: "white"
                                            font.pixelSize: 12
                                            font.bold: true
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: AppTheme.surfaceBackground
                                opacity: 0.5
                            }
                        }
                    }
                }
            }

            // Bottom Status Cards
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 24
                spacing: 16

                Repeater {
                    model: [
                        {
                            title: "Active Cameras",
                            value: "124 / 128",
                            badge: "Active",
                            color: "#22c55e",
                            icon: "video"
                        },
                        {
                            title: "Gate Sensors",
                            value: "512 / 512",
                            badge: "Online",
                            color: AppTheme.accent,
                            icon: "wifi"
                        },
                        {
                            title: "Node Latency",
                            value: "12ms",
                            badge: "Optimal",
                            color: "#3b82f6",
                            icon: "activity"
                        },
                        {
                            title: "Storage Remaining",
                            value: "2.4 TB",
                            badge: "Storage",
                            color: "#a855f7",
                            icon: "database"
                        }
                    ]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 100
                        color: AppTheme.surfaceCard
                        radius: 8
                        border.color: AppTheme.borderCard
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 16

                            Rectangle {
                                width: 48
                                height: 48
                                radius: 12
                                color: modelData.color + "1A" // 10% opacity

                                // Mock Icon (Rectangle for now)
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 24
                                    height: 24
                                    color: modelData.color
                                    radius: 6
                                }
                            }

                            ColumnLayout {
                                spacing: 4
                                Text {
                                    text: modelData.title
                                    color: AppTheme.textSecondary
                                    font.pixelSize: 12
                                }
                                Text {
                                    text: modelData.value
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
