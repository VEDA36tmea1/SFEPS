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

    // Monitoring forwarded values from main window (optional)
    property int monitoringTotalBoardingCount: 0
    property int monitoringFraudBoardingCount: 0
    property int monitoringStreamLatency: 0
    property real monitoringEvasionRate: monitoringTotalBoardingCount > 0 ? Math.round((monitoringFraudBoardingCount / monitoringTotalBoardingCount) * 1000) / 10 : 0

    // Dynamic scaling helper
    property int maxAgeCount: Math.max(1, ageCount18, ageCount25, ageCount35, ageCount45, ageCount60, ageCountPlus)

    // Keep analytics counters updated from fraud events even though the alert list card is removed.
    Connections {
        target: fraudManager
        function onFraudDetected(objectId, cardAgeText, ageGroup, isFraud) {
            sessionEvasions++
            totalEntries++

            const grp = ageGroup.toLowerCase()
            if (grp.indexOf("10") !== -1) ageCount18++
            else if (grp.indexOf("20") !== -1) ageCount25++
            else if (grp.indexOf("30") !== -1) ageCount35++
            else if (grp.indexOf("40") !== -1) ageCount45++
            else if (grp.indexOf("50") !== -1 || grp.indexOf("60") !== -1) ageCount60++
            else ageCountPlus++
        }
    }

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
                    text: "Analytics"
                    color: AppTheme.textPrimary
                    font: AppTheme.fontTitle
                }
            }

            // (Search box removed from top; moved above Recent Fraud Alerts below)

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

                                // Chart (Canvas-based donut: blue = total entries, orange arc = evasions)
                                Item {
                                    width: 160
                                    height: 160
                                    Canvas {
                                        id: donutCanvas
                                        anchors.fill: parent
                                        onPaint: {
                                            var ctx = getContext("2d");
                                            ctx.reset();
                                            ctx.clearRect(0, 0, width, height);
                                            var cx = width / 2;
                                            var cy = height / 2;
                                            var radius = Math.min(width, height) / 2 - 10;
                                            var lineW = 20;

                                            // Draw full ring for Total Entries (blue)
                                            ctx.beginPath();
                                            ctx.lineWidth = lineW;
                                            ctx.strokeStyle = (AppTheme && AppTheme.primaryBlue) ? AppTheme.primaryBlue : "#3b82f6";
                                            // fallback if primaryBlue not defined
                                            try { ctx.strokeStyle = AppTheme.primaryBlue ? AppTheme.primaryBlue : "#3b82f6"; } catch(e) {}
                                            ctx.arc(cx, cy, radius, 0, 2 * Math.PI);
                                            ctx.stroke();

                                            // Compute ratio from monitoring counts (fall back to local totals if not set)
                                            var total = monitoringTotalBoardingCount > 0 ? monitoringTotalBoardingCount : (root.totalEntries > 0 ? root.totalEntries : 0);
                                            var ev = monitoringFraudBoardingCount > 0 ? monitoringFraudBoardingCount : (root.totalEvasions > 0 ? root.totalEvasions : 0);
                                            var ratio = 0;
                                            if (total > 0) ratio = Math.min(1, ev / total);

                                            // Draw evasion arc (orange)
                                            if (ratio > 0) {
                                                var start = -Math.PI / 2; // top
                                                var end = start + ratio * 2 * Math.PI;
                                                ctx.beginPath();
                                                ctx.lineWidth = lineW;
                                                try { ctx.strokeStyle = AppTheme.primaryOrange ? AppTheme.primaryOrange : "#f97316"; } catch(e) { ctx.strokeStyle = "#f97316"; }
                                                ctx.arc(cx, cy, radius, start, end);
                                                ctx.stroke();
                                            }
                                        }
                                        Component.onCompleted: donutCanvas.requestPaint()
                                    }

                                    // Center labels
                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4
                                        Text {
                                            text: monitoringEvasionRate + "%"
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

                                    // Repaint when underlying values change
                                    Connections {
                                        target: root
                                        onMonitoringTotalBoardingCountChanged: donutCanvas.requestPaint()
                                        onMonitoringFraudBoardingCountChanged: donutCanvas.requestPaint()
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
                                                text: monitoringFraudBoardingCount.toLocaleString()
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
                                                text: monitoringTotalBoardingCount.toLocaleString()
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

            // Video Storage
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 240
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

                    Text {
                        text: "Video Storage"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Loader {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        source: "ArchiveView.qml"
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
                            value: "1",
                            badge: "Active",
                            color: "#22c55e",
                            icon: "video"
                        },
                        {
                            title: "Archived Videos",
                            value: "0",
                            badge: "Archived",
                            color: "#d4e635",
                            icon: "wifi"
                        },
                        {
                            title: "Stream Latency",
                            value: "0ms",
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
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 4
                                Text {
                                    text: modelData.title
                                    color: AppTheme.textSecondary
                                    font.pixelSize: 12
                                }
                                Text {
                                    text: modelData.title === "Stream Latency"
                                          ? (monitoringStreamLatency + " ms")
                                        : modelData.title === "Archived Videos"
                                            ? String(recordingListModel.count)
                                            : modelData.value
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
