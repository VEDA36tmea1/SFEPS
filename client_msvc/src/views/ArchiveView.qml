import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtMultimedia 6.4
import src 1.0

Rectangle {
    id: root
    color: "transparent"
    Layout.fillWidth: true
    property string archiveErrorMessage: ""
    property string currentPlaybackCreatedAt: ""
    property string videoFilterText: ""

    function _videoMatchesNeedle(itemCreatedAt, needle) {
        if (!needle || needle === "") return true
        const n = String(needle).trim().toLowerCase()
        if (!itemCreatedAt || String(itemCreatedAt).length === 0) return false

        const createdAtStr = String(itemCreatedAt)
        const caLower = createdAtStr.toLowerCase()
        if (caLower.indexOf(n) !== -1) return true

        // Make partial search friendlier for ISO timestamps.
        const d = root.datePart(createdAtStr).toLowerCase()
        const t = root.timePart(createdAtStr).toLowerCase()
        return d.indexOf(n) !== -1 || t.indexOf(n) !== -1
    }

    function datePart(ts) {
        if (!ts || ts.length === 0) return "-"
        const s = String(ts)
        if (s.indexOf("T") >= 0) return s.split("T")[0]
        if (s.indexOf(" ") >= 0) return s.split(" ")[0]
        return s
    }

    function timePart(ts) {
        if (!ts || ts.length === 0) return "-"
        const s = String(ts)
        let raw = ""
        if (s.indexOf("T") >= 0) raw = s.split("T")[1]
        else if (s.indexOf(" ") >= 0) raw = s.split(" ")[1]
        else return "-"

        raw = raw.replace("Z", "")
        const plusIdx = raw.indexOf("+")
        if (plusIdx > 0) raw = raw.substring(0, plusIdx)
        const dotIdx = raw.indexOf(".")
        if (dotIdx > 0) raw = raw.substring(0, dotIdx)
        return raw.length > 0 ? raw : "-"
    }

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

        // Timestamp Search Box (Event Log style)
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
                    id: videoFilterTextField
                    Layout.fillWidth: true
                    placeholderText: "Filter by Timestamp..."
                    color: "white"
                    placeholderTextColor: "#99FFFFFF"
                    palette.text: "white"
                    palette.placeholderText: "#99FFFFFF"
                    font.pixelSize: 12
                    background: null
                    onTextChanged: {
                        videoFilterText = text
                    }
                }
            }
        }

        ListView {
            id: archiveListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: (videoFilterText && videoFilterText.trim() !== "") ? 0 : 4
            model: recordingListModel

            header: ColumnLayout {
                width: archiveListView.width
                spacing: 6

                RowLayout {
                    width: parent.width
                    spacing: 12
                    Text {
                        text: "DATE"
                        color: AppTheme.textSecondary
                        font.pixelSize: 11
                        font.bold: true
                        Layout.preferredWidth: 120
                    }
                    Text {
                        text: "TIME"
                        color: AppTheme.textSecondary
                        font.pixelSize: 11
                        font.bold: true
                        Layout.fillWidth: true
                    }
                    Text {
                        text: "ACTION"
                        color: AppTheme.textSecondary
                        font.pixelSize: 11
                        font.bold: true
                        horizontalAlignment: Text.AlignRight
                        Layout.preferredWidth: 92
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: AppTheme.borderCard
                }
            }

            delegate: Rectangle {
                width: archiveListView.width
                property bool isMatch: root._videoMatchesNeedle(createdAt, videoFilterText)
                height: isMatch ? 44 : 0
                color: rowMouseArea.containsMouse ? "#2a2a2a" : "transparent"
                radius: 6
                visible: isMatch

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 12

                    Text {
                        text: root.datePart(createdAt)
                        color: AppTheme.textPrimary
                        font.pixelSize: 12
                        Layout.preferredWidth: 120
                    }

                    Text {
                        text: root.timePart(createdAt)
                        color: AppTheme.textPrimary
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Button {
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: 88
                        flat: true
                        background: Rectangle {
                            color: "transparent"
                            border.color: "#4b5563"
                            border.width: 1
                            radius: 4
                        }
                        contentItem: Text {
                            text: "Run"
                            color: "white"
                            font.pixelSize: 12
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: {
                            archiveErrorMessage = ""
                            videoArchiveManager.requestPlayUrl(id)
                        }
                    }
                }

                MouseArea {
                    id: rowMouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }
            }

            footer: Item {
                width: archiveListView.width
                height: recordingListModel.count === 0 ? 54 : 0
                visible: recordingListModel.count === 0

                Text {
                    anchors.centerIn: parent
                    text: "No archived videos"
                    color: AppTheme.textSecondary
                    font.pixelSize: 12
                }
            }
        }

        MediaPlayer {
            id: mediaPlayer
            onErrorOccurred: function(error, errorString) {
                archiveErrorMessage = "PLAYER_ERROR: " + errorString
                console.warn("MediaPlayer error:", error, errorString, "source:", mediaPlayer.source)
            }
        }

        Popup {
            id: playbackPopup
            parent: ApplicationWindow.overlay
            modal: true
            focus: true
            anchors.centerIn: parent
            width: 1000
            height: 600
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            padding: 0

            background: Rectangle {
                color: AppTheme.surfaceCardAlt
                radius: 12
                border.color: AppTheme.borderCard
                border.width: 1
            }

            onClosed: {
                mediaPlayer.stop()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: "Incident Playback"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: currentPlaybackCreatedAt
                        color: AppTheme.textSecondary
                        font.pixelSize: 11
                    }

                    Button {
                        flat: true
                        implicitWidth: 30
                        implicitHeight: 30
                        background: null
                        contentItem: Text {
                            text: "✕"
                            color: "#9ca3af"
                            font.pixelSize: 18
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: playbackPopup.close()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: AppTheme.borderCard
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#111827"
                    radius: 8
                    clip: true

                    VideoOutput {
                        id: popupOutput
                        anchors.fill: parent
                    }
                }
            }

            onOpened: {
                mediaPlayer.videoOutput = popupOutput
            }
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
                currentPlaybackCreatedAt = createdAt
                mediaPlayer.source = url
                playbackPopup.open()
                mediaPlayer.play()
            }
        }
    }
}
