import Foundation

let vendorID = 0xCAFE
let productID = 0xBAF3
let configVersion: UInt8 = 6
let configSize = 60
let configReportID: CFIndex = 100
let runtimeSize = 60
let runtimeReportID: CFIndex = 101
let configUsagePage = 0xFF00
let configUsage = 0x20
let runtimeUsagePage = 0xFF01
let runtimeUsage = 0x21
let fixed16Scale = 65536.0
let screenCount = 2
let unmappedPassthroughFlag: UInt8 = 0x01
let stickyMappingFlag: UInt8 = 0x01

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
    case setMouseConfig = 3
    case getMouseConfig = 4
}
