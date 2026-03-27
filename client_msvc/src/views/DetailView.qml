import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Item {
    id: root
    objectName: "detailViewRoot"
    signal closeClicked
    signal confirmClicked

    property string objectId: "3679"
    property string cardAgeText: "Adult"
    property string age: "Senior"
    property bool isFraud: true
    property string imagePath: ""
    property string cardAgeDisplay: cardAgeText && cardAgeText.trim() !== "" ? cardAgeText.toUpperCase() : "-"
    property string estimatedAgeDisplay: age && age.trim() !== "" ? age.toUpperCase() : "-"
    property string fraudDisplay: isFraud ? "FARE EVASION (Y)" : "NORMAL BOARDING (N)"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // LEFT: Incident Image Area
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.6
            color: "#111"
            
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                
                // Header inside image area
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: "#aa000000"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        Text {
                            text: "INCIDENT PLAYBACK"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 11
                            font.letterSpacing: 1
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "EVENT SNAPSHOT"
                            color: AppTheme.accent
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                // Image Placeholder
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    
                    // Actual fraud image
                    Image {
                        id: fraudImage
                        anchors.fill: parent
                        anchors.margins: 20
                        source: imagePath || ""
                        fillMode: Image.PreserveAspectFit
                        visible: imagePath !== "" && status === Image.Ready
                        onStatusChanged: {
                            if (status === Image.Error) {
                                console.warn("[DetailView] Failed to load fraud image:", source)
                            }
                        }
                    }
                    
                    // Fallback logo when no image available
                    Image {
                        anchors.fill: parent
                        anchors.margins: 20
                        source: "../../assets/video_Stream_logo.svg"
                        fillMode: Image.PreserveAspectFit
                        opacity: 0.15
                        visible: !fraudImage.visible
                    }
                    
                    // Face Detection Box - only show when image is loaded
                    Rectangle {
                        x: parent.width * 0.4
                        y: parent.height * 0.3
                        width: 120; height: 120
                        color: "transparent"
                        border.color: AppTheme.accent
                        border.width: 2
                        visible: fraudImage.visible
                        
                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top
                            width: 30; height: 30; color: AppTheme.accent
                            Text {
                                text: isFraud ? "Y" : "N"
                                anchors.centerIn: parent
                                color: "white"
                                font.bold: true; font.pixelSize: 10
                            }
                        }
                    }
                    
                    // Loading indicator when image is being loaded
                    BusyIndicator {
                        anchors.centerIn: parent
                        visible: fraudImage.visible && fraudImage.status === Image.Loading
                    }
                }
            }
        }

        // RIGHT: Data Area
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            color: AppTheme.surfaceCardAlt

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 24

                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 4
                        Text {
                            objectName: "detailTitleText"
                            text: "Event Details"
                            color: "white"
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Text {
                            objectName: "detailFraudTypeText"
                            text: isFraud ? "FARE EVASION DETECTED" : "BOARDING EVENT DETECTED"
                            color: AppTheme.accent
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "detailCloseButton"
                        flat: true
                        implicitWidth: 32; implicitHeight: 32
                        onClicked: closeClicked()
                        background: null
                        contentItem: Text {
                            text: "✕"
                            color: "#666"
                            font.pixelSize: 20
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: AppTheme.borderCard
                }

                // Data Grid
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Object ID"
                            color: AppTheme.textSecondary
                            font.pixelSize: 13
                            Layout.preferredWidth: 120
                        }
                        Text {
                            objectName: "detailObjectIdValueText"
                            text: objectId
                            color: "white"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Card Tag"
                            color: AppTheme.textSecondary
                            font.pixelSize: 13
                            Layout.preferredWidth: 120
                        }
                        Text {
                            objectName: "detailCardTagValueText"
                            text: cardAgeDisplay
                            color: "white"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Estimated Age"
                            color: AppTheme.textSecondary
                            font.pixelSize: 13
                            Layout.preferredWidth: 120
                        }
                        Text {
                            objectName: "detailEstimatedAgeValueText"
                            text: estimatedAgeDisplay
                            color: "white"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Boarding Result"
                            color: AppTheme.textSecondary
                            font.pixelSize: 13
                            Layout.preferredWidth: 120
                        }
                        Text {
                            objectName: "detailBoardingResultValueText"
                            text: fraudDisplay
                            color: "white"
                            font.pixelSize: 14
                            font.bold: true
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Actions
                Button {
                    id: confirmButton
                    objectName: "detailConfirmButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    background: Rectangle {
                        color: confirmButton.hovered ? AppTheme.accentHover : AppTheme.accent
                        radius: 8
                    }
                    contentItem: Text {
                        text: "Confirm"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        confirmClicked()
                        closeClicked()
                    }
                }
            }
        }
    }
}
