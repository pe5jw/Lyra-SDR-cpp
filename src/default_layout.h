// Lyra — factory-default dock layout.
//
// The operator's curated panel arrangement, captured via
// QMainWindow::saveState() and base64-embedded so BOTH a fresh install
// AND "Reset to default layout" come up exactly this way — no reliance
// on whatever happens to be in QSettings.
//
// To regenerate after the panel set changes: arrange the docks the way
// the build should ship, View -> "Save current layout as my default",
// then base64-encode the registry value
//   HKCU\Software\N8SDR\Lyra-cpp\ui\userWindowState
// and paste it below (PowerShell:
//   [Convert]::ToBase64String((Get-ItemProperty
//     'HKCU:\Software\N8SDR\Lyra-cpp\ui').userWindowState) ).

#pragma once

#include <QByteArray>

namespace lyra::ui {

inline QByteArray defaultWindowState() {
    return QByteArray::fromBase64(QByteArrayLiteral(
        "QABCAHkAdABlAEEAcgByAGEAeQAoAAAAAAAAAP8AAAAAAAAAAAD9AAAAAAAAAAIA"
        "AAAAAAAAAgAAAAAACgAAAAAAAAAAAGsA/AABAAAAAAAAAAIA+wAAAAAAAAAIAAAA"
        "YgAAAGEAAABuAAAAZAABAAAAAAAAAAAAAAAAAAIA5QAAAAAAAACGAAAA/wD/AP8A"
        "+wAAAAAAAAAMAAAAdAAAAHUAAABuAAAAaQAAAG4AAABnAAEAAAAAAAIA6QAAAAAA"
        "BwAXAAAAAAAAAJUAAAD/AP8A/wAAAAAAAAADAAAAAAAKAAAAAAAAAAQAuQD8AAEA"
        "AAAAAAAAAQD8AAAAAAAAAAAAAAAAAAoAAAAAAAAAAQDtAAAA/wD/AP8A/AACAAAA"
        "AAAAAAIA/AAAAAAAAACgAAAAAAAAAHIAAAAAAAAAHAAAAP8A/wD/APwAAQAAAAAA"
        "AAADAPsAAAAAAAAACgAAAGEAAAB1AAAAZAAAAGkAAABvAAEAAAAAAAAAAAAAAAAA"
        "AwAnAAAAAAAAAIwAAAD/AP8A/wD7AAAAAAAAABQAAABtAAAAbwAAAGQAAABlAAAA"
        "ZgAAAGkAAABsAAAAdAAAAGUAAAByAAEAAAAAAAMAKwAAAAAAAQCBAAAAAAAAAMMA"
        "AAD/AP8A/wD7AAAAAAAAAA4AAABkAAAAaQAAAHMAAABwAAAAbAAAAGEAAAB5AAEA"
        "AAAAAAQAsAAAAAAABQBQAAAAAAAAAJYAAAD/AP8A/wD7AAAAAAAAABQAAABwAAAA"
        "YQAAAG4AAABhAAAAZAAAAGEAAABwAAAAdAAAAGUAAAByAAEAAAAAAAEAFgAAAAAA"
        "BABDAAAAAAAAABwAAAD/AP8A/wAAAAAACgAAAAAAAAAAAAAAAAAAAAAABAAAAAAA"
        "AAAEAAAAAAAAAAgAAAAAAAAACAD8AAAAAAAAAAEAAAAAAAAAAgAAAAAAAAABAAAA"
        "AAAAABgAAABtAAAAYQAAAGkAAABuAAAAXwAAAHQAAABvAAAAbwAAAGwAAABiAAAA"
        "YQAAAHIAAQAAAAAAAAAAAP8A/wD/AP8AAAAAAAAAAAAAAAAAAAAAACkA"));
}

} // namespace lyra::ui
