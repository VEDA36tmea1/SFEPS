import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Window {
    id: root
    width: 1000
    height: 600
    visible: true
    title: "SFEPS Login"
    color: AppTheme.background

    signal loginSuccess

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ─── Header (CCTV Management Dashboard login-page.tsx 스타일)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            color: AppTheme.navBarBg
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: AppTheme.borderCard
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    color: AppTheme.primaryOrange
                    radius: 8

                    Image {
                        anchors.centerIn: parent
                        width: 28
                        height: 28
                        source: "../../assets/SFEPS_Logo_no_background.PNG"
                        fillMode: Image.PreserveAspectFit
                    }
                }
                ColumnLayout {
                    spacing: 0
                    Text {
                        text: "Hanwha Vision"
                        color: "white"
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: "SFEPS DASHBOARD"
                        color: "#9ca3af"
                        font.pixelSize: 12
                        font.letterSpacing: 1
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                // Status chip
                Rectangle {
                    Layout.preferredHeight: 28
                    implicitWidth: 160
                    color: "#22c55e1a" // green-500/10
                    radius: 999
                    RowLayout {
                        id: statusRow
                        anchors.centerIn: parent
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: AppTheme.statusOnline
                        }
                        Text {
                            text: "SYSTEM ONLINE"
                            color: AppTheme.statusOnline
                            font.pixelSize: 10
                            font.weight: Font.Medium
                            font.letterSpacing: 1
                        }
                    }
                }
                Button {
                    flat: true
                    implicitWidth: 35
                    implicitHeight: 35
                    background: Rectangle {
                        color: "transparent"
                    }
                    contentItem: Image {
                        source: "../../assets/question.png"
                        sourceSize: Qt.size(30, 30)
                        anchors.centerIn: parent
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 24
        }

        // ─── Login Card (CCTV Management Dashboard 스타일)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Item {
                Layout.fillWidth: true
            }
            Rectangle {
                Layout.preferredWidth: 380
                Layout.preferredHeight: 480
                color: AppTheme.surfaceCardAlt
                radius: 12
                border.color: AppTheme.borderCard
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 100
                        color: "transparent"
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: AppTheme.borderCard
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 4
                            Text {
                                text: "Secure Access"
                                color: "white"
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Text {
                                text: "Smart Fare Evasion Prevention"
                                color: "#9ca3af"
                                font.pixelSize: 12
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }

                    // Card body
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.leftMargin: 30
                        Layout.rightMargin: 30
                        Layout.topMargin: 20
                        Layout.bottomMargin: 20
                        spacing: 16

                        // Manager ID
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text {
                                text: "Manager ID"
                                color: "#dddddd"
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                color: AppTheme.inputBg
                                radius: 8
                                border.color: AppTheme.inputBorder
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 8
                                    Image {
                                        source: "../../assets/ID_logo.svg"
                                        sourceSize: Qt.size(18, 18)
                                        opacity: 0.6
                                    }
                                    TextField {
                                        id: idInputField
                                        objectName: "idInput"
                                        Layout.fillWidth: true
                                        placeholderText: "Enter your ID"
                                        color: "white"
                                        background: Item {}
                                        font.pixelSize: 13
                                    }
                                }
                            }
                        }

                        // Password
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            RowLayout {
                                Text {
                                    text: "Password"
                                    color: "#dddddd"
                                    font.pixelSize: 13
                                    font.bold: true
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Button {
                                    flat: true
                                    text: "Forgot Password ?"
                                    contentItem: Text {
                                        text: parent.text
                                        color: AppTheme.accent
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    background: Rectangle {
                                        color: "transparent"
                                    }
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                color: AppTheme.inputBg
                                radius: 8
                                border.color: AppTheme.inputBorder
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 8
                                    Image {
                                        source: "../../assets/Password.svg"
                                        sourceSize: Qt.size(18, 18)
                                        opacity: 0.6
                                    }
                                    TextField {
                                        id: passField
                                        Layout.fillWidth: true
                                        placeholderText: "••••••••"
                                        echoMode: showPass.checked ? TextInput.Normal : TextInput.Password
                                        color: "white"
                                        background: Item {}
                                        font.pixelSize: 13
                                    }
                                    Button {
                                        id: showPass
                                        checkable: true
                                        flat: true
                                        implicitWidth: 24
                                        implicitHeight: 24
                                        background: Rectangle {
                                            color: "transparent"
                                        }
                                        contentItem: Image {
                                            source: "../../assets/Eye.svg"
                                            sourceSize: Qt.size(20, 20)
                                            opacity: 0.6
                                        }
                                    }
                                }
                            }
                        }

                        CheckBox {
                            text: "Remember this device"
                            checked: false
                            contentItem: Text {
                                text: parent.text
                                color: "#888888"
                                font.pixelSize: 14
                                leftPadding: parent.indicator.width + parent.spacing
                                verticalAlignment: Text.AlignVCenter
                            }
                            indicator: Rectangle {
                                implicitWidth: 18
                                implicitHeight: 18
                                radius: 3
                                border.color: "#555555"
                                border.width: 2
                                color: parent.checked ? AppTheme.primaryOrange : "#222529"
                            }
                        }

                        // Error Message
                        Text {
                            id: errorText
                            visible: false
                            text: ""
                            color: "#ff4444"
                            font.pixelSize: 13
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            contentItem: RowLayout {
                                anchors.centerIn: parent
                                spacing: 8
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: "Login"
                                    color: "white"
                                    font.pixelSize: 14
                                    font.bold: true
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Image {
                                    source: "../../assets/log-in.svg"
                                    sourceSize: Qt.size(18, 18)
                                    Layout.alignment: Qt.AlignVCenter
                                    opacity: 0.9
                                }
                                Item { Layout.fillWidth: true }
                            }
                            background: Rectangle {
                                color: parent.hovered ? "#6e3512" : "#9c4a1b"
                                radius: 8
                            }
                            onClicked: {
                                errorText.visible = false;
                                authManager.login(idInputField.text, passField.text)
                            }
                        }

                        Connections {
                            target: authManager
                            function onLoginSuccess() {
                                root.loginSuccess()
                            }
                            function onLoginFailed(msg) {
                                errorText.text = msg
                                errorText.visible = true
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }
                        Text {
                            text: "SECURE 256-BIT ENCRYPTED CONNECTION"
                            color: "#555555"
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 1
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }

        // ─── Footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            color: AppTheme.background
            border.width: 1
            border.color: AppTheme.surface
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 40
                anchors.rightMargin: 40
                Text {
                    text: "© 2024 Hanwha Vision. All rights reserved."
                    color: "#555555"
                    font.pixelSize: 12
                }
                Item {
                    Layout.fillWidth: true
                }
                RowLayout {
                    spacing: 24
                    Text {
                        text: "Privacy Policy"
                        color: "#555555"
                        font.pixelSize: 12
                    }
                    Text {
                        text: "Terms of Service"
                        color: "#555555"
                        font.pixelSize: 12
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Text {
                    text: "Support"
                    color: "#888888"
                    font.pixelSize: 12
                    font.bold: true
                }
            }
        }
    }
}
