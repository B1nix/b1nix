// SPDX-License-Identifier: GPL-2.0-only
// What the installer shows while it copies: three facts, no marketing.
import QtQuick 2.0
import calamares.slideshow 1.0

Presentation {
    id: presentation

    function nextSlide() { presentation.goToNextSlide() }

    Timer {
        id: advanceTimer
        interval: 12000
        running: presentation.activatedInCalamares
        repeat: true
        onTriggered: nextSlide()
    }

    Slide {
        Text {
            anchors.centerIn: parent
            width: parent.width * 0.8
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 20
            text: "b1nix is Debian trixie on the b1nix kernel: Debian's packages and archive, our kernel and a small overlay."
        }
    }
    Slide {
        Text {
            anchors.centerIn: parent
            width: parent.width * 0.8
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 20
            text: "Two kernels stay installed. A kernel that fails to boot three times is replaced by the previous one automatically."
        }
    }
    Slide {
        Text {
            anchors.centerIn: parent
            width: parent.width * 0.8
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 20
            text: "The root is btrfs. snapper takes a snapshot before and after every apt run, so an upgrade can be undone."
        }
    }

    function onActivate() { presentation.currentSlide = 0 }
    function onLeave() { }
}
