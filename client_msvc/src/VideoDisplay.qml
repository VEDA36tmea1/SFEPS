import QtQuick 2.15
import QtQuick.Controls 2.15
import QtMultimedia 6.0

Item {
    id: root
    property bool running: true
    property int brightness: 0
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

    // Expose image size reported by MediaPlayer
    property int imageWidth: player && player.videoSize ? player.videoSize.width : 0
    property int imageHeight: player && player.videoSize ? player.videoSize.height : 0

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
    function setSelectedDetection(id) { selectedDetection = id }
    function setZoomFromItem(itemRect, itemSize) { /* no-op in QMediaPlayer path */ }
    function resetZoom() { zoomedIn = false }

    function detectionAt(x, y) {
        if (imageWidth <= 0 || imageHeight <= 0) return "";
        var vw = videoOutput.width;
        var vh = videoOutput.height;
        if (vw <= 0 || vh <= 0) return "";
        var fx = (x - videoOutput.x) / vw;
        var fy = (y - videoOutput.y) / vh;
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
        autoPlay: root.running
        // source will be provided by context property `rtspStreamUrl`
        source: rtspStreamUrl
        onPlaybackStateChanged: {
            // debug
        }
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        source: player
        focus: true
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
    // Keep rendering path minimal (VideoOutput direct) for lowest latency.

    // Overlay detections as QML items
    Item {
        anchors.fill: parent
        z: 100
        Repeater {
            model: detections.length
            Rectangle {
                id: box
                property bool isTracked: root.externalTrackedId !== "" && root.externalTrackedId === String(detections[index].id)
                visible: modelData !== undefined
                color: "transparent"
                border.width: isTracked ? 3 : 2
                border.color: isTracked ? "#1e90ff"
                    : root.selectedDetection === String(detections[index].id) ? "yellow"
                    : "green"
                x: videoOutput.x + detections[index].x * videoOutput.width
                y: videoOutput.y + detections[index].y * videoOutput.height
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


    // When `running` changes, start/stop player
    onRunningChanged: {
        if (running) player.play(); else player.pause();
    }
}
