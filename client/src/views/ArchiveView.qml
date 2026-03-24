import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtMultimedia 6.4

Rectangle {
    id: root
    color: "transparent"
    Layout.fillWidth: true
    property string archiveErrorMessage: ""

    Component.onCompleted: {
        recordingListModel.clear()
        videoArchiveManager.connectCatalog()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Text {
            visible: archiveErrorMessage.length > 0
            text: archiveErrorMessage
            color: "#f87171"
            font.pixelSize: 12
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 160
            model: recordingListModel
            delegate: Rectangle {
                width: parent.width
                height: 56
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    Text {
                        text: createdAt
                        color: "white"
                        font.pixelSize: 12
                        Layout.preferredWidth: 220
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: "Run"
                        onClicked: {
                            archiveErrorMessage = ""
                            videoArchiveManager.requestPlayUrl(model.id)
                        }
                    }
                }
            }
        }

        Rectangle {
            id: previewPanel
            Layout.fillWidth: true
            Layout.preferredHeight: mediaPlayer.source.toString().length > 0 ? 160 : 0
            color: "#0f172a"
            radius: 8
            border.color: "#334155"
            visible: mediaPlayer.source.toString().length > 0
            clip: true

            VideoOutput {
                id: playerOutput
                anchors.fill: parent
            }
        }

        MediaPlayer {
            id: mediaPlayer
            videoOutput: playerOutput
        }

        Connections {
            target: videoArchiveManager
            function onSnapshotBegin(total) {
                recordingListModel.clear()
                archiveErrorMessage = ""
            }
            function onSnapshotRecord(id, createdAt) {
                archiveErrorMessage = ""
                recordingListModel.upsert(id, createdAt)
            }
            function onRecordingAdded(id, createdAt) {
                archiveErrorMessage = ""
                recordingListModel.upsert(id, createdAt)
            }
            function onRecordingDeleted(id) {
                recordingListModel.removeById(id)
            }
            function onCatalogError(code, message) {
                archiveErrorMessage = code + ": " + message
                console.warn("Archive error:", code, message)
            }
            function onPlayError(code, message) {
                archiveErrorMessage = code + ": " + message
                console.warn("Play error:", code, message)
            }
            function onPlayUrlReady(id, createdAt, url) {
                archiveErrorMessage = ""
                mediaPlayer.source = url
                mediaPlayer.play()
            }
        }
    }
}
