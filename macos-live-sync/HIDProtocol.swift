import Foundation

let vendorID = 0xCAFE
let productID = 0xBAF3
let configVersion: UInt8 = 9
let configSize = 60
let configReportID: CFIndex = 100
let runtimeSize = 60
let runtimeReportID: CFIndex = 101
let configUsagePage = 0xFF00
let configUsage = 0x20
let runtimeUsagePage = 0xFF01
let runtimeUsage = 0x21
let screenCoordinateScale = 1000.0
let screenCount = 2
let unmappedPassthroughFlag: UInt8 = 0x01
let stickyMappingFlag: UInt8 = 0x01
let switchScreenUsage: UInt32 = 0xFFF2_0001

enum ConfigCommand: UInt8 {
    case resetIntoBootsel = 1
    case setConfig = 2
    case getConfig = 3
    case clearMapping = 4
    case addMapping = 5
    case getMapping = 6
    case persistConfig = 7
    case getOurUsages = 8
    case getTheirUsages = 9
    case suspend = 10
    case resume = 11
    case setScreen = 12
    case getScreen = 13
}

enum RuntimeCommand: UInt8 {
    case getStatus = 1
    case setHostCursor = 2
    case getDiagnostics = 3
}
