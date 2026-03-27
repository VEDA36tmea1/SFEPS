import QtQuick 2.15
import QtQuick.Controls 2.15
import QtMultimedia

Item {
    id: root
    readonly property bool useOpencvVideo: typeof videoBackend !== "undefined"
                                            && videoBackend.useLowLatencyOpenCv === true
    property bool running: true
    property int brightness: 0
    property int contrast: 0
    property real startX: 0
    property real startY: 0
    property bool selecting: false
    property bool zoomedIn: false
    property real lastClickX: 0
    property real lastClickY: 0
    property string selectedDetection: ""
    property string externalTrackedId: ""
    property var detections: []
    property int streamLatency: 0
    property int videoMetaDelay: -1
    property double latestMetaTsMs: -1
    property double latestVideoTsMs: -1

    // Normalized zoom region in item space: (x,y,w,h) each 0..1; full frame = (0,0,1,1)
    property rect zoomNorm: Qt.rect(0, 0, 1, 1)
    // Full-frame: skip viewport clip (slightly cheaper scene graph path)
    readonly property bool zoomIsIdentity: Math.abs(zoomNorm.x) < 1e-4 && Math.abs(zoomNorm.y) < 1e-4
                                           && Math.abs(zoomNorm.width - 1) < 1e-4 && Math.abs(zoomNorm.height - 1) < 1e-4

    property int imageWidth: useOpencvVideo ? videoBackend.frameWidth
                                            : (player && player.videoSize ? player.videoSize.width : 0)
    property int imageHeight: useOpencvVideo ? videoBackend.frameHeight
                                             : (player && player.videoSize ? player.videoSize.height : 0)

    function setDetections(list) {
        detections = list
        latestVideoTsMs = Date.now()
        var latestMeta = -1
        for (var i = 0; i < detections.length; ++i) {
            var d = detections[i]
            if (!d) continue
            if (d.metaTsMs !== undefined && d.metaTsMs > latestMeta) {
                latestMeta = d.metaTsMs
            }
        }
        latestMetaTsMs = latestMeta
        if (latestMeta > 0) {
            videoMetaDelay = Math.round(latestVideoTsMs - latestMeta)
        } else {
            videoMetaDelay = -1
        }
    }
    function setSelectedDetection(id) {
        selectedDetection = id
        // Track 버튼 클릭 전까지는 C++ 추적 타겟을 변경하지 않음
        // (trackByNativeId / clearRbfTarget 에서만 추적 타겟을 설정)
    }
    function setZoomFromItem(itemRect, itemSize) {
        if (!itemSize || itemSize.width <= 0 || itemSize.height <= 0)
            return
        var nx = itemRect.x / itemSize.width
        var ny = itemRect.y / itemSize.height
        var nw = itemRect.width / itemSize.width
        var nh = itemRect.height / itemSize.height
        var minFrac = 0.02
        if (nw < minFrac)
            nw = minFrac
        if (nh < minFrac)
            nh = minFrac
        nx = Math.max(0, Math.min(1 - nw, nx))
        ny = Math.max(0, Math.min(1 - nh, ny))
        zoomNorm = Qt.rect(nx, ny, nw, nh)
        zoomedIn = true
    }
    function resetZoom() {
        zoomNorm = Qt.rect(0, 0, 1, 1)
        zoomedIn = false
    }

    function detectionAt(x, y) {
        if (imageWidth <= 0 || imageHeight <= 0) return "";
        var p = zoomedLayer.mapFromItem(root, x, y)
        var vw = zoomedLayer.width;
        var vh = zoomedLayer.height;
        if (vw <= 0 || vh <= 0) return "";
        var fx = p.x / vw;
        var fy = p.y / vh;
        var imgX = fx * imageWidth;
        var imgY = fy * imageHeight;
        for (var i = 0; i < detections.length; ++i) {
            var d = detections[i];
            if (d === undefined) continue;
            var id = String(d.id);
            var nx = d.x, ny = d.y, nw = d.w, nh = d.h;
            var boxX = nx * imageWidth;
            var boxY = ny * imageHeight;
            var boxW = nw * imageWidth;
            var boxH = nh * imageHeight;
            if (imgX >= boxX && imgX <= boxX + boxW && imgY >= boxY && imgY <= boxY + boxH) return id;
        }
        return "";
    }

    MediaPlayer {
        id: player
        autoPlay: root.running && !useOpencvVideo
        source: useOpencvVideo ? "" : rtspStreamUrl
        videoOutput: videoOutput
        playbackOptions {
            playbackIntent: PlaybackOptions.LowLatencyStreaming
            probeSize: rtspMediaProbeSize
        }
        onPlaybackStateChanged: {
            // debug
        }
    }

    Item {
        id: zoomViewport
        anchors.fill: parent
        clip: !zoomIsIdentity

        Item {
            id: zoomedLayer
            width: zoomViewport.width / zoomNorm.width
            height: zoomViewport.height / zoomNorm.height
            x: -zoomNorm.x * width
            y: -zoomNorm.y * height

            Image {
                id: opencvPreview
                visible: useOpencvVideo
                anchors.fill: parent
                fillMode: Image.Stretch
                source: useOpencvVideo ? ("image://live/frame?id=" + videoBackend.previewRevision) : ""
                asynchronous: false
                cache: false
            }

            VideoOutput {
                id: videoOutput
                visible: !useOpencvVideo
                anchors.fill: parent
                focus: !useOpencvVideo
            }

            Item {
                id: detectionOverlay
                anchors.fill: parent
                z: 100
                Repeater {
                    model: detections.length
                    Rectangle {
                        id: box
                        property bool isTracked: root.externalTrackedId !== "" && root.externalTrackedId === String(detections[index].id)
                        property bool isFraud: detections[index].fraud === true
                        visible: modelData !== undefined
                        color: "transparent"
                        border.width: isTracked ? 3 : 2
                        border.color: isTracked ? "#1e90ff"
                            : isFraud ? "#ff2222"
                            : root.selectedDetection === String(detections[index].id) ? "yellow"
                            : "green"
                        x: detections[index].x * videoOutput.width
                        y: detections[index].y * videoOutput.height
                        width: Math.max(2, detections[index].w * videoOutput.width)
                        height: Math.max(2, detections[index].h * videoOutput.height)
                        Text {
                            visible: box.isTracked
                            text: "Tracking"
                            color: "#60a5fa"
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.topMargin: -16
                            font.pixelSize: 12
                            font.bold: true
                        }

                        Text {
                            text: detections[index].id
                            color: "white"
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.topMargin: box.isTracked ? 2 : 0
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }

    Timer {
        id: videoMetaLogTimer
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            if (latestMetaTsMs > 0) {
                var delay = Math.round(Date.now() - latestMetaTsMs)
                videoMetaDelay = delay
                console.log("[VIDEO-META]", delay, "ms", "detections=", detections.length)
            }
        }
    }

    // NOTE:
    // Brightness/contrast are applied by camera-side CGI control.
    // Video + detection overlay live inside zoomedLayer so zoom stays aligned.

    // When `running` changes, start/stop player
    onRunningChanged: {
        if (running) player.play(); else player.pause();
    }
}
