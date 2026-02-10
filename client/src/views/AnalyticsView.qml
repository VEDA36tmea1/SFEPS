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
                Item {
                    Layout.fillWidth: true
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
                                            text: "15%"
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
                                                text: "6,427"
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
                                                text: "Valid Entries"
                                                color: AppTheme.textSecondary
                                                font.pixelSize: 12
                                            }
                                            Text {
                                                text: "36,423"
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
                            text: "Demographic Distribution"
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
                                    {
                                        label: "<18",
                                        val: 12
                                    },
                                    {
                                        label: "18-25",
                                        val: 28
                                    },
                                    {
                                        label: "26-35",
                                        val: 35
                                    },
                                    {
                                        label: "36-45",
                                        val: 18
                                    },
                                    {
                                        label: "46-60",
                                        val: 5
                                    },
                                    {
                                        label: "60+",
                                        val: 2
                                    }
                                ]
                                delegate: Item {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    ColumnLayout {
                                        anchors.bottom: parent.bottom
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        spacing: 8

                                        Rectangle {
                                            Layout.alignment: Qt.AlignHCenter
                                            width: 30
                                            height: parent.parent.height * (modelData.val / 40) // Scale factor
                                            color: AppTheme.surfaceBackground
                                            radius: 4

                                            // Fill
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                width: parent.width
                                                height: parent.height
                                                color: AppTheme.textSecondary
                                                opacity: 0.1 // Bg track
                                                radius: 4
                                            }
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

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Recent Evasion Alerts"
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
                            text: "GATE / LOCATION"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 200
                        }
                        Text {
                            text: "TYPE"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 100
                        }
                        Text {
                            text: "CONFIDENCE"
                            color: AppTheme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            Layout.preferredWidth: 80
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
                            ListElement {
                                ts: "14:23:05"
                                location: "Terminal A - Gate 04"
                                type: "TAILGATING"
                                confidence: "98.2%"
                                action: "Footage"
                            }
                            ListElement {
                                ts: "14:21:58"
                                location: "Terminal B - Gate 12"
                                type: "JUMP OVER"
                                confidence: "96.5%"
                                action: "Footage"
                            }
                            ListElement {
                                ts: "14:18:12"
                                location: "Main Hub - North Gate"
                                type: "FORCED ENTRY"
                                confidence: "99.8%"
                                action: "Footage"
                            }
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
                                    Layout.preferredWidth: 200
                                    font.pixelSize: 13
                                }
                                Rectangle {
                                    radius: 4
                                    color: type === "TAILGATING" ? "#9a3412" : (type === "JUMP OVER" ? "#92400e" : "#b45309")
                                    Layout.preferredWidth: 100
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
                                    Layout.preferredWidth: 80
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
