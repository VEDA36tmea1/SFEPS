import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0
import src.backend 1.0

Page {
    background: Rectangle {
        color: AppTheme.background
    }

    signal viewDetailRequest(string objectId, string cardAgeText, string ageGroup, bool isFraud)

    // Properties for live stats
    property int alertsToday: 0
    property real detectionRate: 94.2

    // Main Layout...
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

                Rectangle {
                    Layout.preferredHeight: 24
                    implicitWidth: streamStatusLabel.implicitWidth + 24
                    radius: 12
                    color: AppTheme.surfaceCard
                    border.color: streamStatusColor(videoDisplay.streamStatus)
                    border.width: 1

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 6

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: streamStatusColor(videoDisplay.streamStatus)
                        }

                        Text {
                            id: streamStatusLabel
                            text: streamStatusText(videoDisplay.streamStatus)
                            color: streamStatusColor(videoDisplay.streamStatus)
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }
            }

            // Single Camera View (CAM-01)
            Item {
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

                // Real Video Stream for Camera 1
                VideoDisplay {
                    id: videoDisplay
                    anchors.fill: parent
                    anchors.margins: 1 // inside border
                    brightness: brightnessSlider.value
                    running: true

                    // Zoom Selection logic
                    property real startX: 0
                    property real startY: 0
                    property bool selecting: false
                    property bool zoomedIn: false
                        property real lastClickX: 0
                        property real lastClickY: 0

                    Rectangle {
                        id: selectionRect
                        visible: videoDisplay.selecting
                        color: "#4400aaff"
                        border.color: "#00aaff"
                        border.width: 1
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: zoomBtn.checked && !videoDisplay.zoomedIn
                        cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor

                        onPressed: {
                            videoDisplay.startX = mouse.x
                            videoDisplay.startY = mouse.y
                            videoDisplay.selecting = true
                            selectionRect.x = mouse.x
                            selectionRect.y = mouse.y
                            selectionRect.width = 0
                            selectionRect.height = 0
                        }

                        onPositionChanged: {
                            if (videoDisplay.selecting) {
                                selectionRect.x = Math.min(mouse.x, videoDisplay.startX)
                                selectionRect.y = Math.min(mouse.y, videoDisplay.startY)
                                selectionRect.width = Math.abs(mouse.x - videoDisplay.startX)
                                selectionRect.height = Math.abs(mouse.y - videoDisplay.startY)
                            }
                        }

                        onReleased: {
                            if (videoDisplay.selecting) {
                                if (selectionRect.width > 10 && selectionRect.height > 10) {
                                    videoDisplay.setZoomFromItem(
                                        Qt.rect(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height),
                                        Qt.size(videoDisplay.width, videoDisplay.height)
                                    )
                                    videoDisplay.zoomedIn = true
                                }
                                videoDisplay.selecting = false
                            }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        z: 50
                        enabled: !zoomBtn.checked
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // save click coords for popup positioning
                            videoDisplay.lastClickX = mouse.x
                            videoDisplay.lastClickY = mouse.y
                            var id = videoDisplay.detectionAt(mouse.x, mouse.y)
                            if (id && id !== "") {
                                videoDisplay.setSelectedDetection(id)
                            } else {
                                videoDisplay.setSelectedDetection("")
                            }
                        }
                    }
                }

                // Popup for Track controls when an object is selected
                Popup {
                    id: trackPopup
                    // position near last click, clamp inside parent
                    x: Math.min(parent.width - width - 8, Math.max(8, videoDisplay.x + videoDisplay.lastClickX - width/2))
                    y: Math.min(parent.height - height - 8, Math.max(8, videoDisplay.y + videoDisplay.lastClickY - height/2))
                    visible: videoDisplay.selectedDetection !== ""
                    modal: false
                    focus: true

                    Rectangle {
                        width: 240
                        height: 120
                        color: AppTheme.surfaceCard
                        radius: 8
                        border.color: AppTheme.borderCard

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            Text {
                                text: "Selected: " + videoDisplay.selectedDetection
                                color: "white"
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            RowLayout {
                                spacing: 8
                                anchors.horizontalCenter: parent.horizontalCenter

                                Button {
                                    Layout.preferredWidth: 100
                                    text: "Track"
                                    enabled: videoDisplay.selectedDetection !== ""
                                    onClicked: {
                                        if (videoDisplay.selectedDetection !== "") {
                                            positionManager.sendPositionCommand("SUB_POS|" + videoDisplay.selectedDetection)
                                        }
                                        trackPopup.visible = false
                                    }
                                }

                                Button {
                                    Layout.preferredWidth: 100
                                    text: "Untrack"
                                    enabled: videoDisplay.selectedDetection !== ""
                                    onClicked: {
                                        if (videoDisplay.selectedDetection !== "") {
                                            positionManager.sendPositionCommand("UNSUB_POS|" + videoDisplay.selectedDetection)
                                            videoDisplay.setSelectedDetection("")
                                        }
                                        trackPopup.visible = false
                                    }
                                }
                            }
                        }
                    }

                    onVisibleChanged: if (!visible) videoDisplay.setSelectedDetection("")
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
                            text: "CAM-01 • MAIN"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 11
                        }
                    }
                }

                Rectangle {
                    visible: videoDisplay.streamStatus === "DISCONNECTED" || videoDisplay.streamStatus === "RECONNECTING"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 16
                    radius: 6
                    color: AppTheme.surfaceCard
                    border.color: streamStatusColor(videoDisplay.streamStatus)
                    border.width: 1
                    width: streamNoticeText.implicitWidth + 20
                    height: 30

                    Text {
                        id: streamNoticeText
                        anchors.centerIn: parent
                        text: streamStatusText(videoDisplay.streamStatus)
                        color: streamStatusColor(videoDisplay.streamStatus)
                        font.pixelSize: 11
                        font.bold: true
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
                                text: Math.round(brightnessSlider.value) + "%"
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
                            value: 50
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
                                text: Math.round(contrastSlider.value) + "%"
                                color: AppTheme.primaryOrange
                                font.bold: true
                                font.pixelSize: 11
                            }
                        }
                        Slider {
                            id: contrastSlider
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: 50
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

                    // Zoom IN/OUT Button
                    Button {
                        id: zoomBtn
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 48
                        flat: true
                        checkable: true
                        background: Rectangle {
                            color: zoomBtn.checked ? AppTheme.primaryOrange : (zoomBtn.down ? "#1a1a1a" : (zoomBtn.hovered ? "#3a3a3a" : "#2a2a2a"))
                            radius: 6
                        }
                        contentItem: RowLayout {
                            anchors.centerIn: parent
                            spacing: 8
                            Image {
                                source: "../../assets/expand_zoom.svg"
                                sourceSize.width: 20
                                sourceSize.height: 20
                                // 버튼이 체크되었을 때 아이콘 색상 (필요시)
                            }
                            Text {
                                text: "Zoom In/Out"
                                color: zoomBtn.checked ? "white" : AppTheme.primaryOrange
                                font.bold: true
                                font.pixelSize: 12
                            }
                        }
                        onCheckedChanged: {
                            if (!checked) {
                                videoDisplay.resetZoom()
                                videoDisplay.zoomedIn = false
                            }
                        }
                    }

                    // Mic Button
                    Button {
                        id: micBtn
                        Layout.preferredWidth: 35
                        Layout.preferredHeight: 35
                        flat: true
                        checkable: true
                        checked: voiceManager.active
                        
                        background: Rectangle {
                            color: micBtn.checked ? "#ef4444" : "transparent"
                            border.color: micBtn.hovered ? AppTheme.accent : (micBtn.checked ? "#ef4444" : "#333")
                            radius: 6
                        }
                        contentItem: Image {
                            anchors.centerIn: parent
                            source: "../../assets/Mic.svg"
                            sourceSize.width: 20
                            sourceSize.height: 20
                        }
                        onClicked: voiceManager.toggleMicrophone()
                    }

                    
                }
            }
        }

        // RIGHT SECTION (SIDEBAR)
        Rectangle {
            Layout.preferredWidth: 380 // Slightly wider matching drawer
            Layout.fillHeight: true
            color: AppTheme.surfaceCardAlt
            border.color: AppTheme.borderCard
            radius: 12

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 16

                // Header (Event Log + LIVE FEED)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Rectangle {
                        width: 4; height: 24
                        color: AppTheme.accent
                        radius: 2
                    }
                    Text {
                        text: "Event Log"
                        color: "white"
                        font: AppTheme.fontHeader
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        color: "#2d160a"
                        border.color: AppTheme.accent
                        width: 70; height: 22; radius: 4
                        Text {
                            anchors.centerIn: parent
                            text: "LIVE FEED"
                            color: AppTheme.accent
                            font.bold: true; font.pixelSize: 9
                        }
                    }
                }

                // Search Box
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    color: AppTheme.surfaceCardAlt
                    radius: 6
                    border.color: AppTheme.borderCard

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        spacing: 10
                        Image {
                            source: "qrc:/assets/search.svg"
                            sourceSize: Qt.size(16, 16)
                            opacity: 0.7
                        }
                        TextField {
                            Layout.fillWidth: true
                            placeholderText: "Filter events..."
                            color: "white"
                            font.pixelSize: 12
                            background: null
                        }
                    }
                }

                // Event List (mockEvents 스타일, 기본 행 형태)
                ListView {
                    id: eventListView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: ListModel {
                        id: monitoringEventModel
                    }

                    Connections {
                        target: fraudManager
                        function onFraudDetected(objectId, cardAgeText, ageGroup, isFraud) {
                            monitoringEventModel.insert(0, {
                                eventId: objectId,
                                eventType: "FARE EVASION DETECTED",
                                title: "Object " + objectId + " - " + cardAgeText.toUpperCase() + " CARD",
                                camera: "Age Group: " + ageGroup.toUpperCase() + " (Fraud: " + (isFraud ? "Y" : "N") + ")",
                                timestamp: Qt.formatDateTime(new Date(), "HH:mm:ss"),
                                confidence: "98.5%",
                                // Raw data for DetailView
                                objectId: objectId,
                                cardAgeText: cardAgeText,
                                ageGroup: ageGroup,
                                isFraud: isFraud
                            })
                            alertsToday++
                        }
                    }

                    delegate: Rectangle {
                        width: eventListView.width
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
                                        text: "Detail View"
                                        color: "white"
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    onClicked: viewDetailRequest(objectId, cardAgeText, ageGroup, isFraud)
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
                                        text: "delete"
                                        color: "white"
                                        font.pixelSize: 11
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    onClicked: monitoringEventModel.remove(index)
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

                // Removed Stats Button and Spacer to expand Event Log


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
                            text: (94.0 + (alertsToday % 20) / 10.0).toFixed(1) + "%"
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
                            text: alertsToday
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
            Connections {
                target: positionManager
                function onPositionsUpdated(list) {
                    if (!videoDisplay) return;
                    var out = [];
                    for (var i=0;i<list.length;i++) {
                        var it = list[i];
                        // if already in expected format
                        if (it.x !== undefined && it.w !== undefined && it.id !== undefined) {
                            out.push({ id: it.id, x: it.x, y: it.y, w: it.w, h: it.h });
                            continue;
                        }
                        // POS format fields L,T,R,B
                        var id = it.id !== undefined ? it.id : (it.ID !== undefined ? it.ID : "");
                        var L = it.L !== undefined ? it.L : (it.l !== undefined ? it.l : undefined);
                        var T = it.T !== undefined ? it.T : (it.t !== undefined ? it.t : undefined);
                        var R = it.R !== undefined ? it.R : (it.r !== undefined ? it.r : undefined);
                        var B = it.B !== undefined ? it.B : (it.b !== undefined ? it.b : undefined);
                        if (L !== undefined && T !== undefined && R !== undefined && B !== undefined) {
                            var nx = L;
                            var ny = T;
                            var nw = R - L;
                            var nh = B - T;
                            // If coordinates are in pixels (imageWidth>0 and values >1), normalize
                            if (videoDisplay.imageWidth > 0 && videoDisplay.imageHeight > 0) {
                                if (nx > 1 || ny > 1 || nw > 1 || nh > 1) {
                                    nx = nx / videoDisplay.imageWidth;
                                    nw = nw / videoDisplay.imageWidth;
                                    ny = ny / videoDisplay.imageHeight;
                                    nh = nh / videoDisplay.imageHeight;
                                }
                            }
                            out.push({ id: id, x: nx, y: ny, w: nw, h: nh });
                        } else {
                            // unknown format, skip
                        }
                    }
                    // debug: log converted detections to QML console
                    console.log("[QML] positionsUpdated -> converted count:", out.length);
                    for (var j=0;j<out.length;j++) {
                        console.log("[QML] det", out[j].id, "->", out[j].x, out[j].y, out[j].w, out[j].h);
                    }
                    videoDisplay.setDetections(out);
                }
            }
        }
    }

    function getCamName(idx) {
        var names = ["MAIN"];
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

    function streamStatusColor(status) {
        if (status === "ONLINE")
            return AppTheme.statusOnline;
        if (status === "CONNECTING" || status === "RECONNECTING")
            return AppTheme.accent;
        return AppTheme.statusOffline;
    }

    function streamStatusText(status) {
        if (status === "ONLINE")
            return "STREAM ONLINE";
        if (status === "CONNECTING")
            return "CONNECTING";
        if (status === "RECONNECTING")
            return "RECONNECTING";
        if (status === "STOPPED")
            return "STREAM STOPPED";
        return "STREAM OFFLINE";
    }
}
