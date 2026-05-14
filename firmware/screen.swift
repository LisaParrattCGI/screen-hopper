#!/usr/bin/env swift
import AppKit
import CoreGraphics

let screens = NSScreen.screens

var union = CGRect.null
for screen in screens {
    union = union.union(screen.frame)
}

print("global desktop logical:", Int(union.width), "x", Int(union.height))

if let main = NSScreen.main {
    let frame = main.frame
    let scale = main.backingScaleFactor
    let physicalWidth = Int(frame.width * scale)
    let physicalHeight = Int(frame.height * scale)
    print("main display logical:", Int(frame.width), "x", Int(frame.height))
    print("main display scale:", scale)
    print("main display physical:", physicalWidth, "x", physicalHeight)
}

let mainID = CGMainDisplayID()
print("main display framebuffer:", CGDisplayPixelsWide(mainID), "x", CGDisplayPixelsHigh(mainID))
