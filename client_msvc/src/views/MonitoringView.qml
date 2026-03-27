import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Page {
    id: rootPage
    objectName: "monitoringPage"
    background: Rectangle {
        color: AppTheme.background
    }

    signal viewDetailRequest(string objectId, string cardAgeText, string age, bool isFraud, string imagePath)
    function resetSessionStats() {
        totalBoardingCount = 0
        fraudBoardingCount = 0
        boardingDecisionByObject = ({})
        boardingAgeByObject = ({})
        boardingAgeCount18 = 0
        boardingAgeCount25 = 0
        boardingAgeCount35 = 0
        boardingAgeCount45 = 0
        boardingAgeCount60 = 0
        boardingAgeCountPlus = 0
        squishEventCount = 0
        pendingTestMonitoringEvents = []
        if (monitoringEventModel) {
            monitoringEventModel.clear()
        }
    }

    Component.onCompleted: {
        if (videoBackend) {
            videoBackend.running = true
        }
    }
    Component.onDestruction: {
        if (videoBackend) {
            videoBackend.running = false
        }
    }

    // Properties for live stats
    property int totalBoardingCount: 0
    property int fraudBoardingCount: 0
    // Object-level aggregation for virtual-line decision events.
    property var boardingDecisionByObject: ({})
    property var boardingAgeByObject: ({})
    property int boardingAgeCount18: 0
    property int boardingAgeCount25: 0
    property int boardingAgeCount35: 0
    property int boardingAgeCount45: 0
    property int boardingAgeCount60: 0
    property int boardingAgeCountPlus: 0
    property bool laserTrackingEnabled: true
    property real detectionRate: totalBoardingCount > 0
                                 ? Math.round((fraudBoardingCount / totalBoardingCount) * 1000) / 10
                                 : 0
    property var pendingDetections: []
    // Keep primary overlay source as metadata tracker (videoBackend.detections).
    // Enable only when position-port overlay should override as fallback.
    property bool usePositionOverlayFallback: false
    property string currentTrackedId: ""
    property string visualTrackedId: ""
    property string streamStatusOverrideForTest: ""
    readonly property string effectiveStreamStatus: streamStatusOverrideForTest !== ""
                                                    ? streamStatusOverrideForTest
                                                    : ((videoBackend && videoBackend.streamStatus) ? videoBackend.streamStatus : "STOPPED")
    readonly property bool trackingActive: visualTrackedId !== ""
    // Expose video backend latency at page(root) scope so Main.qml can bind safely.
    property int streamLatency: (videoBackend && videoBackend.streamLatency !== undefined) ? videoBackend.streamLatency : 0

    // Squish-readable event counter — incremented directly in appendMonitoringEvent.
    // monitoringView.squishEventCount 를 폴링해 이벤트 추가를 확인.
    // ListView 내부 proxy 를 거치지 않아 caching 문제에서 자유롭다.
    property int squishEventCount: 0
    property var pendingTestMonitoringEvents: []

    function _appendMonitoringEventNow(objectId, cardAgeText, age, isFraud, tag="", imagePath="") {
        if (!monitoringEventModel) {
            return false
        }
        var normalizedObjectId = String(objectId)
        var normalizedCardAgeText = cardAgeText !== undefined ? String(cardAgeText) : ""
        var normalizedAge = age !== undefined ? String(age) : ""
        var fraud = !!isFraud
        var normalizedTag = tag !== undefined ? String(tag) : ""
        var normalizedImagePath = imagePath !== undefined ? String(imagePath) : ""
        var key = normalizedObjectId
        var hasPrev = Object.prototype.hasOwnProperty.call(boardingDecisionByObject, key)

        if (!hasPrev) {
            totalBoardingCount++
            if (fraud) {
                fraudBoardingCount++
            }
        } else if (boardingDecisionByObject[key] !== fraud) {
            if (boardingDecisionByObject[key]) {
                fraudBoardingCount = Math.max(0, fraudBoardingCount - 1)
            }
            if (fraud) {
                fraudBoardingCount++
            }
        }
        boardingDecisionByObject[key] = fraud

        // Keep demographics monotonic per session:
        // count each object once when we first get a usable age.
        if (_hasUsableAge(normalizedAge) && !Object.prototype.hasOwnProperty.call(boardingAgeByObject, key)) {
            var nextBucket = _resolveAgeBucket(normalizedAge)
            _applyAgeBucketDelta(nextBucket, +1)
            boardingAgeByObject[key] = nextBucket
        }

        monitoringEventModel.insert(0, {
            eventId: normalizedObjectId,
            eventType: "FARE EVASION DETECTED",
            title: "Object " + normalizedObjectId + " - " + normalizedCardAgeText.toUpperCase() + " CARD",
            camera: "Age: " + normalizedAge.toUpperCase() + " (Fraud: " + (fraud ? "Y" : "N") + ")",
            timestamp: Qt.formatDateTime(new Date(), "HH:mm:ss"),
            confidence: "",
            objectId: normalizedObjectId,
            cardAgeText: normalizedCardAgeText,
            age: normalizedAge,
            isFraud: fraud,
            tag: normalizedTag,
            imagePath: normalizedImagePath
        })
        squishEventCount++  // Squish 폴링 전용 카운터
        return true
    }

    function _resolveAgeBucket(age) {
        var raw = age !== undefined ? String(age).trim().toLowerCase() : ""
        var m = raw.match(/\d+/)
        if (m && m.length > 0) {
            var n = parseInt(m[0], 10)
            if (!isNaN(n)) {
                if (n < 18) return "18"
                if (n <= 25) return "25"
                if (n <= 35) return "35"
                if (n <= 45) return "45"
                if (n <= 60) return "60"
                return "plus"
            }
        }

        // Backward compatibility for decade-like labels (e.g. 20s, 30대).
        if (raw.indexOf("10") !== -1) return "18"
        if (raw.indexOf("20") !== -1) return "25"
        if (raw.indexOf("30") !== -1) return "35"
        if (raw.indexOf("40") !== -1) return "45"
        if (raw.indexOf("50") !== -1 || raw.indexOf("60") !== -1) return "60"
        return "plus"
    }

    function _applyAgeBucketDelta(bucket, delta) {
        if (!bucket || delta === 0) return
        if (bucket === "18") boardingAgeCount18 = Math.max(0, boardingAgeCount18 + delta)
        else if (bucket === "25") boardingAgeCount25 = Math.max(0, boardingAgeCount25 + delta)
        else if (bucket === "35") boardingAgeCount35 = Math.max(0, boardingAgeCount35 + delta)
        else if (bucket === "45") boardingAgeCount45 = Math.max(0, boardingAgeCount45 + delta)
        else if (bucket === "60") boardingAgeCount60 = Math.max(0, boardingAgeCount60 + delta)
        else boardingAgeCountPlus = Math.max(0, boardingAgeCountPlus + delta)
    }

    function _hasUsableAge(age) {
        var raw = age !== undefined ? String(age).trim().toLowerCase() : ""
        if (raw === "") return false
        if (raw.match(/\d+/)) return true
        // Backward compatibility labels such as 20s / 30대
        return raw.indexOf("10") !== -1
            || raw.indexOf("20") !== -1
            || raw.indexOf("30") !== -1
            || raw.indexOf("40") !== -1
            || raw.indexOf("50") !== -1
            || raw.indexOf("60") !== -1
    }

    function _drainPendingTestMonitoringEvents() {
        if (!monitoringEventModel || pendingTestMonitoringEvents.length === 0) {
            return
        }
        while (pendingTestMonitoringEvents.length > 0) {
            var ev = pendingTestMonitoringEvents.shift()
            _appendMonitoringEventNow(ev.objectId, ev.cardAgeText, ev.age, ev.isFraud, ev.tag, ev.imagePath)
        }
    }

    Timer {
        id: pendingTestEventDrainTimer
        interval: 100
        repeat: true
        running: false
        onTriggered: {
            _drainPendingTestMonitoringEvents()
            if (!pendingTestMonitoringEvents || pendingTestMonitoringEvents.length === 0) {
                stop()
            }
        }
    }

    function appendMonitoringEvent(objectId, cardAgeText, age, isFraud, tag="", imagePath="") {
        if (!monitoringEventModel) {
            squishEventCount++  // model 미준비여도 Squish 폴링용 카운터 즉시 증가
            pendingTestMonitoringEvents.push({
                objectId: objectId,
                cardAgeText: cardAgeText,
                age: age,
                isFraud: isFraud,
                tag: tag,
                imagePath: imagePath
            })
            if (!pendingTestEventDrainTimer.running) {
                pendingTestEventDrainTimer.start()
            }
            return true
        }
        _drainPendingTestMonitoringEvents()
        return _appendMonitoringEventNow(objectId, cardAgeText, age, isFraud, tag, imagePath)
    }

    // Squish helper: inject monitoring events without backend socket dependency.
    function injectTestMonitoringEvent(objectId, cardAgeText, age, isFraud) {
        return appendMonitoringEvent(
            objectId || "TEST-OBJ-001",
            cardAgeText || "adult",
            age || "30",
            isFraud !== false
        )
    }

    // Squish helper: check if model contains a given objectId.
    function hasMonitoringEventObjectIdForTest(targetObjectId) {
        var needle = String(targetObjectId)
        // 1) 실제 모델에서 검색
        if (monitoringEventModel) {
            for (var i = 0; i < monitoringEventModel.count; ++i) {
                var item = monitoringEventModel.get(i)
                if (item && String(item.objectId) === needle) {
                    return true
                }
            }
        }
        // 2) pending 큐에서 검색 (model 미준비 시 fallback)
        if (pendingTestMonitoringEvents) {
            for (var j = 0; j < pendingTestMonitoringEvents.length; ++j) {
                var pending = pendingTestMonitoringEvents[j]
                if (pending && String(pending.objectId) === needle) {
                    return true
                }
            }
        }
        return false
    }

    // Squish helper: force stream-state label rendering for UI verification.
    function setStreamStatusOverrideForTest(status) {
        streamStatusOverrideForTest = status ? String(status) : ""
    }

    function clearStreamStatusOverrideForTest() {
        streamStatusOverrideForTest = ""
    }

    // Always receive FRAUD events at page scope for session counters.
    Connections {
        target: fraudManager
        function onFraudDetected(objectId, cardAgeText, age, isFraud, tag, imagePath) {
            appendMonitoringEvent(objectId, cardAgeText, age, isFraud, tag, imagePath)
        }
    }

    // Squish helper: emit viewDetailRequest for the item at targetIndex, bypassing
    // delegate visibility / ListView recycling timing issues entirely.
    function openMonitoringDetailByIndex(targetIndex) {
        var idx = Number(targetIndex)
        if (isNaN(idx)) {
            return false
        }
        idx = Math.floor(idx)
        if (idx < 0 || idx >= monitoringEventModel.count) {
            return false
        }
        var item = monitoringEventModel.get(idx)
        viewDetailRequest(
            item.objectId    !== undefined ? item.objectId    : "",
            item.cardAgeText !== undefined ? item.cardAgeText : "",
            item.age         !== undefined ? item.age         : "",
            !!item.isFraud,
            item.imagePath   !== undefined ? item.imagePath   : ""
        )
        return true
    }

    // Squish helper: get event model count (for debugging)
    function _getMonitoringEventModelCount() {
        if (!monitoringEventModel) {
            return 0
        }
        return monitoringEventModel.count
    }

    // Squish helper: set selected detection for track testing
    function setSelectedForTest(objectId) {
        if (videoDisplay && objectId) {
            videoDisplay.setSelectedDetection(String(objectId))
            return true
        }
        return false
    }

    // Squish helper: check if object is currently tracked
    function isObjectTrackedForTest(objectId) {
        return String(currentTrackedId) === String(objectId)
    }

    onLaserTrackingEnabledChanged: {
        if (!laserTrackingEnabled && videoDisplay) {
            videoDisplay.setSelectedDetection("")
        }
    }

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
                    objectName: "streamStatusBadge"
                    Layout.preferredHeight: 24
                    implicitWidth: streamStatusLabel.implicitWidth + 24
                    radius: 12
                    color: AppTheme.surfaceCard
                    border.color: streamStatusColor(effectiveStreamStatus)
                    border.width: 1

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 6

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: streamStatusColor(effectiveStreamStatus)
                        }

                        Text {
                            id: streamStatusLabel
                            objectName: "streamStatusLabel"
                            text: streamStatusText(effectiveStreamStatus)
                            color: streamStatusColor(effectiveStreamStatus)
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                Rectangle {
                    id: trackingStateBadge
                    objectName: "trackingStateBadge"
                    Layout.preferredHeight: 24
                    implicitWidth: trackingStateLabel.implicitWidth + 24
                    radius: 12
                    color: AppTheme.surfaceCard
                    border.width: 1
                    border.color: trackingActive ? "#60a5fa" : AppTheme.borderCard

                    Text {
                        id: trackingStateLabel
                        objectName: "trackingStateLabel"
                        anchors.centerIn: parent
                        text: trackingActive ? "TRACKING ON" : "TRACKING OFF"
                        color: trackingActive ? "#60a5fa" : AppTheme.textSecondary
                        font.pixelSize: 10
                        font.bold: true
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
                    objectName: "videoDisplay"
                    anchors.fill: parent
                    anchors.margins: 1 // inside border
                    brightness: brightnessSlider.value
                    contrast: contrastSlider.value
                    running: true
                    onBrightnessChanged: if (videoBackend) videoBackend.brightness = brightness
                    onContrastChanged: if (videoBackend) videoBackend.contrast = contrast

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
                        enabled: !zoomBtn.checked && laserTrackingEnabled
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
                    objectName: "trackPopup"
                    // position near last click, clamp inside parent
                    x: Math.min(parent.width - width - 8, Math.max(8, videoDisplay.x + videoDisplay.lastClickX - width/2))
                    y: Math.min(parent.height - height - 8, Math.max(8, videoDisplay.y + videoDisplay.lastClickY - height/2))
                    visible: laserTrackingEnabled && videoDisplay.selectedDetection !== ""
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
                                objectName: "trackPopupSelectedText"
                                text: "Selected: " + videoDisplay.selectedDetection
                                color: "white"
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            Text {
                                objectName: "trackPopupTrackingStateText"
                                text: visualTrackedId !== "" ? "Tracking: " + visualTrackedId : "Tracking: -"
                                color: visualTrackedId !== "" ? "#60a5fa" : "#9ca3af"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            RowLayout {
                                spacing: 8
                                anchors.horizontalCenter: parent.horizontalCenter

                                Button {
                                    id: trackBtn
                                    objectName: "trackButton"
                                    Layout.preferredWidth: 100
                                    Layout.preferredHeight: 34
                                    text: "Track"
                                    flat: true
                                    enabled: videoDisplay.selectedDetection !== ""
                                             && currentTrackedId === ""
                                    background: Rectangle {
                                        radius: 6
                                        color: !trackBtn.enabled ? "#1f1f1f"
                                              : trackBtn.down ? "#cf5a28"
                                              : trackBtn.hovered ? AppTheme.accentHover
                                              : AppTheme.accent
                                        border.width: 1
                                        border.color: !trackBtn.enabled ? AppTheme.borderCard : "transparent"
                                    }
                                    contentItem: Text {
                                        text: trackBtn.text
                                        color: trackBtn.enabled ? "white" : AppTheme.textDisabled
                                        font.pixelSize: 12
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    onClicked: {
                                        if (videoDisplay.selectedDetection !== "") {
                                            var targetId = String(videoDisplay.selectedDetection)
                                            positionManager.sendPositionCommand("TRACK_START|" + targetId)
                                            // RBF PWM 자동 추적 시작 (매 tick bbox 갱신)
                                            videoBackend.trackByNativeId(targetId)
                                            currentTrackedId = targetId
                                            visualTrackedId = targetId
                                            videoDisplay.externalTrackedId = visualTrackedId
                                        }
                                    }
                                }

                                Button {
                                    id: untrackBtn
                                    objectName: "untrackButton"
                                    Layout.preferredWidth: 100
                                    Layout.preferredHeight: 34
                                    text: "Untrack"
                                    flat: true
                                    enabled: currentTrackedId !== ""
                                             && String(videoDisplay.selectedDetection) === currentTrackedId
                                    background: Rectangle {
                                        radius: 6
                                        color: !untrackBtn.enabled ? "transparent"
                                              : untrackBtn.down ? "#1a1a1a"
                                              : untrackBtn.hovered ? "#2b2b2b"
                                              : "transparent"
                                        border.width: 1
                                        border.color: !untrackBtn.enabled ? AppTheme.borderCard : AppTheme.inputBorder
                                    }
                                    contentItem: Text {
                                        text: untrackBtn.text
                                        color: untrackBtn.enabled ? "white" : AppTheme.textDisabled
                                        font.pixelSize: 12
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    onClicked: {
                                        if (currentTrackedId !== "") {
                                            positionManager.sendPositionCommand("TRACK_END|" + currentTrackedId)
                                            // RBF PWM 추적 해제
                                            videoBackend.clearRbfTarget()
                                            currentTrackedId = ""
                                            visualTrackedId = ""
                                            videoDisplay.externalTrackedId = ""
                                        }
                                    }
                                }
                            }
                        }
                    }
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
                    id: streamNoticeBanner
                    objectName: "streamNoticeBanner"
                    visible: effectiveStreamStatus === "DISCONNECTED" || effectiveStreamStatus === "RECONNECTING"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 16
                    radius: 6
                    color: AppTheme.surfaceCard
                    border.color: streamStatusColor(effectiveStreamStatus)
                    border.width: 1
                    width: streamNoticeText.implicitWidth + 20
                    height: 30

                    Text {
                        id: streamNoticeText
                        objectName: "streamNoticeText"
                        anchors.centerIn: parent
                        text: streamStatusText(effectiveStreamStatus)
                        color: streamStatusColor(effectiveStreamStatus)
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
                            from: 1
                            to: 100
                            value: 53
                            // 카메라에서 실제값 fetch 완료 시 슬라이더 갱신 (누르는 중엔 간섭 없음)
                            Binding on value {
                                value: videoDisplay.brightness
                                when: !brightnessSlider.pressed
                            }
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
                            from: 1
                            to: 100
                            value: 52
                            // 카메라에서 실제값 fetch 완료 시 슬라이더 갱신 (누르는 중엔 간섭 없음)
                            Binding on value {
                                value: videoDisplay.contrast
                                when: !contrastSlider.pressed
                            }
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
                            placeholderTextColor: "#99FFFFFF"
                            palette.text: "white"
                            palette.placeholderText: "#99FFFFFF"
                            font.pixelSize: 12
                            background: null
                        }
                    }
                }

                // Event List (mockEvents 스타일, 기본 행 형태)
                ListView {
                    id: eventListView
                    objectName: "monitoringEventListView"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: ListModel {
                        id: monitoringEventModel
                    }

                    // Handle image downloads that arrive after FRAUD message
                    Connections {
                        target: fraudManager
                        function onImageReceived(objectId, tag, localFilePath) {
                            // Find and update the event with the downloaded image
                            for (let i = 0; i < monitoringEventModel.count; i++) {
                                var item = monitoringEventModel.get(i)
                                if (item.objectId === objectId && item.tag === tag) {
                                    monitoringEventModel.setProperty(i, "imagePath", localFilePath)
                                    console.debug("[MonitoringView] Updated event image:", objectId, tag, "->", localFilePath)
                                }
                            }
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
                                    objectName: "monitoringDetailViewButton_" + index
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
                                    onClicked: viewDetailRequest(objectId, cardAgeText, age, isFraud, imagePath)
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
                            text: "FARE EVASION RATE"
                            color: "#888"
                            font.bold: true
                            font.pixelSize: 10
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: detectionRate.toFixed(1) + "%"
                            color: AppTheme.primaryOrange
                            font.bold: true
                            font.pixelSize: 20
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }

                    // Fraud Riders
                    ColumnLayout {
                        spacing: 0
                        Text {
                            text: "FRAUD RIDERS"
                            color: "#888"
                            font.bold: true
                            font.pixelSize: 10
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: fraudBoardingCount
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
                target: videoBackend
                function onDetectionsChanged() {
                    if (!videoDisplay || !videoBackend) return
                    videoDisplay.setDetections(videoBackend.detections)
                }
            }

            Connections {
                target: positionManager
                function onPositionsUpdated(list) {
                    if (!videoDisplay) return;
                    var out = [];
                    var maxRender = 20
                    for (var i=0;i<list.length;i++) {
                        var it = list[i];
                        var incomingId = it.id !== undefined ? it.id : (it.ID !== undefined ? it.ID : "")
                        var normalizedId = String(incomingId)
                        // if already in expected format
                        if (it.x !== undefined && it.w !== undefined && it.id !== undefined) {
                            var aFlag = false;
                            if (it.alert !== undefined) aFlag = !!it.alert;
                            if (!aFlag && it.FRAUD !== undefined) {
                                var fv2 = ("" + it.FRAUD).toUpperCase();
                                if (fv2 === "Y" || fv2 === "1" || fv2 === "TRUE") aFlag = true;
                            }
                            var dtype2 = (it.type !== undefined) ? it.type : (it.TYPE !== undefined ? it.TYPE : "");
                            var age2 = (it.age !== undefined) ? it.age : (it.AGE !== undefined ? it.AGE : "");
                            out.push({ id: String(it.id), x: it.x, y: it.y, w: it.w, h: it.h, alert: aFlag, type: dtype2, age: age2 });
                            if (out.length >= maxRender) break;
                            continue;
                        }
                        // POS format fields L,T,R,B
                        var id = normalizedId;
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
                            // Preserve alert/type fields if present on incoming POS map
                            var alertFlag = false;
                            if (it.alert !== undefined) alertFlag = !!it.alert;
                            if (!alertFlag && it.FRAUD !== undefined) {
                                var fv = ("" + it.FRAUD).toUpperCase();
                                if (fv === "Y" || fv === "1" || fv === "TRUE") alertFlag = true;
                            }
                            var dtype = (it.type !== undefined) ? it.type : (it.TYPE !== undefined ? it.TYPE : "");
                            var ageRaw = (it.age !== undefined) ? it.age : (it.AGE !== undefined ? it.AGE : "");
                            out.push({ id: id, x: nx, y: ny, w: nw, h: nh, alert: alertFlag, type: dtype, age: ageRaw });
                            if (out.length >= maxRender) break;
                        } else {
                            // unknown format, skip
                        }
                    }
                    // Update bottom stats from position port: compute total and fraud counts
                    var total = out.length;
                    var fraudCount = 0;
                    for (var fi = 0; fi < out.length; ++fi) {
                        if (out[fi] && out[fi].alert) fraudCount++;
                    }
                    // Keep cumulative counters updated from position stream as well.
                    for (var j = 0; j < out.length; ++j) {
                        var itm = out[j]
                        if (!itm) continue
                        var oid = itm.id !== undefined ? String(itm.id) : ""
                        if (oid === "") continue

                        var isAlert = !!itm.alert
                        var hadDecision = Object.prototype.hasOwnProperty.call(boardingDecisionByObject, oid)
                        if (!hadDecision) {
                            boardingDecisionByObject[oid] = isAlert
                            totalBoardingCount++
                            if (isAlert) fraudBoardingCount++
                        } else if (boardingDecisionByObject[oid] !== isAlert) {
                            if (boardingDecisionByObject[oid]) {
                                fraudBoardingCount = Math.max(0, fraudBoardingCount - 1)
                            }
                            if (isAlert) fraudBoardingCount++
                            boardingDecisionByObject[oid] = isAlert
                        }

                        // Position stream may not provide age; skip demographics update in that case.
                        if (_hasUsableAge(itm.age) && !Object.prototype.hasOwnProperty.call(boardingAgeByObject, oid)) {
                            var nextBucket = _resolveAgeBucket(itm.age)
                            _applyAgeBucketDelta(nextBucket, +1)
                            boardingAgeByObject[oid] = nextBucket
                        }
                    }

                    pendingDetections = out;
                    if (videoDisplay && usePositionOverlayFallback) {
                        videoDisplay.setDetections(pendingDetections)
                    }
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
