import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0
import src.backend 1.0

Page {
    background: Rectangle {
        color: AppTheme.background
    }

    signal viewDetailRequest

    // Main Layout: Left (Video + Controls) | Right (Events)
    RowLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 24

        // LEFT SECTION
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            // Header (Stream Status)
            RowLayout {
                spacing: 4
                Image {
                    source: "qrc:/assets/video_Stream_logo.svg"
                    sourceSize.width: 24
                    sourceSize.height: 24
                }
                Text {
                    text: "Active Surveillance Streams"
                    color: "white"
                    font: AppTheme.fontHeader
                }
                Item {
                    Layout.fillWidth: true
                }
            }

            // Video Grid (2x2)
            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: 2
                columnSpacing: 16
                rowSpacing: 16

                Repeater {
                    model: 4
                    delegate: Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        // Background
                        Rectangle {
                            anchors.fill: parent
                            color: "black"
                            radius: 4
                            border.color: "#333"
                            border.width: 1
                        }

                        // Real Video Stream for Camera 1 (Index 0)
                        Loader {
                            anchors.fill: parent
                            // Only load VideoDisplay for the first item to save resources/bandwidth
                            active: index === 0
                            sourceComponent: VideoDisplay {
                                anchors.fill: parent
                                anchors.margins: 1 // inside border
                                // Bind brightness to slider (brightnessSlider is below, we need an id)
                                brightness: brightnessSlider.value
                                running: true
                            }
                        }

                        // Placeholder for others
                        Text {
                            anchors.centerIn: parent
                            text: (index === 0) ? "" : "NO SIGNAL"
                            color: "#555"
                            visible: index !== 0
                        }

                        // Camera ID & Name Overlay
                        RowLayout {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 8
                            z: 10 // Ensure it's on top of video

                            // Recording Dot
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: "#ef4444" // Red
                            }

                            // Cam Label
                            Rectangle {
                                color: "#99000000" // Transparent black
                                radius: 4
                                width: camText.implicitWidth + 16
                                height: 26
                                Text {
                                    id: camText
                                    anchors.centerIn: parent
                                    text: "CAM-0" + (index + 1) + " • " + getCamName(index)
                                    color: "white"
                                    font.bold: true
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }
            }

            // CONTROL BAR
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                color: AppTheme.surface // #1a1a1a
                radius: 8

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    spacing: 32

                    // Brightness Control
                    ColumnLayout {
                        Layout.preferredWidth: 200
                        spacing: 2 // Minimal spacing as requested

                        // Label Row
                        RowLayout {
                            Text {
                                text: "BRIGHTNESS"
                                color: "#888"
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Text {
                                text: "75%"
                                color: AppTheme.primaryOrange
                                font.bold: true
                                font.pixelSize: 11
                            }
                        }

                        // Custom Slider
                        Slider {
                            id: brightnessSlider
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: 75
                            background: Rectangle {
                                x: parent.leftPadding
                                y: parent.topPadding + parent.availableHeight / 2 - height / 2
                                implicitWidth: 200
                                implicitHeight: 4
                                width: parent.availableWidth
                                height: implicitHeight
                                radius: 2
                                color: "#333"
                                Rectangle {
                                    width: parent.parent.visualPosition * parent.width
                                    height: parent.height
                                    color: AppTheme.primaryOrange
                                    radius: 2
                                }
                            }
                            handle: Rectangle {
                                x: parent.leftPadding + parent.visualPosition * (parent.availableWidth - width)
                                y: parent.topPadding + parent.availableHeight / 2 - height / 2
                                implicitWidth: 16
                                implicitHeight: 16
                                radius: 8
                                color: AppTheme.primaryOrange
                            }
                        }
                    }

                    // Contrast Control (Duplicate logic for robust layout)
                    ColumnLayout {
                        Layout.preferredWidth: 200
                        spacing: 2
                        RowLayout {
                            Text {
                                text: "CONTRAST"
                                color: "#888"
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Text {
                                text: "60%"
                                color: AppTheme.primaryOrange
                                font.bold: true
                                font.pixelSize: 11
                            }
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: 60
                            background: Rectangle {
                                x: parent.leftPadding
                                y: parent.topPadding + parent.availableHeight / 2 - height / 2
                                implicitWidth: 200
                                implicitHeight: 4
                                width: parent.availableWidth
                                height: implicitHeight
                                radius: 2
                                color: "#333"
                                Rectangle {
                                    width: parent.parent.visualPosition * parent.width
                                    height: parent.height
                                    color: AppTheme.primaryOrange
                                    radius: 2
                                }
                            }
                            handle: Rectangle {
                                x: parent.leftPadding + parent.visualPosition * (parent.availableWidth - width)
                                y: parent.topPadding + parent.availableHeight / 2 - height / 2
                                implicitWidth: 16
                                implicitHeight: 16
                                radius: 8
                                color: AppTheme.primaryOrange
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    } // Spacer

                    // Zoom Out Button
                    Button {
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 48
                        flat: true
                        background: Rectangle {
                            color: parent.down ? "#1a1a1a" : (parent.hovered ? "#3a3a3a" : "#2a2a2a")
                            radius: 6
                        }
                        contentItem: RowLayout {
                            anchors.centerIn: parent
                            spacing: 8
                            Image {
                                source: "../../assets/expand_zoom.svg"
                                sourceSize.width: 20
                                sourceSize.height: 20
                            }
                            Text {
                                text: "Zoom In"
                                color: AppTheme.primaryOrange
                                font.bold: true
                                font.pixelSize: 12
                            }
                        }
                    }

                    // Mic Button
                    Button {
                        Layout.preferredWidth: 35
                        Layout.preferredHeight: 35
                        flat: true
                        background: Rectangle {
                            color: "transparent"
                            border.color: parent.hovered ? AppTheme.accent : "#333"
                            radius: 6
                        }
                        contentItem: Image {
                            anchors.centerIn: parent
                            source: "../../assets/Mic.svg"
                            sourceSize.width: 20
                            sourceSize.height: 20
                        }
                    }
                }
            }
        }

        // RIGHT SECTION (SIDEBAR)
        Rectangle {
            Layout.preferredWidth: 360
            Layout.fillHeight: true
            color: "transparent" // Matches parent bg, but sidebar usually has structure

            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                // Header (Event Log + LIVE FEED)
                RowLayout {
                    Image {
                        source: "qrc:/assets/List.svg"
                        sourceSize.width: 24
                        sourceSize.height: 24
                    }
                    Text {
                        text: "Event Log"
                        color: "white"
                        font: AppTheme.fontHeader
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        color: AppTheme.accent
                        width: 80
                        height: 24
                        radius: 4
                        Text {
                            anchors.centerIn: parent
                            text: "LIVE FEED"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 10
                        }
                    }
                }

                // Search Box (Filter events...)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: AppTheme.inputBg
                    radius: 4
                    border.color: AppTheme.inputBorder

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Image {
                            source: "qrc:/assets/search.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                        }
                        TextField {
                            Layout.fillWidth: true
                            placeholderText: "Filter events..."
                            color: "white"
                            background: null
                            font.pixelSize: 12
                        }
                    }
                }

                // Event List (mockEvents 스타일, 기본 행 형태)
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: ListModel {
                        ListElement {
                            eventId: "1"
                            eventType: "FARE EVASION DETECTED"
                            title: "Gate 04 - Tailgating"
                            camera: "Camera: CAM-04 North Entry"
                            timestamp: "14:51:58"
                            confidence: "98.2%"
                        }
                        ListElement {
                            eventId: "2"
                            eventType: "GATE ACCESS"
                            title: "Standard Entry - Gate 01"
                            camera: "ID: #USR-5621 (Staff)"
                            timestamp: "14:50:17"
                            confidence: ""
                        }
                        ListElement {
                            eventId: "3"
                            eventType: "FARE EVASION DETECTED"
                            title: "Gate 12 - Forcing Gate"
                            camera: "Camera: CAM-01 West Entry"
                            timestamp: "14:28:44"
                            confidence: "96.5%"
                        }
                        ListElement {
                            eventId: "4"
                            eventType: "CROWD ALERT"
                            title: "Platform B - High Density"
                            camera: "Occupancy: 82%"
                            timestamp: "14:25:30"
                            confidence: ""
                        }
                    }

                    delegate: Rectangle {
                        width: ListView.view ? ListView.view.width : 0
                        color: hovered ? "#262626" : "transparent"
                        implicitHeight: contentCol.implicitHeight + 16

                        // 하단 보더
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: AppTheme.borderCard
                        }

                        ColumnLayout {
                            id: contentCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 12
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                // Type Badge
                                Rectangle {
                                    radius: 4
                                    color: eventColor(eventType)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 4
                                        Text {
                                            text: eventType
                                            color: "white"
                                            font.pixelSize: 10
                                        }
                                    }
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: timestamp
                                    color: "#6b7280"
                                    font.pixelSize: 11
                                }
                            }

                            Text {
                                text: title
                                color: "white"
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Text {
                                text: camera
                                color: "#9ca3af"
                                font.pixelSize: 11
                            }

                            // Fare evasion일 때만 액션 버튼 보여주기
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                visible: eventType === "FARE EVASION DETECTED"

                                Button {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 24
                                    flat: true
                                    background: Rectangle {
                                        color: AppTheme.accent
                                        radius: 4
                                    }
                                    contentItem: Text {
                                        text: "View Clip"
                                        color: "white"
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    onClicked: viewDetailRequest()
                                }

                                Button {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 24
                                    flat: true
                                    background: Rectangle {
                                        color: "transparent"
                                        radius: 4
                                        border.color: AppTheme.inputBorder
                                        border.width: 1
                                    }
                                    contentItem: Text {
                                        text: "Acknowledge"
                                        color: "white"
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }

                            // Confidence 라벨
                            RowLayout {
                                spacing: 4
                                visible: confidence !== ""
                                Text {
                                    text: "Confidence:"
                                    color: "#6b7280"
                                    font.pixelSize: 11
                                }
                                Text {
                                    text: confidence
                                    color: "#22c55e"
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }

                // Spacer to push stats section to bottom
                Item {
                    Layout.fillHeight: true
                }

                // STATS BUTTON (THE FIX)
                Button {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    flat: true

                    // Custom Background
                    background: Rectangle {
                        color: parent.hovered ? AppTheme.primaryOrangeHover : AppTheme.primaryOrange
                        radius: 8
                    }

                    // Explicit Content Item (Transparent Backgrounds)
                    contentItem: RowLayout {
                        anchors.centerIn: parent
                        spacing: 10

                        Image {
                            source: "../../assets/statistic.svg"
                            sourceSize.width: 20
                            sourceSize.height: 20
                            // No background set, so transparent by default
                        }
                        Text {
                            text: "OPEN SYSTEM STATISTICS"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 10
                            // No background set, so transparent by default
                        }
                    }

                    onClicked: viewDetailRequest()
                }

                // Bottom Stats
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 60
                    spacing: 24

                    // 왼쪽 여백
                    Item { Layout.fillWidth: true }

                    // Detection Rate
                    ColumnLayout {
                        spacing: 0
                        Text {
                            text: "DETECTION RATE"
                            color: "#888"
                            font.bold: true
                            font.pixelSize: 10
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: "94.2%"
                            color: AppTheme.primaryOrange
                            font.bold: true
                            font.pixelSize: 20
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }

                    // Alerts Today
                    ColumnLayout {
                        spacing: 0
                        Text {
                            text: "ALERTS TODAY"
                            color: "#888"
                            font.bold: true
                            font.pixelSize: 10
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: "128"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 20
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }

                    // 오른쪽 여백
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }

    function getCamName(idx) {
        var names = ["MAIN ENTRANCE WEST", "TICKET GATES NORTH", "CONCOURSE A", "PLATFORM B SOUTH"];
        return names[idx];
    }

    // Event 타입별 배지 색상
    function eventColor(t) {
        if (t === "FARE EVASION DETECTED")
            return AppTheme.accent;
        if (t === "GATE ACCESS")
            return "#3b82f6";
        if (t === "CROWD ALERT")
            return "#6b7280";
        return "#4b5563";
    }
}
