import QtQuick
import Quickshell

QtObject {
	property bool locked: false
	readonly property bool secure: false
	property Component surface: null

	signal unlock()

	onLockedChanged: {
		if (!locked) return;
		Quickshell.execDetached(["rundll32.exe", "user32.dll,LockWorkStation"]);
		Qt.callLater(() => { locked = false; });
	}
}
